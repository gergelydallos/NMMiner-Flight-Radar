# Flight Radar Firmware — Display Debug Handoff (paused 2026-07-02)

## ✅ RESOLVED 2026-07-03 — read this, ignore the theories below

The panel (TPM408-2.8 clone) decodes the MADCTL orientation bits
**non-standardly**: identical renders were photographed for different MADCTL
values and different renders for the same window shape, so no TFT_eSPI
rotation value can ever be correct on its own. It also is NOT an ILI9342 —
the M5STACK define was tried and made things worse.

**Working config (verified by photo, geometry + colors + channel order all
correct):**

```cpp
tft.init();
tft.setRotation(1);            // for TFT_eSPI's 320x240 logical frame only
tft.writecommand(TFT_MADCTL);  // then override the panel register directly
tft.writedata(0x80);
tft.invertDisplay(false);      // panel needs NO inversion
```

Found by a brute-force diagnostic cycling all 8 raw MADCTL bytes x 2 window
shapes. `include/User_Setup.h` keeps `ILI9341_2_DRIVER`, native 240x320 dims,
`TFT_RGB_ORDER TFT_RGB`, SPI at 40MHz (55MHz suspected in earlier ghost
artifacts, untested since). The old "inv=ON is correct" conclusion was a
false lead (the old test sketch swapped its own palette when inverting).
If `setRotation()` is ever called again at runtime, the raw 0x80 override
must be re-applied afterwards. Real firmware restored to `src/main.cpp` with
this fix; `main.cpp.bak` deleted.

---

Historical notes from 2026-07-02 follow (superseded):

## Where things stand

The overall firmware (WiFi captive portal + adsb.lol polling + radar rendering)
is written and builds cleanly — see `src/main.cpp.bak`. The ONLY unresolved
problem is getting the display to show content right-side-up, unmirrored, and
without wraparound artifacts on this specific physical panel. This has taken
many iterations without success — see "What we tried" below before repeating
any of it.

**Current file state (as of stopping):**
- `src/main.cpp` currently holds a **diagnostic sketch**, NOT the real
  firmware. It draws corner labels (TOP-LEFT/TOP-RIGHT/BOT-LEFT/BOT-RIGHT) and
  a `rot=X inv=Y` readout, and cycles through rotation values on each BOOT
  button press.
- `src/main.cpp.bak` is the real flight-radar firmware, untouched, ready to
  restore once the display config is solved. **Restore this before doing
  anything else**: `mv src/main.cpp.bak src/main.cpp` (or copy over it).
- `include/User_Setup.h` currently has: `ILI9341_2_DRIVER`, `TFT_WIDTH 240`,
  `TFT_HEIGHT 320` (native/original values).
- The board right now is flashed with the diagnostic sketch, mid-test, showing
  garbled/wrong output at every rotation tried so far.

## Hardware facts (confirmed, don't re-derive)

- Board: ESP32-2432S028R "Cheap Yellow Display" clone, sold under an
  NMMiner/NerdMiner-style solo-mining product listing. Panel is printed with
  the label **"TPM408-2.8"** — confirmed via web search to be a genuine
  **ILI9341** driver, 240x320 native controller resolution, SPI, resistive
  touch (touch not used in this project).
- USB-serial chip: CH340 (COM10 on this PC). Windows already has the driver;
  no install needed.
- The physical glass is **landscape-shaped** (wider than tall) — confirmed
  directly by the user, not inferred.
- **The BOOT/RST auto-reset circuit does NOT reliably enter bootloader mode**
  via esptool's normal DTR/RTS toggle. Manual entry is required every time:
  hold BOOT, tap RST (while still holding BOOT), then release both. This is
  needed before every single flash (erase and upload both).
- PlatformIO's upload also needs `board_upload.before_reset = no_reset` in
  `platformio.ini` (already set) — otherwise PlatformIO's own reset toggle
  kicks the chip back out of bootloader before esptool can talk to it. This
  fix is already applied and confirmed working — don't remove it.
- Toolchain: PlatformIO Core installed via `pip install platformio` (already
  done, works). ESP-IDF also happens to be installed separately at
  `C:\esp\esp-idf` (source `export.ps1` if you want `idf.py`/`esptool.py`
  directly in PowerShell) but PlatformIO does not need it.

