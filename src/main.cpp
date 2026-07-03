// Flight Radar firmware for ESP32-2432S028R (CYD)
//
// Polls the adsb.lol public ADS-B aggregator for aircraft near a configured
// home location and renders them as a sonar-style radar on the built-in
// 320x240 ILI9341 display. WiFi + home location are set on first boot (or
// when BOOT is held at power-on) via a WiFiManager captive portal.

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include <math.h>

// ---------- Config persisted in NVS ----------
Preferences prefs;
double homeLat = 0.0;
double homeLon = 0.0;
float rangeKm = 50.0f;

// ---------- Display ----------
TFT_eSPI tft = TFT_eSPI();
TFT_eSprite frame = TFT_eSprite(&tft);

constexpr int SCREEN_W = 320;
constexpr int SCREEN_H = 240;
// Full-screen flat map centered on home; the configured range spans half the
// screen width. The sweep beam reaches the screen corners (sqrt(160^2+120^2)).
constexpr int RADAR_CX = SCREEN_W / 2;
constexpr int RADAR_CY = SCREEN_H / 2;
constexpr int SWEEP_LEN = 200;
constexpr int GRID_STEP = 40;

constexpr unsigned long POLL_INTERVAL_MS = 9000; // adsb.lol fair-use friendly

// Radar sweep: degrees per millisecond (90 deg/s = one revolution per 4s)
constexpr float SWEEP_DEG_PER_MS = 0.09f;

// ---------- Aircraft model ----------
struct Aircraft {
  String callsign;
  bool hasFlight;  // true if callsign is a real flight number (not hex fallback)
  String route;    // e.g. "HU -> DE", empty if unknown
  double lat, lon;
  bool hasAlt;
  float altM;      // meters, only valid if hasAlt
  bool onGround;
  float speedKmh;
  float trackDeg;
  float distanceKm;
  float bearingDeg;
};

constexpr int MAX_AIRCRAFT = 40;
// Shared between the fetch task (core 0) and the render loop (core 1);
// guard every access with aircraftMutex.
Aircraft aircraftList[MAX_AIRCRAFT];
int aircraftCount = 0;
SemaphoreHandle_t aircraftMutex;

bool wifiPortalActive = false;

// ---------- Route cache (callsign -> "HU -> DE") ----------
// Only touched from the fetch task, so no locking needed. An entry with an
// empty route means "looked up, route unknown" so we don't re-query forever.
struct RouteCacheEntry {
  String callsign;
  String route;
};
constexpr int ROUTE_CACHE_SIZE = 64;
RouteCacheEntry routeCache[ROUTE_CACHE_SIZE];
int routeCacheNext = 0;

RouteCacheEntry *routeCacheFind(const String &cs) {
  for (int i = 0; i < ROUTE_CACHE_SIZE; i++) {
    if (routeCache[i].callsign == cs && cs.length()) return &routeCache[i];
  }
  return nullptr;
}

void routeCacheStore(const String &cs, const String &route) {
  RouteCacheEntry *e = routeCacheFind(cs);
  if (!e) {
    e = &routeCache[routeCacheNext];
    routeCacheNext = (routeCacheNext + 1) % ROUTE_CACHE_SIZE;
    e->callsign = cs;
  }
  e->route = route;
}

// ---------- Geo math ----------
double toRad(double deg) { return deg * PI / 180.0; }

// Great-circle distance in km (haversine)
double haversineKm(double lat1, double lon1, double lat2, double lon2) {
  const double R = 6371.0;
  double dLat = toRad(lat2 - lat1);
  double dLon = toRad(lon2 - lon1);
  double a = sin(dLat / 2) * sin(dLat / 2) +
             cos(toRad(lat1)) * cos(toRad(lat2)) * sin(dLon / 2) * sin(dLon / 2);
  double c = 2 * atan2(sqrt(a), sqrt(1 - a));
  return R * c;
}

// Initial bearing in degrees, 0 = north, clockwise
double bearingDeg(double lat1, double lon1, double lat2, double lon2) {
  double phi1 = toRad(lat1), phi2 = toRad(lat2);
  double dLon = toRad(lon2 - lon1);
  double y = sin(dLon) * cos(phi2);
  double x = cos(phi1) * sin(phi2) - sin(phi1) * cos(phi2) * cos(dLon);
  double brng = atan2(y, x) * 180.0 / PI;
  return fmod(brng + 360.0, 360.0);
}

