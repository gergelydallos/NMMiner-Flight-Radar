// TFT_eSPI pin configuration for the ESP32-2432S028R "Cheap Yellow Display" board.
// This is the widely published pinout for this exact board's ILI9341 panel.
// If the screen shows garbled colors, try swapping ILI9341_2_DRIVER for ILI9341_DRIVER.
// If the image is upside down/mirrored, change tft.setRotation() in main.cpp instead of here.
#define USER_SETUP_LOADED

#define ILI9341_2_DRIVER

// Diagnostic photos (2026-07-03) prove this is a normal portrait-scan 240x320
// ILI9341 with a fully working MADCTL/MV — do NOT re-add the M5STACK define
// (tested; it inverts the rotation table and breaks every mode on this panel).
// The panel needs NO color inversion; it does need RGB (not the ILI9341
// default BGR) channel order, confirmed via R/G/B test bars.
#define TFT_RGB_ORDER TFT_RGB

#define TFT_WIDTH  240
#define TFT_HEIGHT 320

#define TFT_MISO 12
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS   15
#define TFT_DC    2
#define TFT_RST  -1
#define TFT_BL   21
#define TFT_BACKLIGHT_ON HIGH

// 55MHz (the usual CYD value) is an overclock beyond the ILI9341 spec; the
// unexplained duplicated-text ghosts seen earlier in debugging may have been
// corrupted SPI commands on this clone's marginal wiring. Keep at 40MHz until
// the display is confirmed stable, then optionally try 55MHz again.
#define SPI_FREQUENCY       40000000
#define SPI_READ_FREQUENCY  20000000

#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT
