#include "CardputerKeyboard.h"
#include <Wire.h>

// TCA8418 registers
#define TCA8418_REG_CFG          0x01
#define TCA8418_REG_INT_STAT     0x02
#define TCA8418_REG_KEY_LCK_EC   0x03
#define TCA8418_REG_KEY_EVENT_A  0x04
#define TCA8418_REG_KP_GPIO1     0x1D
#define TCA8418_REG_KP_GPIO2     0x1E
#define TCA8418_REG_KP_GPIO3     0x1F

// TCA8418 key event encoding: keyCode = (row * 10) + col + 1
// Keymap (7 rows x 8 cols) verified from hardware scan:
//        C0      C1     C2      C3    C4    C5       C6      C7
// R0: { "~",   "tab",  "fn",  "ctrl", "1",  "q",  "shift", "opt"  }
// R1: { "2",   "w",    "a",   "alt",  "3",  "e",  "s",     "z"    }
// R2: { "4",   "r",    "d",   "x",    "5",  "t",  "f",     "c"    }
// R3: { "6",   "y",    "g",   "v",    "7",  "u",  "h",     "b"    }
// R4: { "8",   "i",    "j",   "n",    "9",  "o",  "k",     "m"    }
// R5: { "0",   "p",    "l",   ",",    "-",  "[",  ";",     "."    }
// R6: { "+",   "]",    "'",   "/",   "del", "\\", "ok",  "space"  }
//
// keyCode = row * 10 + col + 1

#define KEY_CODE(row, col) ((row) * 10 + (col) + 1)

CardputerKeyboard::CardputerKeyboard() {
    _wire = nullptr;
    _addr = 0x34;
    memset(_prevState, 0, sizeof(_prevState));
    memset(_currState, 0, sizeof(_currState));
}

void CardputerKeyboard::begin(TwoWire* wire, uint8_t addr) {
    _wire = wire;
    _addr = addr;
    // Wire.begin() is NOT called here — caller owns the bus

    writeReg(TCA8418_REG_CFG, 0x00);

    for (int i = 0; i < 16; i++) {
        uint8_t ev = readReg(TCA8418_REG_KEY_EVENT_A);
        if (ev == 0) break;
    }
    writeReg(TCA8418_REG_INT_STAT, 0x0F);

    writeReg(TCA8418_REG_KP_GPIO1, 0x7F);  // R0-R6 (7 rows)
    writeReg(TCA8418_REG_KP_GPIO2, 0xFF);  // C0-C7 (8 cols)
    writeReg(TCA8418_REG_KP_GPIO3, 0x00);  // no extra cols

    writeReg(TCA8418_REG_CFG, 0x01);

    writeReg(TCA8418_REG_INT_STAT, 0x0F);
    for (int i = 0; i < 16; i++) {
        uint8_t ev = readReg(TCA8418_REG_KEY_EVENT_A);
        if (ev == 0) break;
    }

    Serial.println("TCA8418 keyboard initialized");
}

void CardputerKeyboard::update() {
    memcpy(_prevState, _currState, sizeof(_currState));

    uint8_t evCount = readReg(TCA8418_REG_KEY_LCK_EC) & 0x0F;

    for (int i = 0; i < evCount; i++) {
        uint8_t ev = readReg(TCA8418_REG_KEY_EVENT_A);
        if (ev == 0) break;

        uint8_t keyCode = ev & 0x7F;
        bool pressed = (ev & 0x80) != 0;

        if (keyCode < MAX_KEYS) {
            _currState[keyCode] = pressed ? 1 : 0;
        }
    }

    writeReg(TCA8418_REG_INT_STAT, 0x0F);
}

bool CardputerKeyboard::wasPressed(const char* key) {
    int code = keyNameToCode(key);
    if (code < 0 || code >= MAX_KEYS) return false;
    return (_currState[code] == 1 && _prevState[code] == 0);
}

bool CardputerKeyboard::isHeld(const char* key) {
    int code = keyNameToCode(key);
    if (code < 0 || code >= MAX_KEYS) return false;
    return (_currState[code] == 1);
}

bool CardputerKeyboard::wasReleased(const char* key) {
    int code = keyNameToCode(key);
    if (code < 0 || code >= MAX_KEYS) return false;
    return (_currState[code] == 0 && _prevState[code] == 1);
}

