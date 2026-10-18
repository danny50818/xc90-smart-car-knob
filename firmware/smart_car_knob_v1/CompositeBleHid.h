#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>

class CompositeBleHid;

class CompositeServerCallbacks : public NimBLEServerCallbacks {
 public:
  explicit CompositeServerCallbacks(CompositeBleHid* owner) : owner_(owner) {}
  void onConnect(NimBLEServer*, NimBLEConnInfo& info) override;
  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) override;
  void onConfirmPassKey(NimBLEConnInfo& info, uint32_t pin) override;
  void onAuthenticationComplete(NimBLEConnInfo& info) override;
 private:
  CompositeBleHid* owner_;
};

class CompositeBleHid {
 public:
  void begin(const char* name);
  bool connected() const { return connected_ && encrypted_; }
  void clearBonds();
  void tapKey(uint8_t key, uint8_t modifiers = 0);
  void tapConsumer(uint16_t usage);
  void mouseClick();
  void mouseMove(int8_t x, int8_t y);
  void mouseWheel(int8_t amount);

 private:
  friend class CompositeServerCallbacks;
  void setConnected(bool connected, bool encrypted, uint16_t handle);
  void notify(NimBLECharacteristic* characteristic, const uint8_t* data, size_t size);

  NimBLEServer* server_ = nullptr;
  NimBLEHIDDevice* hid_ = nullptr;
  NimBLECharacteristic* keyboard_ = nullptr;
  NimBLECharacteristic* consumer_ = nullptr;
  NimBLECharacteristic* mouse_ = nullptr;
  CompositeServerCallbacks* callbacks_ = nullptr;
  volatile bool connected_ = false;
  volatile bool encrypted_ = false;
  volatile uint16_t connHandle_ = BLE_HS_CONN_HANDLE_NONE;
};