// ---------- Preferences ----------
void loadConfig() {
  prefs.begin("flightradar", true);
  homeLat = prefs.getDouble("lat", 0.0);
  homeLon = prefs.getDouble("lon", 0.0);
  rangeKm = prefs.getFloat("range", 50.0f);
  prefs.end();
}

void saveConfig(double lat, double lon, float range) {
  prefs.begin("flightradar", false);
  prefs.putDouble("lat", lat);
  prefs.putDouble("lon", lon);
  prefs.putFloat("range", range);
  prefs.end();
}

// ---------- WiFiManager custom params + portal ----------
char latBuf[16];
char lonBuf[16];
char rangeBuf[8];
bool shouldSaveParams = false;

void saveParamsCallback() { shouldSaveParams = true; }

void drawStatus(const char *line1, const char *line2 = nullptr) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(line1, SCREEN_W / 2, SCREEN_H / 2 - 10, 4);
  if (line2) {
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString(line2, SCREEN_W / 2, SCREEN_H / 2 + 20, 2);
  }
}

void runWiFiSetup(bool forcePortal) {
  wifiPortalActive = true;
  drawStatus("Flight Radar", "Connect phone to WiFi: FlightRadar-Setup");

  WiFiManager wm;

  snprintf(latBuf, sizeof(latBuf), "%.6f", homeLat);
  snprintf(lonBuf, sizeof(lonBuf), "%.6f", homeLon);
  snprintf(rangeBuf, sizeof(rangeBuf), "%.0f", rangeKm);

  WiFiManagerParameter paramLat("lat", "Home latitude (e.g. 47.4979)", latBuf, sizeof(latBuf));
  WiFiManagerParameter paramLon("lon", "Home longitude (e.g. 19.0402)", lonBuf, sizeof(lonBuf));
  WiFiManagerParameter paramRange("range", "Range in km (e.g. 50)", rangeBuf, sizeof(rangeBuf));

  wm.addParameter(&paramLat);
  wm.addParameter(&paramLon);
  wm.addParameter(&paramRange);
  wm.setSaveParamsCallback(saveParamsCallback);
  wm.setBreakAfterConfig(true);

  bool connected;
  if (forcePortal) {
    wm.setConfigPortalTimeout(180); // fall back to last-known config if untouched
    connected = wm.startConfigPortal("FlightRadar-Setup");
  } else {
    wm.setConfigPortalTimeout(0); // no saved creds yet: wait indefinitely for setup
    connected = wm.autoConnect("FlightRadar-Setup");
  }

  if (shouldSaveParams) {
    double lat = atof(paramLat.getValue());
    double lon = atof(paramLon.getValue());
    float range = atof(paramRange.getValue());
    if (range < 5) range = 5;
    saveConfig(lat, lon, range);
    homeLat = lat;
    homeLon = lon;
    rangeKm = range;
    shouldSaveParams = false;
  }

  wifiPortalActive = false;

  if (!connected) {
    drawStatus("WiFi setup timed out", "Retrying...");
    delay(2000);
  }
}

