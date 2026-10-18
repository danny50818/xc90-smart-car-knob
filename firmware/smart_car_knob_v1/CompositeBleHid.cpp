#include "CompositeBleHid.h"

namespace {
constexpr uint8_t kReportMap[] = {
  // Keyboard, report 1: modifiers + reserved + six key codes.
  0x05,0x01, 0x09,0x06, 0xA1,0x01, 0x85,0x01,
  0x05,0x07, 0x19,0xE0, 0x29,0xE7, 0x15,0x00, 0x25,0x01,
  0x75,0x01, 0x95,0x08, 0x81,0x02,
  0x95,0x01, 0x75,0x08, 0x81,0x01,
  0x95,0x05, 0x75,0x01, 0x05,0x08, 0x19,0x01, 0x29,0x05,
  0x91,0x02, 0x95,0x01, 0x75,0x03, 0x91,0x01,
  0x95,0x06, 0x75,0x08, 0x15,0x00, 0x26,0xE7,0x00,
  0x05,0x07, 0x19,0x00, 0x2A,0xE7,0x00, 0x81,0x00, 0xC0,

  // Consumer control, report 2: one 16-bit usage.
  0x05,0x0C, 0x09,0x01, 0xA1,0x01, 0x85,0x02,
  0x15,0x00, 0x26,0xFF,0x03, 0x19,0x00, 0x2A,0xFF,0x03,
  0x75,0x10, 0x95,0x01, 0x81,0x00, 0xC0,

  // Mouse, report 3: five buttons, X, Y, vertical wheel.
  0x05,0x01, 0x09,0x02, 0xA1,0x01, 0x85,0x03, 0x09,0x01, 0xA1,0x00,
  0x05,0x09, 0x19,0x01, 0x29,0x05, 0x15,0x00, 0x25,0x01,
  0x75,0x01, 0x95,0x05, 0x81,0x02,
  0x75,0x03, 0x95,0x01, 0x81,0x01,
  0x05,0x01, 0x09,0x30, 0x09,0x31, 0x09,0x38,
  0x15,0x81, 0x25,0x7F, 0x75,0x08, 0x95,0x03, 0x81,0x06,
  0xC0, 0xC0
};

constexpr uint8_t kTapMs = 20;
}

void CompositeServerCallbacks::onConnect(NimBLEServer* server, NimBLEConnInfo& info) {
  Serial.printf("BLE connected handle=%u encrypted=%d\n", info.getConnHandle(), info.isEncrypted());
  owner_->setConnected(true, info.isEncrypted(), info.getConnHandle());
  const bool securityStarted = NimBLEDevice::startSecurity(info.getConnHandle());
  Serial.printf("BLE security start=%d\n", securityStarted);
}

void CompositeServerCallbacks::onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) {
  Serial.println("BLE disconnected; advertising restarted");
  owner_->setConnected(false, false, BLE_HS_CONN_HANDLE_NONE);
  NimBLEDevice::startAdvertising();
}

void CompositeServerCallbacks::onConfirmPassKey(NimBLEConnInfo& info, uint32_t pin) {
  Serial.printf("BLE numeric comparison pin=%06lu; accepted on knob\n",
                static_cast<unsigned long>(pin));
  NimBLEDevice::injectConfirmPasskey(info, true);
}

void CompositeServerCallbacks::onAuthenticationComplete(NimBLEConnInfo& info) {
  Serial.printf("BLE authentication complete encrypted=%d bonded=%d\n",
                info.isEncrypted(), info.isBonded());
  owner_->setConnected(true, info.isEncrypted(), info.getConnHandle());
  if (info.isEncrypted()) {
    // Avoid racing macOS' pairing/GATT discovery. Request the responsive HID
    // interval only after the encrypted link is fully established.
    owner_->server_->updateConnParams(info.getConnHandle(), 6, 12, 0, 400);
  }
}

void CompositeBleHid::setConnected(bool connected, bool encrypted, uint16_t handle) {
  connected_ = connected;
  encrypted_ = encrypted;
  connHandle_ = handle;
}