## What we tried, and what happened (don't repeat these)

1. **Original config**: `ILI9341_2_DRIVER`, 240x320, `rotation(1)`.
   Colors were inverted (green text showed as purple, black background as
   white) but user did not initially notice/report an orientation problem.
   *(Later, re-testing this exact combo with a rigorous corner-label test
   DID show wraparound — see point 4. The original "orientation was fine"
   impression was likely wrong; the first screen only had centered text that
   didn't reveal the bug.)*

2. **Switched to `ILI9341_DRIVER`** (thinking it'd fix colors, since the two
   variants sometimes differ). Colors did become correct, but orientation
   broke (became portrait rendering instead of landscape). Reverted this —
   **both driver variants use the identical rotation code path**
   (`TFT_Drivers/ILI9341_Rotation.h` in the TFT_eSPI lib, confirmed by reading
   the library source), so driver choice should NOT affect rotation. This
   result is confusing/unexplained and possibly coincidental with another
   change made at the same time.

3. **Tried `tft.invertDisplay(true)`** as a pure software color fix on top of
   the original driver/rotation. This is a color-only call and shouldn't
   touch orientation, but user reported orientation was still wrong after
   this change too.

4. **Built a proper diagnostic sketch** (now living in `src/main.cpp`) that
   draws 4 corner labels + a live rotation/invert readout, changeable via BOOT
   press, so we could actually SEE what each combination does instead of
   guessing blind. Key photographed findings:
   - **`rotation(2)`, `inv=ON`, dims 240x320**: colors looked genuinely
     correct (dark background, white/cyan text, readable) — inversion fix
     works. But rotation=2 is portrait (240x320) and showed a duplicated,
     90°-rotated ghost of "TOP-LEFT" along one edge — a wraparound artifact.
   - **`rotation(3)`, `inv=ON`, dims reported 320x240 (landscape)**: still
     showed the same wraparound/duplication artifact, just rotated. This is
     the first strong evidence that **rotations using the MV bit (1, 3, 5, 7
     — i.e. anything that produces a 320-wide landscape framebuffer) cause a
     GRAM addressing wraparound on this specific panel**, independent of
     which driver macro or header dimensions are used.
   - **Swapped header to `TFT_WIDTH 320 / TFT_HEIGHT 240`, `rotation(0)`**:
     no more wraparound (clean signal!), but output was upside-down and
     mirrored.
   - **Same swapped header, `rotation(4)`** (MX+MY, no MV — meant to be a
     clean 180° flip per the TFT_eSPI driver table): user described output as
     still mirrored/wrong, not a clean fix.
   - **Reverted to exact original (`ILI9341_2_DRIVER`, 240x320, `rotation(1)`,
     `inv=ON`)** for a clean one-variable-at-a-time re-test: this ALSO showed
     the wraparound/duplicate-text artifact clearly in a photo. This
     contradicts the very first impression that this combo was fine — it
     wasn't; the bug was just not obvious in the very first (simple
     centered-text) screen.

   **Working conclusion**: any rotation value that relies on the MV bit
   (values 1, 3, 5, 7 in TFT_eSPI's rotation table) causes GRAM
   wraparound/corruption on this specific panel unit. This is very likely
   because MV is supposed to remap the addressable CASET/PASET range from
   240x320 to 320x240, and that remap appears broken/incomplete on this
   particular clone — a real hardware/controller quirk, not a config
   mistake.

5. Started a follow-up diagnostic (already flashed, not yet evaluated)
   restricting the button-cycle to only the non-MV rotation values (0, 2, 4,
   6) with header dims back to native 240x320, to find which of those 4 gives
   clean, non-mirrored output as a "base" orientation. **User paused here**,
   reporting the pixels/resolution look "not even correct" at this stage
   too — i.e. even the non-MV rotations may have further problems beyond
   simple mirroring (possibly a GRAM offset/resolution issue — see "New
   symptom" below). This is where we left off.

## New symptom to investigate first tomorrow

User's last message: **"all of them are shit, i dont even think the pixels
are correct... the resolution is also incorrect."** This suggests something
beyond simple rotation/mirroring — possibly:
- A **GRAM column/row offset** issue common on some ILI9341 clones with a
  smaller active area than the full 240x320 controller memory (would need
  `tft.setAddrWindow` offset correction, or `TFT_eSPI` panel-specific offset
  defines).
- The panel might not actually be a full 240x320 active area — worth
  measuring/counting visible pixels or checking for black bars/cut content in
  the corner-label diagnostic photos already taken.
- Possibly worth re-examining the existing photos (already in this
  conversation) with fresh eyes for evidence of cropping, stretching, or
  offset, not just rotation/mirroring.

## Recommended next steps for tomorrow

1. **Don't immediately keep cycling rotation values.** Re-look at the photos
   already taken (rot=2 inv=ON, rot=3 inv=ON, swapped-dims rot=0, swapped-dims
   rot=4, and the final rot=1 retest) with the "is the resolution/pixel count
   right" question specifically in mind, since that's the new, more
   fundamental concern raised.
2. Consider trying explicit **GRAM offset defines** for TFT_eSPI (some ILI9341
   clones need `#define TFT_INVERSION_ON` alone, others need a `colstart`/
   `rowstart` adjustment — TFT_eSPI doesn't expose this for ILI9341 by default
   the way it does for ST7735, so this may require a small patch to
   `TFT_Drivers/ILI9341_Rotation.h` or a custom `setAddrWindow` override if it
   turns out to be a real offset problem).
3. Alternative strategy considered but not yet tried: since MV-based
   (hardware) landscape rotation seems broken, render the UI in whichever
   portrait orientation (0/2/4/6) turns out clean and correct, then use
   `TFT_eSprite::pushRotated()` to rotate the finished frame into landscape
   **in software**, bypassing the hardware MV bit entirely. This is the
   most promising path if the "resolution incorrect" issue turns out to
   just be a distraction/misperception and the base non-MV orientations are
   otherwise clean.
4. If pixel geometry really is wrong even in native portrait mode, it's worth
   trying the plain `ILI9341_DRIVER` macro (not `_2`) combined with the
   corner-label diagnostic again, since driver variant differences in the
   TFT_eSPI init sequence (not just the shared rotation code) could plausibly
   affect GRAM addressing even though rotation code is shared.
5. Once a correct base config is found, remember to also re-verify color
   (inv on/off) since that was working fine at various points and shouldn't
   need more experimentation — likely just `tft.invertDisplay(true)`.

## How to resume flashing/testing (procedure, already proven to work)

1. `cd "c:/Users/gexom/Documents/GitHub/flight scanner"`
2. Edit `src/main.cpp` and/or `include/User_Setup.h` as needed.
3. `pio run` to build (fast, few seconds once deps are cached).
4. Ask the user to physically do: **hold BOOT, tap RST while still holding
   BOOT, release both** — this must happen immediately before every flash.
5. `pio run -t upload --upload-port COM10` to flash.
6. Ask user for a fresh photo of the actual screen — text descriptions alone
   have repeatedly been ambiguous/misleading in this debugging process;
   photos are far more reliable and should be the default way of getting
   feedback on display state from now on.

## Restoring the real firmware once display is fixed

Once a working rotation/color/offset config is confirmed via the diagnostic
sketch, port the final values into the real firmware:
1. Copy `src/main.cpp.bak` back to `src/main.cpp` (or merge — main.cpp.bak is
   untouched real firmware).
2. Apply the confirmed `tft.setRotation(N)` / `tft.invertDisplay(bool)` calls
   in `setup()` (currently at the top of `setup()`, right after `tft.init()`).
3. If the fix requires the `pushRotated()` software-rotation approach instead
   of a simple hardware rotation, the rendering code
   (`renderRadar()`/`drawStatus()`) will need to draw into the sprite in the
   panel's native (working) orientation and then push via
   `frame.pushRotated(&tft, angle)` instead of `frame.pushSprite(0, 0)` —
   this is a real code change, not just a config tweak, if it comes to that.
4. Rebuild, flash (manual bootloader entry as always), and continue from
   "Guide user through captive portal first-run setup" in the overall project
   plan (`C:\Users\gexom\.claude\plans\functional-jumping-hedgehog.md`).