// ---------- ADS-B fetch ----------
bool fetchAircraft() {
  if (WiFi.status() != WL_CONNECTED) return false;

  float radiusNm = rangeKm * 0.539957f;
  if (radiusNm > 250) radiusNm = 250;

  char url[160];
  snprintf(url, sizeof(url), "https://api.adsb.lol/v2/point/%.6f/%.6f/%.0f",
           homeLat, homeLon, radiusNm);

  // adsb.lol is HTTPS-only; HTTPClient::begin(url) without a TLS client
  // fails silently on ESP32, so pass an insecure (no cert check) client.
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.begin(client, url);
  http.setTimeout(8000);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("fetchAircraft: HTTP %d\n", code);
    http.end();
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) return false;

  // Parse into a private buffer first; publish under the mutex at the end so
  // the render loop never sees a half-written list.
  static Aircraft parsed[MAX_AIRCRAFT];
  int parsedCount = 0;

  JsonArray ac = doc["ac"].as<JsonArray>();
  for (JsonObject a : ac) {
    if (parsedCount >= MAX_AIRCRAFT) break;
    if (!a["lat"].is<float>() || !a["lon"].is<float>()) continue;

    Aircraft &out = parsed[parsedCount];
    out.lat = a["lat"].as<double>();
    out.lon = a["lon"].as<double>();

    const char *flight = a["flight"] | "";
    out.callsign = String(flight);
    out.callsign.trim();
    out.hasFlight = out.callsign.length() > 0;
    if (!out.hasFlight) {
      const char *hex = a["hex"] | "?????";
      out.callsign = String(hex);
    }

    RouteCacheEntry *cached = out.hasFlight ? routeCacheFind(out.callsign) : nullptr;
    out.route = cached ? cached->route : "";

    out.onGround = false;
    out.hasAlt = false;
    if (a["alt_baro"].is<float>()) {
      out.hasAlt = true;
      out.altM = a["alt_baro"].as<float>() * 0.3048f;
    } else if (a["alt_baro"].is<const char *>()) {
      out.onGround = true; // adsb.lol reports "ground" as a string here
    }

    out.speedKmh = (a["gs"] | 0.0f) * 1.852f;
    out.trackDeg = a["track"] | 0.0f;

    out.distanceKm = haversineKm(homeLat, homeLon, out.lat, out.lon);
    out.bearingDeg = bearingDeg(homeLat, homeLon, out.lat, out.lon);

    if (out.distanceKm <= rangeKm) {
      parsedCount++;
    }
  }

  xSemaphoreTake(aircraftMutex, portMAX_DELAY);
  for (int i = 0; i < parsedCount; i++) aircraftList[i] = parsed[i];
  aircraftCount = parsedCount;
  xSemaphoreGive(aircraftMutex);

  Serial.printf("fetchAircraft: %d aircraft in range\n", parsedCount);
  return true;
}

// Look up origin/destination countries for aircraft whose callsign isn't in
// the route cache yet, via api.adsbdb.com (one GET per callsign, max 4 per
// poll cycle to stay polite; the rest catch up on later cycles).
void fetchRoutes() {
  String pending[4];
  int pendingCount = 0;

  xSemaphoreTake(aircraftMutex, portMAX_DELAY);
  for (int i = 0; i < aircraftCount && pendingCount < 4; i++) {
    Aircraft &a = aircraftList[i];
    if (a.hasFlight && !routeCacheFind(a.callsign)) {
      pending[pendingCount++] = a.callsign;
    }
  }
  xSemaphoreGive(aircraftMutex);
  if (pendingCount == 0) return;

  for (int i = 0; i < pendingCount; i++) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    String url = "https://api.adsbdb.com/v0/callsign/" + pending[i];
    http.begin(client, url);
    http.setTimeout(8000);
    int code = http.GET();
    // 404 = "unknown callsign": cache as known-unknown so we stop asking.
    // Other errors: skip, so the callsign is retried next cycle.
    if (code != HTTP_CODE_OK && code != HTTP_CODE_NOT_FOUND) {
      Serial.printf("fetchRoutes: %s HTTP %d\n", pending[i].c_str(), code);
      http.end();
      continue;
    }
    String resp = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, resp) != DeserializationError::Ok) continue;

    String route = "";
    JsonObject fr = doc["response"]["flightroute"];
    const char *from = fr["origin"]["country_iso_name"] | "";
    const char *to = fr["destination"]["country_iso_name"] | "";
    if (from[0] && to[0]) {
      route = String(from) + " -> " + String(to);
    }
    Serial.printf("fetchRoutes: %s = '%s'\n", pending[i].c_str(), route.c_str());
    routeCacheStore(pending[i], route); // empty = known-unknown, don't re-query
  }

  // Push fresh routes into the live list so they show without waiting a poll
  xSemaphoreTake(aircraftMutex, portMAX_DELAY);
  for (int i = 0; i < aircraftCount; i++) {
    Aircraft &a = aircraftList[i];
    if (a.hasFlight && a.route.length() == 0) {
      RouteCacheEntry *e = routeCacheFind(a.callsign);
      if (e) a.route = e->route;
    }
  }
  xSemaphoreGive(aircraftMutex);
}

