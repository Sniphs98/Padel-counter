#include <Arduino.h>
#include <NimBLEDevice.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>

// ── Display ───────────────────────────────────────────
MatrixPanel_I2S_DMA *dma_display = nullptr;
uint16_t clrWhite, clrGreen, clrRed, clrYellow, clrCyan;

// ── Einstellungen ─────────────────────────────────────
#define MAX_REMOTES      4      // Anzahl der BLE Remotes
#define SCAN_DURATION    15     // Sekunden zum Suchen beim Start
#define COOLDOWN_MS      7000  // Wartezeit nach einem Punkt (ms)
#define UNDO_COOLDOWN_MS 5000   // Wartezeit nach einem Undo (ms)
#define MAX_UNDO_HISTORY 10     // Wie viele Punkte man zurück kann

// ── Padel Config ──────────────────────────────────────
bool advantageEnabled = true;  // false = Golden Point bei Deuce (40:40)

// ── BLE ───────────────────────────────────────────────
static NimBLEUUID hidServiceUUID((uint16_t)0x1812);
static NimBLEUUID hidReportUUID((uint16_t)0x2A4D);

std::vector<NimBLEAddress> foundAddresses;
NimBLEClient* clients[MAX_REMOTES]    = {nullptr};
bool clientConnected[MAX_REMOTES]     = {false};
std::map<NimBLERemoteCharacteristic*, int> charToPlayer;

// ── Game State ────────────────────────────────────────
enum GamePhase { TEAM_SELECTION, PLAYING, MATCH_OVER };
GamePhase phase = TEAM_SELECTION;

int  teamOf[MAX_REMOTES];
bool playerAssigned[MAX_REMOTES] = {false};
int  assignedCount = 0;

unsigned long lastPointTime = 0;
unsigned long lastUndoTime  = 0;

struct PadelScore {
  int  sets[2]     = {0, 0};
  int  games[2]    = {0, 0};
  int  points[2]   = {0, 0};
  int  advantage   = -1;      // -1 = keiner/deuce, 0 = Team A, 1 = Team B
  bool inTiebreak  = false;
  int  matchWinner = -1;
} score;

std::vector<PadelScore> undoStack;

// ── Anzeige ───────────────────────────────────────────
const char* pointStr(int p) {
  switch (p) {
    case 0: return "0";
    case 1: return "15";
    case 2: return "30";
    case 3: return "40";
    default: return "?";
  }
}

