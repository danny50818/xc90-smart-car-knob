#pragma once

#include <Arduino.h>

enum class KnobPage : uint8_t { Navigation, Touchpad, Media, System };

struct KnobUiState {
  KnobPage page;
  uint8_t focus;
  uint8_t volume;
  uint8_t scrollPosition;
  int8_t pageDirection;
  uint16_t touchX;
  uint16_t touchY;
  bool connected;
  bool pressed;
  bool booting;
  bool touchPressed;
  bool playing;
  uint32_t revision;
};

// Hardware isolation layer for the recovered production board wiring:
// SH8601 QSPI display + CST816S I2C touch, both at 466x466 logical pixels.
void boardUiBegin();
void boardUiRender(const KnobUiState& state);
bool boardReadTouch(uint16_t& x, uint16_t& y, bool& pressed);
void boardUiSetAwake(bool awake);
bool boardUiIsAwake();
