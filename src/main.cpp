#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLEClient.h>

// HID Service & Report UUIDs (Standard BLE HID)
static BLEUUID hidServiceUUID((uint16_t)0x1812);
static BLEUUID hidReportUUID((uint16_t)0x2A4D);

static BLEAddress targetAddress("ff:ff:12:ce:1b:fe");  // Deine AB Shutter3
static BLEClient* pClient = nullptr;
static bool connected = false;
static bool doConnect = false;

// Wird aufgerufen wenn Remote einen Knopf drückt
void notifyCallback(BLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
  Serial.print("Taste gedrückt! HID Daten: ");
  for (int i = 0; i < length; i++) {
    Serial.printf("%02X ", pData[i]);
  }
  Serial.println();

  // Prüfen ob Daten nicht leer (0x00 = Taste losgelassen)
  bool anyPressed = false;
  for (int i = 0; i < length; i++) {
    if (pData[i] != 0x00) anyPressed = true;
  }
  if (anyPressed) {
    Serial.println(">>> KNOPF GEDRÜCKT! <<<");
  }
}

bool connectToRemote() {
  Serial.println("Verbinde mit AB Shutter3...");
  pClient = BLEDevice::createClient();

  if (!pClient->connect(targetAddress)) {
    Serial.println("Verbindung fehlgeschlagen!");
    return false;
  }
  Serial.println("Verbunden!");

  // HID Service suchen
  BLERemoteService* pService = pClient->getService(hidServiceUUID);
  if (pService == nullptr) {
    Serial.println("HID Service nicht gefunden!");
    pClient->disconnect();
    return false;
  }
  Serial.println("HID Service gefunden!");

  // Alle HID Report Characteristics abonnieren
  std::map<std::string, BLERemoteCharacteristic*>* chars = pService->getCharacteristics();
  int count = 0;
  for (auto& c : *chars) {
    BLERemoteCharacteristic* pChar = c.second;
    if (pChar->getUUID().equals(hidReportUUID)) {
      if (pChar->canNotify()) {
        pChar->registerForNotify(notifyCallback);
        Serial.printf("Notification aktiviert auf Characteristic %d\n", count);
        count++;
      }
    }
  }

  if (count == 0) {
    Serial.println("Keine notifizierbaren HID Characteristics gefunden!");
    return false;
  }

  connected = true;
  return true;
}

// Scan Callback - sucht nach AB Shutter3
class MyScanCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (advertisedDevice.getAddress().equals(targetAddress)) {
      Serial.println("AB Shutter3 gefunden! Stoppe Scan...");
      BLEDevice::getScan()->stop();
      doConnect = true;
    }
  }
};

void setup() {
  Serial.begin(921600);
  Serial.println("\n=== Padel Counter BLE Remote ===");

  BLEDevice::init("ESP32_Padel");
  BLEScan* pScan = BLEDevice::getScan();
  pScan->setAdvertisedDeviceCallbacks(new MyScanCallbacks());
  pScan->setActiveScan(true);
  pScan->setInterval(100);
  pScan->setWindow(99);

  Serial.println("Suche nach AB Shutter3...");
  pScan->start(0, false);  // 0 = endlos scannen bis gefunden
}

void loop() {
  if (doConnect && !connected) {
    doConnect = false;
    if (!connectToRemote()) {
      Serial.println("Verbindung fehlgeschlagen, versuche erneut in 3 Sek...");
      delay(3000);
      doConnect = true;
    }
  }

  if (!connected && pClient != nullptr && !pClient->isConnected()) {
    Serial.println("Verbindung verloren! Scanne neu...");
    connected = false;
    BLEDevice::getScan()->start(0, false);
  }

  delay(100);
}