void updateDisplay() {
  if (!dma_display) return;
  dma_display->clearScreen();
  dma_display->setTextSize(1);
  dma_display->setTextWrap(false);
  char buf[16];

  if (phase == TEAM_SELECTION) {
    int cntA = 0, cntB = 0;
    for (int i = 0; i < MAX_REMOTES; i++) {
      if (playerAssigned[i]) { if (teamOf[i] == 0) cntA++; else cntB++; }
    }
    dma_display->setTextColor(clrYellow);
    dma_display->setCursor(5, 2);  dma_display->print("TEAM WAHL");
    dma_display->setTextColor(clrGreen);
    dma_display->setCursor(8, 13); snprintf(buf, sizeof(buf), "A: %d/2", cntA); dma_display->print(buf);
    dma_display->setTextColor(clrRed);
    dma_display->setCursor(8, 23); snprintf(buf, sizeof(buf), "B: %d/2", cntB); dma_display->print(buf);
    return;
  }

  if (phase == MATCH_OVER) {
    dma_display->setTextColor(clrYellow);
    dma_display->setCursor(17, 3); dma_display->print("MATCH");
    dma_display->setTextColor(score.matchWinner == 0 ? clrGreen : clrRed);
    dma_display->setCursor(14, 13);
    snprintf(buf, sizeof(buf), "Team %s", score.matchWinner == 0 ? "A" : "B");
    dma_display->print(buf);
    dma_display->setTextColor(clrWhite);
    dma_display->setCursor(11, 23); dma_display->print("gewinnt");
    return;
  }

  // ── PLAYING ──
  // Zeile 1: Satz
  dma_display->setTextColor(clrYellow);
  dma_display->setCursor(0, 1);  dma_display->print("SAT");
  dma_display->setTextColor(clrGreen);
  dma_display->setCursor(28, 1); snprintf(buf, sizeof(buf), "%d", score.sets[0]); dma_display->print(buf);
  dma_display->setTextColor(clrWhite);
  dma_display->setCursor(36, 1); dma_display->print("-");
  dma_display->setTextColor(clrRed);
  dma_display->setCursor(46, 1); snprintf(buf, sizeof(buf), "%d", score.sets[1]); dma_display->print(buf);

  // Zeile 2: Spiel
  dma_display->setTextColor(clrYellow);
  dma_display->setCursor(0, 12); dma_display->print("SPL");
  dma_display->setTextColor(clrGreen);
  dma_display->setCursor(28, 12); snprintf(buf, sizeof(buf), "%d", score.games[0]); dma_display->print(buf);
  dma_display->setTextColor(clrWhite);
  dma_display->setCursor(36, 12); dma_display->print("-");
  dma_display->setTextColor(clrRed);
  dma_display->setCursor(46, 12); snprintf(buf, sizeof(buf), "%d", score.games[1]); dma_display->print(buf);

  // Zeile 3: Punkte
  if (score.inTiebreak) {
    dma_display->setTextColor(clrCyan);
    dma_display->setCursor(0, 23); dma_display->print("TB");
    int tbA = score.points[0], tbB = score.points[1];
    dma_display->setTextColor(clrGreen);
    dma_display->setCursor(tbA >= 10 ? 22 : 28, 23);
    snprintf(buf, sizeof(buf), "%d", tbA); dma_display->print(buf);
    dma_display->setTextColor(clrWhite);
    dma_display->setCursor(36, 23); dma_display->print("-");
    dma_display->setTextColor(clrRed);
    dma_display->setCursor(46, 23); snprintf(buf, sizeof(buf), "%d", tbB); dma_display->print(buf);
  } else if (score.points[0] >= 3 && score.points[1] >= 3) {
    if (score.advantage == -1) {
      dma_display->setTextColor(clrYellow);
      dma_display->setCursor(17, 23); dma_display->print("DEUCE");
    } else {
      dma_display->setTextColor(score.advantage == 0 ? clrGreen : clrRed);
      dma_display->setCursor(17, 23);
      snprintf(buf, sizeof(buf), "ADV %s", score.advantage == 0 ? "A" : "B");
      dma_display->print(buf);
    }
  } else {
    dma_display->setTextColor(clrYellow);
    dma_display->setCursor(0, 23); dma_display->print("PKT");
    const char* pA = pointStr(score.points[0]);
    const char* pB = pointStr(score.points[1]);
    dma_display->setTextColor(clrGreen);
    dma_display->setCursor(strlen(pA) == 1 ? 28 : 22, 23); dma_display->print(pA);
    dma_display->setTextColor(clrWhite);
    dma_display->setCursor(36, 23); dma_display->print("-");
    dma_display->setTextColor(clrRed);
    dma_display->setCursor(46, 23); dma_display->print(pB);
  }
}

void printScore() {
  Serial.println();
  Serial.printf("  Sätze:  A:%d - B:%d\n", score.sets[0], score.sets[1]);
  Serial.printf("  Spiele: A:%d - B:%d\n", score.games[0], score.games[1]);

  if (score.inTiebreak) {
    Serial.printf("  Tiebreak: A:%d - B:%d\n", score.points[0], score.points[1]);
  } else if (score.points[0] >= 3 && score.points[1] >= 3) {
    if (score.advantage == -1)
      Serial.println("  Punkte:  Deuce");
    else
      Serial.printf("  Punkte:  Vorteil Team %s\n", score.advantage == 0 ? "A" : "B");
  } else {
    Serial.printf("  Punkte: A:%s - B:%s\n",
                  pointStr(score.points[0]), pointStr(score.points[1]));
  }
  Serial.println();
  updateDisplay();
}

// ── Undo ──────────────────────────────────────────────
void pushUndo() {
  if ((int)undoStack.size() >= MAX_UNDO_HISTORY)
    undoStack.erase(undoStack.begin());
  undoStack.push_back(score);
}

