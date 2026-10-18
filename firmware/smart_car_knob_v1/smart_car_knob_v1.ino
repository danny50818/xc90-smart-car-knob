#ifndef ARDUINO_USB_MODE
#define ARDUINO_USB_MODE 1
#endif
#ifndef ARDUINO_USB_CDC_ON_BOOT
#define ARDUINO_USB_CDC_ON_BOOT 0
#endif

#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include "CompositeBleHid.h"
#include "SmartCarKnobBoard.h"
#include "UiDesignV12.h"

namespace {
// Recovered from the running factory firmware. GPIO4 belongs to touch IRQ.
constexpr uint8_t PIN_ENCODER_A = 6;
constexpr uint8_t PIN_ENCODER_B = 5;
constexpr uint8_t PIN_ENCODER_BUTTON = 0;

constexpr uint16_t DEBOUNCE_MS = 8;
constexpr uint16_t LONG_PRESS_MS = 700;
constexpr uint16_t DOUBLE_CLICK_MS = 250;
constexpr uint16_t ENCODER_STEP_MS = 18;
constexpr uint16_t CLEAR_BONDS_MS = 8000;
constexpr uint32_t DISPLAY_IDLE_MS = 60000;

// USB HID keyboard usages.
constexpr uint8_t KEY_F10 = 0x43;
constexpr uint8_t KEY_ENTER = 0x28;
constexpr uint8_t KEY_TAB = 0x2B;
constexpr uint8_t KEY_RIGHT = 0x4F;
constexpr uint8_t KEY_LEFT = 0x50;
constexpr uint8_t KEY_DOWN = 0x51;
constexpr uint8_t KEY_UP = 0x52;
constexpr uint8_t MOD_LEFT_ALT = 0x04;
constexpr uint8_t MOD_LEFT_SHIFT = 0x02;

// USB HID Consumer Page usages.
constexpr uint16_t MEDIA_SLEEP = 0x0032;
constexpr uint16_t MEDIA_PREVIOUS = 0x00B6;
constexpr uint16_t MEDIA_NEXT = 0x00B5;
constexpr uint16_t MEDIA_PLAY_PAUSE = 0x00CD;
constexpr uint16_t MEDIA_VOICE_COMMAND = 0x00CF;
constexpr uint16_t MEDIA_MUTE = 0x00E2;
constexpr uint16_t MEDIA_VOLUME_UP = 0x00E9;
constexpr uint16_t MEDIA_VOLUME_DOWN = 0x00EA;
constexpr uint16_t MEDIA_HOME = 0x0223;
constexpr uint16_t MEDIA_BACK = 0x0224;

CompositeBleHid hid;
portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
KnobUiState uiState{
  KnobPage::Navigation, 4, 45, 50, 0, 0, 0,
  false, false, true, false, true, 0
};

struct DebouncedButton {
  bool stable = false;
  bool rawLast = false;
  bool releaseEvent = false;
  bool longSent = false;
  uint32_t changedAt = 0;
  uint32_t pressedAt = 0;

  void begin(uint32_t now) {
    pinMode(PIN_ENCODER_BUTTON, INPUT_PULLUP);
    stable = digitalRead(PIN_ENCODER_BUTTON) == LOW;
    rawLast = stable;
    changedAt = now;
    pressedAt = now;
  }

