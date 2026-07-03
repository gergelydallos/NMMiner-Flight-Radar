# NMMiner Flight Radar

Turn a cheap ESP32 "Cheap Yellow Display" board (the same hardware sold as
NMMiner/NerdMiner solo-mining gadgets) into an old-school sweep radar that
shows **live air traffic around your home** — no receiver antenna, no
soldering, no account, no API keys.

![Flight radar demo](docs/demo.gif)

A green radar beam sweeps the screen once every 4 seconds. Aircraft within
your configured range appear as little airplane silhouettes pointing in their
real direction of flight, each with its callsign, altitude, and speed. When
the beam passes over a plane it flares up and briefly reveals the flight's
origin and destination countries (e.g. `TR -> DE`).

## Features

- 🛩️ Live aircraft positions from the free [adsb.lol](https://adsb.lol) API
  (community-run ADS-B aggregator, no key needed)
- 🌍 Origin/destination country lookup via [adsbdb.com](https://www.adsbdb.com),
  cached per callsign
- 📡 Old-school rotating sweep with fading afterglow; planes flare up as the
  beam catches them
- 🗺️ Full-screen flat map, subtle grid, home marker, compass directions
- 📱 First-run setup from your phone via a WiFi captive portal — WiFi
  credentials, home coordinates, and radar range, all stored in flash
- ⚙️ Runs fetching on one CPU core and rendering on the other, so the
  animation never stutters during network requests

## Hardware

One board, nothing else:

| Part | Notes |
|------|-------|
| ESP32-2432S028R ("Cheap Yellow Display", CYD) | 2.8" 320x240 ILI9341-family SPI TFT, ESP32-WROOM-32, USB powered. Sold under many names, including NMMiner/NerdMiner miner kits. |

> **Note on display variants:** these boards ship with several panel clones.
> This project was built on a unit with a **TPM408-2.8** panel whose MADCTL
> register decodes non-standardly; the firmware works around it by overriding
> the rotation register directly (`tft.writecommand(TFT_MADCTL)` +
> `tft.writedata(0x80)` right after `tft.setRotation(1)` in `setup()`).
> On a standard CYD panel you can likely just delete those two override
> lines. If your screen shows mirrored, rotated, or wrapped output with some
> other panel clone, try the other MADCTL orientation bytes in place of
> `0x80` (0x00, 0x20, 0x40, 0x60, 0xA0, 0xC0, 0xE0) until the picture is
> correct — a small sketch that cycles them on a button press makes this a
> two-minute job.

## Building & flashing

1. Install [PlatformIO](https://platformio.org/) (`pip install platformio`).
2. Clone this repo and connect the board via USB (CH340 serial — most
   systems have the driver already).
3. Build: `pio run`
4. Put the board in bootloader mode — on many of these boards the auto-reset
   circuit doesn't work, so do it manually: **hold BOOT, tap RST while still
   holding BOOT, release both.**
5. Flash: `pio run -t upload --upload-port <YOUR_COM_PORT>`

`platformio.ini` already contains `board_upload.before_reset = no_reset` and a
conservative 115200 upload speed to cope with these boards' quirks.

## First-run setup

1. Power the board. It shows **"Connect phone to WiFi: FlightRadar-Setup"**.
2. Join the open `FlightRadar-Setup` network with your phone (choose "stay
   connected" if it warns about no internet). A captive portal opens — if
   not, browse to `192.168.4.1`.
3. Tap **Configure WiFi**, choose your network, and fill in:
   - your home **latitude** and **longitude** (decimal degrees),
   - the radar **range** in km (range = center of the screen to its left/right edge).
4. Save. The radar starts within seconds.

To change settings later: tap RST, then immediately press and hold BOOT for
~3 seconds — the setup portal reopens for 3 minutes.

## How it works

- Polls `api.adsb.lol/v2/point/{lat}/{lon}/{radius}` every 9 seconds from a
  FreeRTOS task pinned to core 0.
- Looks up flight routes on `api.adsbdb.com/v0/callsign/{callsign}` (max 4
  new callsigns per cycle) and caches results, including negative ones.
- Renders ~15–20 fps into an 8-bit off-screen sprite pushed over 40 MHz SPI
  from the Arduino loop on core 1; the aircraft list is shared behind a
  mutex.

## Credits

- Aircraft data: [adsb.lol](https://adsb.lol) — a community ADS-B aggregator.
  Please be fair to their free API (this project polls every 9 s).
- Route data: [adsbdb.com](https://www.adsbdb.com)
- Display driver: [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI),
  WiFi onboarding: [WiFiManager](https://github.com/tzapu/WiFiManager),
  JSON: [ArduinoJson](https://arduinojson.org/)

## License

[MIT](LICENSE)