void undoLastPoint() {
  unsigned long now = millis();
  long remaining = UNDO_COOLDOWN_MS - (long)(now - lastUndoTime);
  if (remaining > 0) {
    Serial.printf("Undo: Cooldown noch %ld Sek.\n", (remaining / 1000) + 1);
    return;
  }
  if (undoStack.empty()) {
    Serial.println("Undo: Kein Punkt zum Rückgängigmachen!");
    return;
  }
  lastUndoTime = now;
  score = undoStack.back();
  undoStack.pop_back();
  phase = (score.matchWinner >= 0) ? MATCH_OVER : PLAYING;
  Serial.println("<<< Letzter Punkt rückgängig gemacht! >>>");
  printScore();
}

// ── Scoring Logic ─────────────────────────────────────
void winSet(int team);

void winGame(int team) {
  score.points[0] = 0;
  score.points[1] = 0;
  score.advantage  = -1;
  score.inTiebreak = false;
  score.games[team]++;

  int g  = score.games[team];
  int go = score.games[1 - team];

  if (g == 6 && go == 6) {
    score.inTiebreak = true;
    Serial.println("  === TIEBREAK! ===");
    printScore();
    return;
  }

  // Satz gewonnen: erst 6 mit 2 Spielen Vorsprung, oder 7 (aus 7:5 oder Tiebreak 7:6)
  if ((g >= 6 && g - go >= 2) || g == 7) {
    winSet(team);
    return;
  }

  printScore();
}

void winSet(int team) {
  score.sets[team]++;
  score.games[0]   = 0;
  score.games[1]   = 0;
  score.points[0]  = 0;
  score.points[1]  = 0;
  score.advantage  = -1;
  score.inTiebreak = false;

  Serial.printf("\n*** Satz gewonnen! Team %s | Satzstand %d:%d ***\n",
                team == 0 ? "A" : "B", score.sets[0], score.sets[1]);

  if (score.sets[team] == 2) {
    phase = MATCH_OVER;
    score.matchWinner = team;
    Serial.println();
    Serial.println("  ╔══════════════════════════╗");
    Serial.printf( "  ║   MATCH GEWONNEN!        ║\n");
    Serial.printf( "  ║   Team %s gewinnt!        ║\n", team == 0 ? "A" : "B");
    Serial.printf( "  ║   Sätze  %d:%d             ║\n", score.sets[0], score.sets[1]);
    Serial.println("  ╚══════════════════════════╝\n");
    updateDisplay();
  } else {
    printScore();
  }
}

void scorePoint(int team) {
  int other = 1 - team;

  // Tiebreak
  if (score.inTiebreak) {
    score.points[team]++;
    if (score.points[team] >= 7 && score.points[team] - score.points[other] >= 2)
      winGame(team);
    else
      printScore();
    return;
  }

  // Beide bei 40 → Deuce / Vorteil / Golden Point
  if (score.points[0] >= 3 && score.points[1] >= 3) {
    if (!advantageEnabled) {
      winGame(team);                  // Golden Point: nächster Punkt gewinnt
    } else if (score.advantage == team) {
      winGame(team);                  // Hatte Vorteil → Spiel
    } else if (score.advantage == other) {
      score.advantage = -1;           // Gegner verliert Vorteil → Deuce
      printScore();
    } else {
      score.advantage = team;         // Deuce → Vorteil
      printScore();
    }
    return;
  }

  // Normaler Punkt
  score.points[team]++;
  if (score.points[team] >= 4)
    winGame(team);
  else
    printScore();
}