  void update(uint32_t now) {
    releaseEvent = false;
    const bool raw = digitalRead(PIN_ENCODER_BUTTON) == LOW;
    if (raw != rawLast) {
      rawLast = raw;
      changedAt = now;
    }
    if (now - changedAt < DEBOUNCE_MS || raw == stable) return;
    stable = raw;
    if (stable) {
      pressedAt = now;
      longSent = false;
    } else {
      releaseEvent = true;
    }
  }
};

DebouncedButton knobButton;
uint8_t encoderHistory = 0;
int8_t encoderAccumulator = 0;
uint32_t lastEncoderAt = 0;
bool chordUsed = false;
bool singlePending = false;
uint32_t singleReleasedAt = 0;
bool touchWasPressed = false;
bool touchMoved = false;
bool touchWakeConsumed = false;
bool buttonWakeConsumed = false;
uint16_t touchLastX = 0;
uint16_t touchLastY = 0;
uint32_t lastInteractionAt = 0;

void markActivity(uint32_t now = millis()) {
  lastInteractionAt = now;
  if (!boardUiIsAwake()) boardUiSetAwake(true);
}

void mutateState(void (*fn)(KnobUiState&)) {
  portENTER_CRITICAL(&stateMux);
  fn(uiState);
  portEXIT_CRITICAL(&stateMux);
}

KnobUiState stateSnapshot() {
  portENTER_CRITICAL(&stateMux);
  KnobUiState copy = uiState;
  portEXIT_CRITICAL(&stateMux);
  return copy;
}

void setPage(int8_t direction) {
  portENTER_CRITICAL(&stateMux);
  int page = static_cast<int>(uiState.page);
  page = (page + direction + 4) % 4;
  uiState.page = static_cast<KnobPage>(page);
  uiState.focus = (uiState.page == KnobPage::Navigation) ? 4 :
                  (uiState.page == KnobPage::Media) ? 1 : 0;
  uiState.pageDirection = direction;
  ++uiState.revision;
  portEXIT_CRITICAL(&stateMux);
}

void returnToNavigation() {
  portENTER_CRITICAL(&stateMux);
  uiState.page = KnobPage::Navigation;
  uiState.focus = 4;
  uiState.pageDirection = -1;
  ++uiState.revision;
  portEXIT_CRITICAL(&stateMux);
}

void rotateNormal(int8_t direction) {
  const KnobUiState state = stateSnapshot();
  switch (state.page) {
    case KnobPage::Navigation:
      // The dial behaves like an ordinary keyboard: clockwise selects the
      // next item with Down, counter-clockwise selects the previous with Up.
      // It never changes the focus/highlight drawn on the knob display.
      hid.tapKey(direction > 0 ? KEY_DOWN : KEY_UP);
      break;
    case KnobPage::Touchpad:
      hid.mouseWheel(direction > 0 ? -1 : 1);
      portENTER_CRITICAL(&stateMux);
      uiState.scrollPosition = constrain(uiState.scrollPosition + direction * 13, 0, 100);
      ++uiState.revision;
      portEXIT_CRITICAL(&stateMux);
      break;
    case KnobPage::Media:
      if (direction > 0) {
        hid.tapConsumer(MEDIA_VOLUME_UP);
        portENTER_CRITICAL(&stateMux);
        uiState.volume = min<uint8_t>(100, uiState.volume + 15);
        ++uiState.revision;
        portEXIT_CRITICAL(&stateMux);
      } else {
        hid.tapConsumer(MEDIA_VOLUME_DOWN);
        portENTER_CRITICAL(&stateMux);
        uiState.volume = uiState.volume >= 15 ? uiState.volume - 15 : 0;
        ++uiState.revision;
        portEXIT_CRITICAL(&stateMux);
      }
      break;
    case KnobPage::System:
      // Same plain Up/Down keyboard shuttle used on the navigation page.
      hid.tapKey(direction > 0 ? KEY_DOWN : KEY_UP);
      break;
  }
}

void onEncoderStep(int8_t direction, uint32_t now) {
  if (now - lastEncoderAt < ENCODER_STEP_MS) return;
  lastEncoderAt = now;
  const bool waking = !boardUiIsAwake();
  markActivity(now);
  // The first detent only wakes the display; subsequent detents operate the
  // current page. This prevents an unseen volume/key change while asleep.
  if (waking) return;
  if (knobButton.stable) {
    chordUsed = true;
    singlePending = false;
    setPage(direction);
  } else {
    rotateNormal(direction);
  }
}

void updateEncoder(uint32_t now) {
  static const int8_t transitions[16] = {
    0,-1,1,0, 1,0,0,-1, -1,0,0,1, 0,1,-1,0
  };
  const uint8_t state = (digitalRead(PIN_ENCODER_A) == LOW ? 2 : 0) |
                        (digitalRead(PIN_ENCODER_B) == LOW ? 1 : 0);
  encoderHistory = ((encoderHistory << 2) | state) & 0x0F;
  encoderAccumulator += transitions[encoderHistory];
  if (encoderAccumulator >= 4) {
    encoderAccumulator = 0;
    // This encoder's electrical phase is opposite to the physical knob's
    // clockwise direction. Normalize it here so every page uses CW = +1.
    onEncoderStep(-1, now);
  } else if (encoderAccumulator <= -4) {
    encoderAccumulator = 0;
    onEncoderStep(1, now);
  }
}

void executeFocusedAction() {
  const KnobUiState state = stateSnapshot();
  if (state.page == KnobPage::Navigation) {
    constexpr uint8_t keys[4] = {KEY_UP, KEY_RIGHT, KEY_DOWN, KEY_LEFT};
    if (state.focus < 4) hid.tapKey(keys[state.focus]);
    else hid.tapConsumer(MEDIA_VOICE_COMMAND); // old firmware's proven voice path
  } else if (state.page == KnobPage::Touchpad) {
    hid.mouseClick();
  } else if (state.page == KnobPage::Media) {
    constexpr uint16_t actions[4] = {
      MEDIA_PREVIOUS, MEDIA_PLAY_PAUSE, MEDIA_NEXT, MEDIA_MUTE
    };
    hid.tapConsumer(actions[min<uint8_t>(state.focus, 3)]);
    if (state.focus == 1) {
      portENTER_CRITICAL(&stateMux);
      uiState.playing = !uiState.playing;
      ++uiState.revision;
      portEXIT_CRITICAL(&stateMux);
    }
  } else {
    switch (state.focus) {
      case 0: hid.tapConsumer(MEDIA_HOME); returnToNavigation(); break;
      case 1: hid.tapConsumer(MEDIA_BACK); break;
      case 2: hid.tapKey(KEY_TAB, MOD_LEFT_ALT); returnToNavigation(); break;
      case 3: hid.tapConsumer(MEDIA_SLEEP); break;
      default: hid.tapKey(KEY_F10); break; // optional host/App vehicle-settings hook
    }
  }
}

void handleButton(uint32_t now) {
  if (knobButton.stable && !knobButton.longSent && !chordUsed &&
      now - knobButton.pressedAt >= LONG_PRESS_MS) {
    knobButton.longSent = true;
    singlePending = false;
    hid.tapConsumer(MEDIA_HOME);
    returnToNavigation();
  }

  if (!knobButton.releaseEvent) return;
  if (buttonWakeConsumed) {
    buttonWakeConsumed = false;
    singlePending = false;
    return;
  }
  if (chordUsed) {
    chordUsed = false;
    return;
  }
  if (knobButton.longSent) return;

  if (singlePending && now - singleReleasedAt <= DOUBLE_CLICK_MS) {
    singlePending = false;
    hid.tapConsumer(MEDIA_BACK);
  } else {
    singlePending = true;
    singleReleasedAt = now;
  }
}

uint8_t focusFromTouch(KnobPage page, uint16_t x, uint16_t y) {
  const int32_t dx = static_cast<int32_t>(x) - 233;
  const int32_t dy = static_cast<int32_t>(y) - 233;
  if (page == KnobPage::Navigation) {
    if (dx * dx + dy * dy < 65 * 65) return 4;
    if (abs(dx) > abs(dy)) return dx > 0 ? 1 : 3;
    return dy > 0 ? 2 : 0;
  }
  if (page == KnobPage::Media) {
    if (y > 340) return 3;
    if (x < 170) return 0;
    if (x > 296) return 2;
    return 1;
  }
  if (page == KnobPage::System) {
    if (dx * dx + dy * dy < 58 * 58) return 4;
    if (y < 233) return x < 233 ? 0 : 1;
    return x < 233 ? 2 : 3;
  }
  return 0;
}

void updateTouch() {
  uint16_t x = 0, y = 0;
  bool pressed = false;
  if (!boardReadTouch(x, y, pressed)) return;
  const KnobUiState state = stateSnapshot();
  if (pressed) {
    if (!touchWasPressed) touchWakeConsumed = !boardUiIsAwake();
    markActivity();
  }

  if (pressed && state.page == KnobPage::Touchpad && touchWasPressed) {
    const int dx = constrain(static_cast<int>(x) - touchLastX, -127, 127);
    const int dy = constrain(static_cast<int>(y) - touchLastY, -127, 127);
    if (dx || dy) {
      touchMoved = true;
      hid.mouseMove(dx, dy);
    }
  } else if (pressed && state.page != KnobPage::Touchpad) {
    portENTER_CRITICAL(&stateMux);
    uiState.focus = focusFromTouch(state.page, x, y);
    ++uiState.revision;
    portEXIT_CRITICAL(&stateMux);
  }

  if (!pressed && touchWasPressed && !touchMoved && !touchWakeConsumed) executeFocusedAction();
  if (!pressed && touchWasPressed) touchWakeConsumed = false;
  if (pressed && !touchWasPressed) touchMoved = false;
  touchWasPressed = pressed;
  touchLastX = x;
  touchLastY = y;
  portENTER_CRITICAL(&stateMux);
  uiState.touchX = x;
  uiState.touchY = y;
  uiState.touchPressed = pressed;
  portEXIT_CRITICAL(&stateMux);
}

// Hidden bench-test input: sending 0..3 over USB selects a page.  It leaves
// the physical encoder/touch behavior unchanged and lets every screen be
// inspected on the actual panel after flashing.
void updateSerialPreview() {
  while (Serial.available()) {
    const char command = static_cast<char>(Serial.read());
    if (command == 'r' || command == 'R') {
      Serial.flush();
      ESP.restart();
    }
    if (command == 's' || command == 'S') { boardUiSetAwake(false); continue; }
    if (command == 'w' || command == 'W') { markActivity(); continue; }
    if (command == 'i' || command == 'I') {
      lastInteractionAt = millis() - DISPLAY_IDLE_MS;
      continue;
    }
    if (command == 'c' || command == 'C') {
      Serial.printf("BLE HID ready=%d screenAwake=%d\n", hid.connected(), boardUiIsAwake());
      continue;
    }
    if (command == 't' || command == 'T') {
      // Reversible production self-test: exercises every composite HID input
      // report without leaving the host volume, pointer or scroll position
      // changed. Keyboard arrows are sent as opposite pairs.
      Serial.printf("HID self-test start connected=%d\n", hid.connected());
      if (hid.connected()) {
        Serial.println("HID test keyboard: up down left right");
        hid.tapKey(KEY_UP); hid.tapKey(KEY_DOWN);
        hid.tapKey(KEY_LEFT); hid.tapKey(KEY_RIGHT);
        Serial.println("HID test consumer: volume up down");
        hid.tapConsumer(MEDIA_VOLUME_UP);
        hid.tapConsumer(MEDIA_VOLUME_DOWN);
        Serial.println("HID test mouse: move out/back, wheel down/up");
        hid.mouseMove(12, 8); delay(30); hid.mouseMove(-12, -8);
        hid.mouseWheel(-1); hid.mouseWheel(1);
      }
      Serial.println("HID self-test complete");
      continue;
    }
    // Individual non-destructive HID checks used by the production test jig.
    // Opposite commands allow the host state to be restored after checking.
    if (command == '+') { hid.tapConsumer(MEDIA_VOLUME_UP); continue; }
    if (command == '-') { hid.tapConsumer(MEDIA_VOLUME_DOWN); continue; }
    if (command == '^') { hid.tapKey(KEY_UP); continue; }
    if (command == 'v') { hid.tapKey(KEY_DOWN); continue; }
    if (command == '<') { hid.tapKey(KEY_LEFT); continue; }
    if (command == '>') { hid.tapKey(KEY_RIGHT); continue; }
    if (command == '{') { hid.mouseMove(-12, 0); continue; }
    if (command == '}') { hid.mouseMove(12, 0); continue; }
    if (command == '[') { hid.mouseWheel(1); continue; }
    if (command == ']') { hid.mouseWheel(-1); continue; }
    // Direct checks for the encoder's ordinary-keyboard shuttle/confirm map.
    // These commands are USB-only bench hooks and do not alter production input.
    if (command == ')') { hid.tapKey(KEY_DOWN); continue; }
    if (command == '(') { hid.tapKey(KEY_UP); continue; }
    if (command == '=') { hid.tapKey(KEY_ENTER); continue; }
    if (command == 'b' || command == 'B') {
      Serial.println("Clearing BLE bonds and restarting");
      Serial.flush();
      hid.clearBonds();
      delay(100);
      ESP.restart();
    }
    if (command < '0' || command > '3') continue;
    const KnobPage requested = static_cast<KnobPage>(command - '0');
    portENTER_CRITICAL(&stateMux);
    const int oldPage = static_cast<int>(uiState.page);
    const int newPage = static_cast<int>(requested);
    uiState.pageDirection = newPage >= oldPage ? 1 : -1;
    uiState.page = requested;
    uiState.focus = requested == KnobPage::Navigation ? 4 :
                    requested == KnobPage::Media ? 1 : 0;
    uiState.booting = false;
    ++uiState.revision;
    portEXIT_CRITICAL(&stateMux);
  }
}

void uiTask(void*) {
  Serial.println("UI task started");
  ESP_LOGE("knob", "UI task started");
  boardUiBegin();
  Serial.println("boardUiBegin returned");
  ESP_LOGE("knob", "boardUiBegin returned");
  for (;;) {
    boardUiRender(stateSnapshot());
    vTaskDelay(pdMS_TO_TICKS(16));
  }
}

bool heldForBondClear() {
  if (digitalRead(PIN_ENCODER_BUTTON) != LOW) return false;
  const uint32_t started = millis();
  while (millis() - started < CLEAR_BONDS_MS) {
    if (digitalRead(PIN_ENCODER_BUTTON) != LOW) return false;
    delay(10);
  }
  return true;
}
} // namespace

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println("SmartCarKnob setup started");
  ESP_LOGE("knob", "setup started");
  pinMode(PIN_ENCODER_A, INPUT_PULLUP);
  pinMode(PIN_ENCODER_B, INPUT_PULLUP);
  pinMode(PIN_ENCODER_BUTTON, INPUT_PULLUP);
  // Bring up the screen independently of Bluetooth.  BLE initialization can
  // take time (or fail because of stale NVS), but must never leave the knob
  // looking dead.
  xTaskCreatePinnedToCore(uiTask, "knob-ui", 6144, nullptr, 1, nullptr, 1);
  const bool clearBonds = heldForBondClear();

  ESP_LOGE("knob", "starting BLE HID");
  hid.begin("SmartCarKnob");
  if (clearBonds) {
    hid.clearBonds();
    while (digitalRead(PIN_ENCODER_BUTTON) == LOW) delay(10);
    delay(250);
    ESP.restart();
  }

  knobButton.begin(millis());
  lastInteractionAt = millis();
  ESP_LOGE("knob", "BLE HID ready");
  delay(UiV12::BOOT_TOTAL_MS);
  mutateState([](KnobUiState& state) { state.booting = false; });
}

void loop() {
  const uint32_t now = millis();
  const bool buttonWasPressed = knobButton.stable;
  const bool screenWasAwake = boardUiIsAwake();
  knobButton.update(now);
  if (knobButton.stable != buttonWasPressed) {
    if (knobButton.stable && !screenWasAwake) buttonWakeConsumed = true;
    markActivity(now);
  }
  mutateState([](KnobUiState& state) {
    state.connected = hid.connected();
    state.pressed = knobButton.stable;
  });
  updateEncoder(now);
  handleButton(now);
  updateTouch();
  updateSerialPreview();

  const uint32_t idleNow = millis();
  if (boardUiIsAwake() && idleNow - lastInteractionAt >= DISPLAY_IDLE_MS)
    boardUiSetAwake(false);

  if (singlePending && now - singleReleasedAt > DOUBLE_CLICK_MS) {
    singlePending = false;
    // A normal physical press always confirms the host's current selection.
    // Screen icons remain direct touch targets and use executeFocusedAction().
    hid.tapKey(KEY_ENTER);
  }
  delay(1);
}
