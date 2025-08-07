/* XH32S TFT_eSPI custom setup (copy to TFT_eSPI/User_Setups/)
   or #include it via User_Setup_Select.h
*/
#define ST7789_DRIVER
#define TFT_WIDTH  240
#define TFT_HEIGHT 320

#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS   15
#define TFT_DC   2
#define TFT_RST  -1      // RST pin tied to ESP32 reset or permanently HIGH
#define TFT_BL   27      // Back‑light control pin

// Optional touch pins are defined in touch_config.h

#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_GFXFF

#define SPI_FREQUENCY  40000000   // 40 MHz