// ---------- Rendering ----------
// Flat full-screen map: the configured range spans half the screen width,
// uniform scale in both axes (far north/south planes may fall off-screen).
void polarToScreen(float bearing, float distKm, int &x, int &y) {
  float scale = (SCREEN_W / 2.0f) / rangeKm;
  float rad = bearing * PI / 180.0f;
  x = RADAR_CX + (int)(distKm * scale * sin(rad));
  y = RADAR_CY - (int)(distKm * scale * cos(rad));
}

// Small airplane silhouette (fuselage + swept wings + tail), nose pointing
// north by default, rotated to the aircraft's track.
void drawPlaneMarker(int x, int y, float trackDeg, uint16_t color) {
  float rad = trackDeg * PI / 180.0f;
  float c = cos(rad), s = sin(rad);
  auto rx = [&](float px, float py) { return (int16_t)lroundf(x + px * c - py * s); };
  auto ry = [&](float px, float py) { return (int16_t)lroundf(y + px * s + py * c); };

  // Fuselage: slim triangle nose->rear
  frame.fillTriangle(rx(0, -8), ry(0, -8), rx(-2, 6), ry(-2, 6), rx(2, 6), ry(2, 6), color);
  // Main wings, swept back
  frame.fillTriangle(rx(0, -3), ry(0, -3), rx(-8, 3), ry(-8, 3), rx(8, 3), ry(8, 3), color);
  // Tail wings
  frame.fillTriangle(rx(0, 3), ry(0, 3), rx(-4, 8), ry(-4, 8), rx(4, 8), ry(4, 8), color);
}

void renderRadar(float sweepDeg) {
  frame.fillSprite(TFT_BLACK);

  // Subtle full-screen rectangular grid, brighter crosshair through home
  uint16_t gridCol = frame.color565(0, 70, 0);
  for (int gx = RADAR_CX % GRID_STEP; gx < SCREEN_W; gx += GRID_STEP)
    frame.drawFastVLine(gx, 0, SCREEN_H, gridCol);
  for (int gy = RADAR_CY % GRID_STEP; gy < SCREEN_H; gy += GRID_STEP)
    frame.drawFastHLine(0, gy, SCREEN_W, gridCol);
  uint16_t axisCol = frame.color565(0, 120, 0);
  frame.drawFastVLine(RADAR_CX, 0, SCREEN_H, axisCol);
  frame.drawFastHLine(0, RADAR_CY, SCREEN_W, axisCol);

  // Rotating sweep beam with a fading afterglow trail behind it; long enough
  // to reach the screen corners, the sprite clips the rest.
  for (int k = 40; k >= 0; k--) {
    float ang = (sweepDeg - k * 1.5f) * PI / 180.0f;
    uint8_t g = 230 - k * 5; // brightest at the beam, fading behind it
    uint16_t col = frame.color565(0, g, 0);
    int ex = RADAR_CX + (int)(SWEEP_LEN * sin(ang));
    int ey = RADAR_CY - (int)(SWEEP_LEN * cos(ang));
    frame.drawLine(RADAR_CX, RADAR_CY, ex, ey, col);
  }

  // Compass labels at the screen edges
  frame.setTextColor(TFT_GREEN);
  frame.setTextDatum(MC_DATUM);
  frame.drawString("N", RADAR_CX, 8, 2);
  frame.drawString("S", RADAR_CX, SCREEN_H - 9, 2);
  frame.drawString("E", SCREEN_W - 8, RADAR_CY, 2);
  frame.drawString("W", 8, RADAR_CY, 2);

  // Range scale note in the corner (range = center to left/right edge)
  char buf[16];
  snprintf(buf, sizeof(buf), "%.0fkm", rangeKm);
  frame.setTextColor(TFT_CYAN);
  frame.setTextDatum(BR_DATUM);
  frame.drawString(buf, SCREEN_W - 3, SCREEN_H - 3, 1);

  // Home position
  frame.fillCircle(RADAR_CX, RADAR_CY, 3, TFT_RED);

  // Aircraft: icon with callsign + altitude labels underneath. Planes the
  // beam has just swept over flare up bright with a green glow, then relax
  // back to yellow as the beam moves on.
  xSemaphoreTake(aircraftMutex, portMAX_DELAY);
  for (int i = 0; i < aircraftCount; i++) {
    Aircraft &a = aircraftList[i];
    int x, y;
    polarToScreen(a.bearingDeg, a.distanceKm, x, y);

    float lag = fmodf(sweepDeg - a.bearingDeg + 360.0f, 360.0f);
    bool hot = lag < 30.0f;
    if (hot) {
      uint8_t glow = (uint8_t)(120 * (1.0f - lag / 30.0f));
      frame.fillCircle(x, y, 13, frame.color565(0, glow, 0));
    }
    drawPlaneMarker(x, y, a.trackDeg, hot ? TFT_WHITE : TFT_YELLOW);

    char altStr[16];
    if (a.onGround) {
      snprintf(altStr, sizeof(altStr), "GND");
    } else if (a.hasAlt) {
      snprintf(altStr, sizeof(altStr), "%.0f m", a.altM);
    } else {
      altStr[0] = '\0';
    }

    frame.setTextDatum(TC_DATUM);
    frame.setTextColor(TFT_WHITE);
    frame.drawString(a.callsign.c_str(), x, y + 11, 1);
    int labelY = y + 20;
    if (altStr[0]) {
      frame.setTextColor(TFT_YELLOW);
      frame.drawString(altStr, x, labelY, 1);
      labelY += 9;
    }
    if (a.speedKmh > 1.0f) {
      snprintf(buf, sizeof(buf), "%.0f km/h", a.speedKmh);
      frame.setTextColor(TFT_CYAN);
      frame.drawString(buf, x, labelY, 1);
      labelY += 9;
    }
    // Route countries appear when the sweep hits the plane and linger for a
    // third of a revolution (~1.3s) so they're actually readable
    if (lag < 120.0f && a.route.length()) {
      frame.setTextColor(TFT_GREENYELLOW);
      frame.drawString(a.route.c_str(), x, labelY, 1);
    }
  }
  xSemaphoreGive(aircraftMutex);

  frame.pushSprite(0, 0);
}