// ── Button Handler ────────────────────────────────────
void onButtonPress(int playerIndex) {

  if (phase == TEAM_SELECTION) {
    if (playerAssigned[playerIndex]) return;
    playerAssigned[playerIndex] = true;
    int team = (assignedCount < 2) ? 0 : 1;
    teamOf[playerIndex] = team;
    assignedCount++;

    Serial.printf("Spieler %d → Team %s (%d/2)\n",
                  playerIndex + 1, team == 0 ? "A" : "B",
                  team == 0 ? assignedCount : assignedCount - 2);
    updateDisplay();

    if (assignedCount == (int)foundAddresses.size()) {
      Serial.println("\n=== Teams komplett! Spiel beginnt! ===");
      Serial.print("  Team A: ");
      for (int i = 0; i < MAX_REMOTES; i++)
        if (playerAssigned[i] && teamOf[i] == 0) Serial.printf("Spieler %d ", i + 1);
      Serial.println();
      Serial.print("  Team B: ");
      for (int i = 0; i < MAX_REMOTES; i++)
        if (playerAssigned[i] && teamOf[i] == 1) Serial.printf("Spieler %d ", i + 1);
      Serial.println();
      phase = PLAYING;
      printScore();
    }
    return;
  }

  if (phase == MATCH_OVER) {
    Serial.println("Match vorbei! ESP32 neu starten für neues Spiel.");
    return;
  }

  // Cooldown
  unsigned long now = millis();
  long remaining = COOLDOWN_MS - (long)(now - lastPointTime);
  int team = teamOf[playerIndex];

  if (remaining > 0) {
    Serial.printf("Team %s: Cooldown noch %ld Sek.\n",
                  team == 0 ? "A" : "B", (remaining / 1000) + 1);
    return;
  }

  lastPointTime = now;
  pushUndo();
  Serial.printf(">>> Team %s: Punkt! <<<\n", team == 0 ? "A" : "B");
  scorePoint(team);
}

// ── BLE Callbacks ─────────────────────────────────────
void notifyCallback(NimBLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
  if (length < 1) return;

  auto it = charToPlayer.find(pChar);
  if (it == charToPlayer.end()) return;
  int playerIndex = it->second;

  // Bekannte HID-Codes:
  //   E9 = Volume Up   → Button 1 (Punkt)
  //   01 = Alternativ  → Button 1 (Punkt, bei einer Remote-Variante)
  //   EA = Volume Down → Button 2 (Undo)
  //   00 = losgelassen → ignorieren
  //   alles andere     → loggen aber ignorieren

  bool isButton1 = false;
  bool isButton2 = false;

  for (int i = 0; i < length; i++) {
    if      (pData[i] == 0xE9) { isButton1 = true; break; }
    else if (pData[i] == 0xEA) { isButton2 = true; break; }
    else if (pData[i] == 0x01) { isButton1 = true; break; }
    else if (pData[i] == 0x02) { isButton2 = true; break; }
  }

  if (!isButton1 && !isButton2) {
    // Unbekannter Code — nur loggen (hilft beim Debuggen)
    bool anyNonZero = false;
    for (int i = 0; i < length; i++) if (pData[i] != 0x00) { anyNonZero = true; break; }
    if (anyNonZero) {
      Serial.printf("[Spieler %d] Unbekannter HID Code: ", playerIndex + 1);
      for (int i = 0; i < length; i++) Serial.printf("%02X ", pData[i]);
      Serial.println();
    }
    return;
  }

  if (isButton2)
    undoLastPoint();
  else
    onButtonPress(playerIndex);
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
  if (pService == nullptr) { clients[index]->disconnect(); return false; }

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
  Serial.printf("Vorteil-Regel: %s\n", advantageEnabled ? "AN" : "AUS (Golden Point)");
  Serial.printf("Schalte alle %d Remotes ein! Scan läuft %d Sek...\n", MAX_REMOTES, SCAN_DURATION);

  // Display initialisieren
  HUB75_I2S_CFG mxconfig(64, 32, 1);
  dma_display = new MatrixPanel_I2S_DMA(mxconfig);
  dma_display->begin();
  dma_display->setBrightness8(80);
  clrWhite  = dma_display->color565(255, 255, 255);
  clrGreen  = dma_display->color565(0,   255, 0);
  clrRed    = dma_display->color565(255, 50,  50);
  clrYellow = dma_display->color565(255, 255, 0);
  clrCyan   = dma_display->color565(0,   200, 255);
  dma_display->clearScreen();
  dma_display->setTextSize(1);
  dma_display->setTextWrap(false);
  dma_display->setTextColor(clrYellow);
  dma_display->setCursor(13, 8);  dma_display->print("Padel");
  dma_display->setTextColor(clrWhite);
  dma_display->setCursor(1, 20);  dma_display->print("Scan laeuft..");

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
  updateDisplay();
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
