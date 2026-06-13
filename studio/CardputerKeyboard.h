#ifndef CARDPUTER_KEYBOARD_H
#define CARDPUTER_KEYBOARD_H

#include <Arduino.h>

class TwoWire;

class CardputerKeyboard {
public:
    CardputerKeyboard();
    void begin(TwoWire* wire, uint8_t addr = 0x34);  // NO sda/scl pins
    void update();

    bool wasPressed(const char* key);
    bool isHeld(const char* key);
    bool wasReleased(const char* key);

private:
    TwoWire* _wire;
    uint8_t _addr;

    static const int MAX_KEYS = 80;
    uint8_t _prevState[MAX_KEYS];
    uint8_t _currState[MAX_KEYS];

    void writeReg(uint8_t reg, uint8_t value);
    uint8_t readReg(uint8_t reg);
    int keyNameToCode(const char* name);
};

#endif