void CompositeBleHid::begin(const char* name) {
  NimBLEDevice::init(name);
  NimBLEDevice::setPower(8);
  // Numeric comparison provides authenticated bonding for keyboard-class HID
  // while remaining a standard system pairing flow with no APK or driver.
  NimBLEDevice::setSecurityAuth(BLE_SM_PAIR_AUTHREQ_BOND |
                                BLE_SM_PAIR_AUTHREQ_MITM |
                                BLE_SM_PAIR_AUTHREQ_SC);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_YESNO);
  NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
  NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);

  server_ = NimBLEDevice::createServer();
  callbacks_ = new CompositeServerCallbacks(this);
  server_->setCallbacks(callbacks_);
  hid_ = new NimBLEHIDDevice(server_);
  hid_->setManufacturer("Smart Car Knob");
  hid_->setPnp(0x01, 0x02E5, 0x0002, 0x0100);
  hid_->setHidInfo(0x00, 0x01);
  hid_->setReportMap(const_cast<uint8_t*>(kReportMap), sizeof(kReportMap));
  // macOS reserves the HID service from CoreBluetooth discovery until the
  // device is bonded. This small encrypted setup characteristic remains
  // discoverable and gives the host a deterministic way to initiate the same
  // LE Secure Connections flow; it carries no private or control data.
  NimBLEService* pairingService = server_->createService("7b06a100-5f10-4f61-8f47-3640e08b0001");
  NimBLECharacteristic* pairingTrigger = pairingService->createCharacteristic(
    "7b06a101-5f10-4f61-8f47-3640e08b0001",
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC);
  pairingTrigger->setValue(static_cast<uint8_t>(1));
  keyboard_ = hid_->getInputReport(1);
  hid_->getOutputReport(1);
  consumer_ = hid_->getInputReport(2);
  mouse_ = hid_->getInputReport(3);
  hid_->setBatteryLevel(100);
  server_->start();

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->setAppearance(0x03C0); // Generic HID
  advertising->addServiceUUID(hid_->getHidService()->getUUID());
  advertising->addServiceUUID(hid_->getBatteryService()->getUUID());
  advertising->setPreferredParams(0x08, 0x10);
  advertising->enableScanResponse(true);
  NimBLEAdvertisementData scan;
  scan.setName(name);
  advertising->setScanResponseData(scan);
  advertising->start();
}

void CompositeBleHid::clearBonds() {
  NimBLEDevice::deleteAllBonds();
}

void CompositeBleHid::notify(NimBLECharacteristic* characteristic,
                             const uint8_t* data, size_t size) {
  if (!connected() || characteristic == nullptr) return;
  if (!characteristic->notify(data, size, connHandle_)) {
    Serial.printf("BLE HID notify failed handle=%u size=%u\n",
                  connHandle_, static_cast<unsigned>(size));
  }
}

void CompositeBleHid::tapKey(uint8_t key, uint8_t modifiers) {
  const uint8_t down[8] = {modifiers, 0, key, 0, 0, 0, 0, 0};
  const uint8_t up[8] = {};
  notify(keyboard_, down, sizeof(down));
  delay(kTapMs);
  notify(keyboard_, up, sizeof(up));
  delay(kTapMs);
}

void CompositeBleHid::tapConsumer(uint16_t usage) {
  const uint8_t down[2] = {static_cast<uint8_t>(usage), static_cast<uint8_t>(usage >> 8)};
  const uint8_t up[2] = {};
  notify(consumer_, down, sizeof(down));
  delay(kTapMs);
  notify(consumer_, up, sizeof(up));
  delay(kTapMs);
}

void CompositeBleHid::mouseClick() {
  const uint8_t down[4] = {1, 0, 0, 0};
  const uint8_t up[4] = {};
  notify(mouse_, down, sizeof(down));
  delay(kTapMs);
  notify(mouse_, up, sizeof(up));
  delay(kTapMs);
}

void CompositeBleHid::mouseMove(int8_t x, int8_t y) {
  const uint8_t report[4] = {0, static_cast<uint8_t>(x), static_cast<uint8_t>(y), 0};
  notify(mouse_, report, sizeof(report));
}

void CompositeBleHid::mouseWheel(int8_t amount) {
  const uint8_t report[4] = {0, 0, 0, static_cast<uint8_t>(amount)};
  const uint8_t stop[4] = {};
  notify(mouse_, report, sizeof(report));
  delay(kTapMs);
  notify(mouse_, stop, sizeof(stop));
  delay(kTapMs);
}