int CardputerKeyboard::keyNameToCode(const char* name) {
    // Row 0: "~", "tab", "fn", "ctrl", "1", "q", "shift", "opt"
    if (strcmp(name, "`")     == 0 || strcmp(name, "~") == 0) return KEY_CODE(0, 0);
    if (strcmp(name, "tab")   == 0)  return KEY_CODE(0, 1);
    if (strcmp(name, "fn")    == 0)  return KEY_CODE(0, 2);
    if (strcmp(name, "ctrl")  == 0)  return KEY_CODE(0, 3);
    if (strcmp(name, "1")     == 0)  return KEY_CODE(0, 4);
    if (strcmp(name, "q")     == 0)  return KEY_CODE(0, 5);
    if (strcmp(name, "shift") == 0)  return KEY_CODE(0, 6);
    if (strcmp(name, "opt")   == 0)  return KEY_CODE(0, 7);

    // Row 1: "2", "w", "a", "alt", "3", "e", "s", "z"
    if (strcmp(name, "2")     == 0)  return KEY_CODE(1, 0);
    if (strcmp(name, "w")     == 0)  return KEY_CODE(1, 1);
    if (strcmp(name, "a")     == 0)  return KEY_CODE(1, 2);
    if (strcmp(name, "alt")   == 0)  return KEY_CODE(1, 3);
    if (strcmp(name, "3")     == 0)  return KEY_CODE(1, 4);
    if (strcmp(name, "e")     == 0)  return KEY_CODE(1, 5);
    if (strcmp(name, "s")     == 0)  return KEY_CODE(1, 6);
    if (strcmp(name, "z")     == 0)  return KEY_CODE(1, 7);

    // Row 2: "4", "r", "d", "x", "5", "t", "f", "c"
    if (strcmp(name, "4")     == 0)  return KEY_CODE(2, 0);
    if (strcmp(name, "r")     == 0)  return KEY_CODE(2, 1);
    if (strcmp(name, "d")     == 0)  return KEY_CODE(2, 2);
    if (strcmp(name, "x")     == 0)  return KEY_CODE(2, 3);
    if (strcmp(name, "5")     == 0)  return KEY_CODE(2, 4);
    if (strcmp(name, "t")     == 0)  return KEY_CODE(2, 5);
    if (strcmp(name, "f")     == 0)  return KEY_CODE(2, 6);
    if (strcmp(name, "c")     == 0)  return KEY_CODE(2, 7);

    // Row 3: "6", "y", "j", "v", "7", "u", "h", "b"
    if (strcmp(name, "6")     == 0)  return KEY_CODE(3, 0);
    if (strcmp(name, "y")     == 0)  return KEY_CODE(3, 1);
    // Note: "j" appears in both R3C2 and R4C2 in original keymap (likely a typo in original)
    // Assigning R3 as primary "j"
    if (strcmp(name, "g")     == 0)  return KEY_CODE(3, 2);
    if (strcmp(name, "v")     == 0)  return KEY_CODE(3, 3);
    if (strcmp(name, "7")     == 0)  return KEY_CODE(3, 4);
    if (strcmp(name, "u")     == 0)  return KEY_CODE(3, 5);
    if (strcmp(name, "h")     == 0)  return KEY_CODE(3, 6);
    if (strcmp(name, "b")     == 0)  return KEY_CODE(3, 7);

    // Row 4: "8", "i", "j"(dup), "n", "9", "o", "k", "m"
    if (strcmp(name, "8")     == 0)  return KEY_CODE(4, 0);
    if (strcmp(name, "i")     == 0)  return KEY_CODE(4, 1);
    if (strcmp(name, "j")     == 0)  return KEY_CODE(4, 2);   // 之前漏了这行! j 不是硬件坏
    if (strcmp(name, "n")     == 0)  return KEY_CODE(4, 3);
    if (strcmp(name, "9")     == 0)  return KEY_CODE(4, 4);
    if (strcmp(name, "o")     == 0)  return KEY_CODE(4, 5);
    if (strcmp(name, "k")     == 0)  return KEY_CODE(4, 6);
    if (strcmp(name, "m")     == 0)  return KEY_CODE(4, 7);

    // Row 5: "0", "p", "l", ",", "-", "[", ";", "."
    if (strcmp(name, "0")     == 0)  return KEY_CODE(5, 0);
    if (strcmp(name, "p")     == 0)  return KEY_CODE(5, 1);
    if (strcmp(name, "l")     == 0)  return KEY_CODE(5, 2);
    if (strcmp(name, ",")     == 0)  return KEY_CODE(5, 3);
    if (strcmp(name, "-")     == 0)  return KEY_CODE(5, 4);
    if (strcmp(name, "[")     == 0)  return KEY_CODE(5, 5);
    if (strcmp(name, ";")     == 0)  return KEY_CODE(5, 6);
    if (strcmp(name, ".")     == 0)  return KEY_CODE(5, 7);

    // Row 6: "+", "]", "'", "/", "del", "\", "ok", "space"
    if (strcmp(name, "+")     == 0 ||
        strcmp(name, "=")     == 0)  return KEY_CODE(6, 0);
    if (strcmp(name, "]")     == 0)  return KEY_CODE(6, 1);
    if (strcmp(name, "'")     == 0)  return KEY_CODE(6, 2);
    if (strcmp(name, "/")     == 0)  return KEY_CODE(6, 3);
    if (strcmp(name, "del")   == 0 ||
        strcmp(name, "backspace") == 0) return KEY_CODE(6, 4);
    if (strcmp(name, "\\")    == 0)  return KEY_CODE(6, 5);
    if (strcmp(name, "ok")    == 0 ||
        strcmp(name, "enter") == 0)  return KEY_CODE(6, 6);
    if (strcmp(name, "space") == 0)  return KEY_CODE(6, 7);

    return -1;
}

void CardputerKeyboard::writeReg(uint8_t reg, uint8_t value) {
    if (!_wire) return;
    _wire->beginTransmission(_addr);
    _wire->write(reg);
    _wire->write(value);
    _wire->endTransmission();
}

uint8_t CardputerKeyboard::readReg(uint8_t reg) {
    if (!_wire) return 0;
    _wire->beginTransmission(_addr);
    _wire->write(reg);
    _wire->endTransmission(false);
    _wire->requestFrom(_addr, (uint8_t)1);
    return _wire->available() ? _wire->read() : 0;
}
