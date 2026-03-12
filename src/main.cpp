#include <Arduino.h>
#include <NimBLEDevice.h>

#define MAX_REMOTES   4
#define COOLDOWN_MS   10000
#define SCAN_DURATION 15

static NimBLEUUID hidServiceUUID((uint16_t)0x1812);
static NimBLEUUID hidReportUUID((uint16_t)0x2A4D);

std::vector<NimBLEAddress> foundAddresses;
NimBLEClient* clients[MAX_REMOTES] = {nullptr};
bool clientConnected[MAX_REMOTES] = {false};
int scores[MAX_REMOTES] = {0, 0, 0, 0};
unsigned long lastPointTime = 0;

std::map<NimBLERemoteCharacteristic*, int> charToPlayer;

void printScores() {
  Serial.println("\n--- Punktestand ---");
  for (int i = 0; i < (int)foundAddresses.size(); i++) {
    Serial.printf("  Spieler %d: %d Punkte\n", i + 1, scores[i]);
  }
  Serial.println("-------------------\n");
}

void onButtonPress(int playerIndex) {
  unsigned long now = millis();
  long remaining = COOLDOWN_MS - (long)(now - lastPointTime);

  if (remaining > 0) {
    Serial.printf("Spieler %d: Cooldown! Noch %ld Sek warten.\n",
                  playerIndex + 1, (remaining / 1000) + 1);
    return;
  }

  lastPointTime = now;
  scores[playerIndex]++;
  Serial.printf(">>> Spieler %d drückt! +1 Punkt (%d gesamt) <<<\n",
                playerIndex + 1, scores[playerIndex]);
  printScores();
}

void notifyCallback(NimBLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
  auto it = charToPlayer.find(pChar);
  int playerIndex = (it != charToPlayer.end()) ? it->second : -1;

  // Debug: zeige alle empfangenen Daten
  Serial.printf("[Spieler %d] HID: ", playerIndex + 1);
  for (int i = 0; i < length; i++) Serial.printf("%02X ", pData[i]);
  Serial.println();

  if (length < 1) return;

  // Knopf gedrückt wenn irgendetwas != 0 kommt (0x00 = losgelassen)
  bool pressed = false;
  for (int i = 0; i < length; i++) {
    if (pData[i] != 0x00) { pressed = true; break; }
  }

  if (pressed && playerIndex >= 0) {
    onButtonPress(playerIndex);
  }
}

bool connectRemote(int index) {
  NimBLEAddress addr = foundAddresses[index];
  Serial.printf("Verbinde Spieler %d (%s)...\n", index + 1, addr.toString().c_str());

  if (clients[index] == nullptr) {
    clients[index] = NimBLEDevice::createClient();
  }

  if (!clients[index]->connect(addr)) {
    Serial.printf("Verbindung zu Spieler %d fehlgeschlagen!\n", index + 1);
    return false;
  }

  NimBLERemoteService* pService = clients[index]->getService(hidServiceUUID);
  if (pService == nullptr) {
    Serial.printf("HID Service bei Spieler %d nicht gefunden!\n", index + 1);
    clients[index]->disconnect();
    return false;
  }

  std::vector<NimBLERemoteCharacteristic*>* chars = pService->getCharacteristics(true);
  for (auto pChar : *chars) {
    if (pChar->getUUID().equals(hidReportUUID) && pChar->canNotify()) {
      pChar->subscribe(true, notifyCallback);
      charToPlayer[pChar] = index;
    }
  }

  clientConnected[index] = true;
  Serial.printf("Spieler %d verbunden!\n", index + 1);
  return true;
}

class ScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice* advertisedDevice) {
    if (advertisedDevice->getName() != "AB Shutter3") return;
    if ((int)foundAddresses.size() >= MAX_REMOTES) return;

    NimBLEAddress addr = advertisedDevice->getAddress();
    for (auto& a : foundAddresses) {
      if (a == addr) return;
    }
    Serial.printf("Remote %d gefunden: %s\n",
                  (int)foundAddresses.size() + 1, addr.toString().c_str());
    foundAddresses.push_back(addr);
  }
};

void setup() {
  Serial.begin(921600);
  Serial.println("\n=== Padel Counter ===");
  Serial.printf("Schalte alle %d Remotes ein! Scan läuft %d Sekunden...\n",
                MAX_REMOTES, SCAN_DURATION);

  NimBLEDevice::init("ESP32_Padel");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);  // Max Reichweite

  NimBLEScan* pScan = NimBLEDevice::getScan();
  pScan->setAdvertisedDeviceCallbacks(new ScanCallbacks());
  pScan->setActiveScan(true);
  pScan->setInterval(100);
  pScan->setWindow(99);
  pScan->start(SCAN_DURATION, false);

  Serial.printf("\n%d Remote(s) gefunden.\n", (int)foundAddresses.size());

  if (foundAddresses.empty()) {
    Serial.println("Keine Remotes gefunden! ESP32 neu starten und Remotes einschalten.");
    return;
  }

  for (int i = 0; i < (int)foundAddresses.size(); i++) {
    connectRemote(i);
    delay(300);
  }

  Serial.println("\n=== Bereit! Viel Spaß beim Padel! ===");
  printScores();
}

void loop() {
  for (int i = 0; i < (int)foundAddresses.size(); i++) {
    if (clientConnected[i] && clients[i] != nullptr && !clients[i]->isConnected()) {
      Serial.printf("Spieler %d getrennt! Reconnect...\n", i + 1);
      clientConnected[i] = false;
      delay(1000);
      connectRemote(i);
    }
  }
  delay(500);
}
