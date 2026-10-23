#pragma once

// --- Power ---
#define PIN_POWER_ON        10

// --- SPI Bus ---
#define PIN_SPI_MOSI        41
#define PIN_SPI_MISO        38
#define PIN_SPI_SCK         40

// --- TFT Display (ST7789, 240x320) ---
#define PIN_TFT_CS          12
#define PIN_TFT_DC          11
#define PIN_TFT_BL          42

// --- SD Card ---
#define PIN_SD_CS           39

// --- Radio (SX1262 on the original T-Deck, not T-Deck Pro) ---
// Shared SPI with the TFT and the microSD. Drive both of those CS pins
// high before any transfer. DIO2 is the RF switch and DIO3 is a 1.8 V
// TCXO inside the module; the driver has to program both when it starts.
// Until then CS stays high and RST stays deasserted.
#define PIN_RADIO_CS        9
#define PIN_RADIO_RST       17
#define PIN_RADIO_BUSY      13
#define PIN_RADIO_DIO1      45

// --- I2C Bus ---
#define PIN_I2C_SDA         18
#define PIN_I2C_SCL         8

// --- Keyboard (ESP32-C3 @ I2C) ---
#define KB_I2C_ADDR         0x55
#define PIN_KB_INT          46

// --- Trackball (5 GPIOs, active LOW) ---
#define PIN_TRACKBALL_UP    3
#define PIN_TRACKBALL_DOWN  15
#define PIN_TRACKBALL_LEFT  1
#define PIN_TRACKBALL_RIGHT 2
#define PIN_TRACKBALL_CLICK 0

// --- I2S Audio ---
#define PIN_I2S_WS          5
#define PIN_I2S_BCK         7
#define PIN_I2S_DOUT        6

// --- Display Scaling ---
// Cybiko LCD is 160x100, upscaled 2x to 320x200
// Centered vertically on 320x240 display (20px border top/bottom)
#define SCALED_W            320
#define SCALED_H            200
#define BORDER_TOP          20
