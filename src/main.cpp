#include <Arduino.h>
#include <NimBLEDevice.h>

#define MAX_REMOTES   4
#define COOLDOWN_MS   10000
#define SCAN_DURATION 15

static NimBLEUUID hidServiceUUID((uint16_t)0x1812);
static NimBLEUUID hidReportUUID((uint16_t)0x2A4D);

// ── BLE ──────────────────────────────────────────────
std::vector<NimBLEAddress> foundAddresses;
NimBLEClient* clients[MAX_REMOTES] = {nullptr};
bool clientConnected[MAX_REMOTES] = {false};
std::map<NimBLERemoteCharacteristic*, int> charToPlayer;

// ── Game State ────────────────────────────────────────
enum GamePhase { TEAM_SELECTION, PLAYING };
GamePhase phase = TEAM_SELECTION;

int teamOf[MAX_REMOTES];        // teamOf[playerIndex] = 0 (A) oder 1 (B)
bool playerAssigned[MAX_REMOTES] = {false};
int assignedCount = 0;

int teamScores[2] = {0, 0};    // [Team A, Team B]
unsigned long lastPointTime = 0;

// ── Output ────────────────────────────────────────────
void printScores() {
  Serial.println("\n--- Punktestand ---");
  Serial.printf("  Team A: %d Punkte\n", teamScores[0]);
  Serial.printf("  Team B: %d Punkte\n", teamScores[1]);
  Serial.println("-------------------\n");
}

void printTeams() {
  Serial.println("\n--- Teams ---");
  Serial.print("  Team A: ");
  for (int i = 0; i < MAX_REMOTES; i++)
    if (playerAssigned[i] && teamOf[i] == 0) Serial.printf("Spieler %d  ", i + 1);
  Serial.println();
  Serial.print("  Team B: ");
  for (int i = 0; i < MAX_REMOTES; i++)
    if (playerAssigned[i] && teamOf[i] == 1) Serial.printf("Spieler %d  ", i + 1);
  Serial.println();
  Serial.println("-------------\n");
}

// ── Button Logic ──────────────────────────────────────
void onButtonPress(int playerIndex) {

  // ── Team-Wahl Phase ──
  if (phase == TEAM_SELECTION) {
    if (playerAssigned[playerIndex]) return;

    playerAssigned[playerIndex] = true;
    int team = (assignedCount < 2) ? 0 : 1;
    teamOf[playerIndex] = team;
    assignedCount++;

    Serial.printf("Spieler %d → Team %s (%d/2)\n",
                  playerIndex + 1, team == 0 ? "A" : "B",
                  team == 0 ? assignedCount : assignedCount - 2);

    if (assignedCount == 4) {
      Serial.println("\n=== Teams komplett! Spiel beginnt! ===");
      printTeams();
      printScores();
      phase = PLAYING;
    }
    return;
  }

  // ── Spielphase ──
  unsigned long now = millis();
  long remaining = COOLDOWN_MS - (long)(now - lastPointTime);
  int team = teamOf[playerIndex];

  if (remaining > 0) {
    Serial.printf("Team %s: Cooldown! Noch %ld Sek warten.\n",
                  team == 0 ? "A" : "B", (remaining / 1000) + 1);
    return;
  }

  lastPointTime = now;
  teamScores[team]++;
  Serial.printf(">>> Team %s: +1 Punkt! (%d gesamt) <<<\n",
                team == 0 ? "A" : "B", teamScores[team]);
  printScores();
}

// ── BLE Callbacks ─────────────────────────────────────
void notifyCallback(NimBLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
  if (length < 1) return;

  bool pressed = false;
  for (int i = 0; i < length; i++) if (pData[i] != 0x00) { pressed = true; break; }
  if (!pressed) return;

  auto it = charToPlayer.find(pChar);
  if (it != charToPlayer.end()) onButtonPress(it->second);
}

bool connectRemote(int index) {
  NimBLEAddress addr = foundAddresses[index];
  Serial.printf("Verbinde Spieler %d (%s)...\n", index + 1, addr.toString().c_str());

  if (clients[index] == nullptr)
    clients[index] = NimBLEDevice::createClient();

  if (!clients[index]->connect(addr)) {
    Serial.printf("Verbindung zu Spieler %d fehlgeschlagen!\n", index + 1);
    return false;
  }

  NimBLERemoteService* pService = clients[index]->getService(hidServiceUUID);
  if (pService == nullptr) {
    clients[index]->disconnect();
    return false;
  }

  auto* chars = pService->getCharacteristics(true);
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
  void onResult(NimBLEAdvertisedDevice* dev) {
    if (dev->getName() != "AB Shutter3") return;
    if ((int)foundAddresses.size() >= MAX_REMOTES) return;

    NimBLEAddress addr = dev->getAddress();
    for (auto& a : foundAddresses) if (a == addr) return;

    Serial.printf("Remote %d gefunden: %s\n", (int)foundAddresses.size() + 1, addr.toString().c_str());
    foundAddresses.push_back(addr);
  }
};

// ── Setup / Loop ──────────────────────────────────────
void setup() {
  Serial.begin(921600);
  Serial.println("\n=== Padel Counter ===");
  Serial.printf("Schalte alle %d Remotes ein! Scan läuft %d Sekunden...\n", MAX_REMOTES, SCAN_DURATION);

  NimBLEDevice::init("ESP32_Padel");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  NimBLEScan* pScan = NimBLEDevice::getScan();
  pScan->setAdvertisedDeviceCallbacks(new ScanCallbacks());
  pScan->setActiveScan(true);
  pScan->setInterval(100);
  pScan->setWindow(99);
  pScan->start(SCAN_DURATION, false);

  Serial.printf("\n%d Remote(s) gefunden.\n", (int)foundAddresses.size());

  if (foundAddresses.empty()) {
    Serial.println("Keine Remotes gefunden! ESP32 neu starten.");
    return;
  }

  for (int i = 0; i < (int)foundAddresses.size(); i++) {
    connectRemote(i);
    delay(300);
  }

  Serial.println("\n=== Team-Wahl: Erste 2 die drücken = Team A, letzte 2 = Team B ===\n");
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