// Runs on core 0 so the HTTPS fetch (1-2s) never stalls the sweep animation,
// which renders from loop() on core 1.
void fetchTask(void *) {
  for (;;) {
    if (!wifiPortalActive && WiFi.status() == WL_CONNECTED) {
      if (fetchAircraft()) fetchRoutes();
    }
    vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(0, INPUT_PULLUP); // BOOT button

  tft.init();
  // This board's TPM408-2.8 panel decodes the MADCTL orientation bits
  // non-standardly: TFT_eSPI's own rotation-1 MADCTL renders transposed and
  // wrapped. setRotation(1) is still needed for the library's 320x240 logical
  // frame, but the hardware register must then be overridden with raw 0x80
  // (found by brute-force diagnostic, 2026-07-03 — see DISPLAY_DEBUG_NOTES.md).
  // In this state geometry, colors, and channel order are all correct with no
  // inversion. If setRotation() is ever called again, re-apply the override.
  tft.setRotation(1);
  tft.writecommand(TFT_MADCTL);
  tft.writedata(0x80);
  tft.invertDisplay(false);
  tft.fillScreen(TFT_BLACK);

  // 8-bit color: a 16-bit 320x240 sprite needs 153KB of contiguous heap,
  // which the ESP32 cannot provide (largest block ~110KB) — createSprite
  // fails silently and the screen never updates. 8-bit halves it to 76.8KB.
  frame.setColorDepth(8);
  if (frame.createSprite(SCREEN_W, SCREEN_H) == nullptr) {
    Serial.printf("createSprite FAILED, largest free block: %u\n",
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    drawStatus("Display buffer error", "Out of memory");
    while (true) delay(1000);
  }

  loadConfig();

  bool forcePortal = (digitalRead(0) == LOW); // BOOT held at power-on
  runWiFiSetup(forcePortal);

  aircraftMutex = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(fetchTask, "fetch", 12288, nullptr, 1, nullptr, 0);
}

void loop() {
  if (wifiPortalActive) return;

  if (WiFi.status() != WL_CONNECTED) {
    drawStatus("WiFi disconnected", "Reconnecting...");
    WiFi.reconnect();
    delay(3000);
    return;
  }

  renderRadar(fmodf(millis() * SWEEP_DEG_PER_MS, 360.0f));
  delay(20); // sprite push dominates; yields ~15-20 fps sweep
}
