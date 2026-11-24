#ifndef OLED_SSD1315_H
#define OLED_SSD1315_H

#include <Arduino.h>
#include <Wire.h>

#define OLED_WIDTH  128
#define OLED_HEIGHT 64
#define OLED_BUF_SIZE (OLED_WIDTH * OLED_HEIGHT / 8)

class OledSSD1315 {
public:
    OledSSD1315();
    bool begin(TwoWire &wirePort = Wire);
    bool isReady() const { return _ready; }
    uint8_t getAddress() const { return _addr; }

    void clear();
    void drawPixel(int16_t x, int16_t y, bool on = true);
    void drawHLine(int16_t x, int16_t y, int16_t w);
    void drawChar(int16_t x, int16_t y, char c, uint8_t scale = 1);
    void drawText(int16_t x, int16_t y, const char *str, uint8_t scale = 1);
    void drawTextCentered(int16_t y, const char *str, uint8_t scale = 1);
    bool display();

private:
    TwoWire *_wire;
    uint8_t _addr;
    bool _ready;
    uint8_t _buffer[OLED_BUF_SIZE];

    bool probeAddress(uint8_t addr);
    bool sendCommand(uint8_t cmd);
    bool sendCommandList(const uint8_t *cmds, size_t len);
};

#endif

