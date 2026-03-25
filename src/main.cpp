#include <Arduino.h>
#include <NimBLEDevice.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <TelnetStream.h>

// ── WiFi / OTA ────────────────────────────────────────
#include "credentials.h"  // nicht in Git — siehe credentials.h.example

// ── Log-Helper: Serial + Telnet gleichzeitig ──────────
void logf(const char* fmt, ...) {
  char buf[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  Serial.print(buf);
  TelnetStream.print(buf);
}

void logln(const char* msg = "") {
  Serial.println(msg);
  TelnetStream.println(msg);
}

// ── Display ───────────────────────────────────────────
MatrixPanel_I2S_DMA *dma_display = nullptr;
uint16_t clrWhite, clrBlack, clrGreen, clrRed, clrYellow, clrCyan;

// ── Einstellungen ─────────────────────────────────────
#define MAX_REMOTES      4      // Anzahl der BLE Remotes
#define SCAN_DURATION    15     // Sekunden zum Suchen beim Start
#define COOLDOWN_MS      7000   // Wartezeit nach einem Punkt (ms)
#define UNDO_COOLDOWN_MS 5000   // Wartezeit nach einem Undo (ms)
#define MAX_UNDO_HISTORY 10     // Wie viele Punkte man zurück kann
#define DISPLAY_BRIGHTNESS 20   // Display-Helligkeit (0-255), niedrig = stromsparend
#define RESET_BUTTON_PIN   32   // Reset-Button für neues Spiel (GPIO32)

// ── Padel Config ──────────────────────────────────────
bool advantageEnabled = false;  // false = Golden Point bei Deuce (40:40)

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

unsigned long lastPointTime[2] = {0, 0};
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
bool isTeamInCooldown(int team) {
  return phase == PLAYING && (long)(millis() - lastPointTime[team]) < COOLDOWN_MS;
}

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
  bool cdA = isTeamInCooldown(0);
  bool cdB = isTeamInCooldown(1);

  // Zeile 1: Satz
  dma_display->setTextColor(clrYellow);
  dma_display->setCursor(0, 1);  dma_display->print("SAT");
  dma_display->setTextColor(cdA ? clrWhite : clrGreen);
  dma_display->setCursor(28, 1); snprintf(buf, sizeof(buf), "%d", score.sets[0]); dma_display->print(buf);
  dma_display->setTextColor(clrWhite);
  dma_display->setCursor(36, 1); dma_display->print("-");
  dma_display->setTextColor(cdB ? clrWhite : clrRed);
  dma_display->setCursor(46, 1); snprintf(buf, sizeof(buf), "%d", score.sets[1]); dma_display->print(buf);

  // Zeile 2: Spiel
  dma_display->setTextColor(clrYellow);
  dma_display->setCursor(0, 12); dma_display->print("SPL");
  dma_display->setTextColor(cdA ? clrWhite : clrGreen);
  dma_display->setCursor(28, 12); snprintf(buf, sizeof(buf), "%d", score.games[0]); dma_display->print(buf);
  dma_display->setTextColor(clrWhite);
  dma_display->setCursor(36, 12); dma_display->print("-");
  dma_display->setTextColor(cdB ? clrWhite : clrRed);
  dma_display->setCursor(46, 12); snprintf(buf, sizeof(buf), "%d", score.games[1]); dma_display->print(buf);

  // Zeile 3: Punkte
  if (score.inTiebreak) {
    dma_display->setTextColor(clrCyan);
    dma_display->setCursor(0, 23); dma_display->print("TB");
    int tbA = score.points[0], tbB = score.points[1];
    dma_display->setTextColor(cdA ? clrWhite : clrGreen);
    dma_display->setCursor(tbA >= 10 ? 22 : 28, 23);
    snprintf(buf, sizeof(buf), "%d", tbA); dma_display->print(buf);
    dma_display->setTextColor(clrWhite);
    dma_display->setCursor(36, 23); dma_display->print("-");
    dma_display->setTextColor(cdB ? clrWhite : clrRed);
    dma_display->setCursor(46, 23); snprintf(buf, sizeof(buf), "%d", tbB); dma_display->print(buf);
  } else if (score.points[0] >= 3 && score.points[1] >= 3) {
    // Deuce/ADV: kein per-Team-Highlight (Text ist zentriert)
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
    dma_display->setTextColor(cdA ? clrWhite : clrGreen);
    dma_display->setCursor(strlen(pA) == 1 ? 28 : 22, 23); dma_display->print(pA);
    dma_display->setTextColor(clrWhite);
    dma_display->setCursor(36, 23); dma_display->print("-");
    dma_display->setTextColor(cdB ? clrWhite : clrRed);
    dma_display->setCursor(46, 23); dma_display->print(pB);
  }
}

void printScore() {
  logln();
  logf("  Sätze:  A:%d - B:%d\n", score.sets[0], score.sets[1]);
  logf("  Spiele: A:%d - B:%d\n", score.games[0], score.games[1]);

  if (score.inTiebreak) {
    logf("  Tiebreak: A:%d - B:%d\n", score.points[0], score.points[1]);
  } else if (score.points[0] >= 3 && score.points[1] >= 3) {
    if (score.advantage == -1)
      logln("  Punkte:  Deuce");
    else
      logf("  Punkte:  Vorteil Team %s\n", score.advantage == 0 ? "A" : "B");
  } else {
    logf("  Punkte: A:%s - B:%s\n",
                  pointStr(score.points[0]), pointStr(score.points[1]));
  }
  logln();
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
    logf("Undo: Cooldown noch %ld Sek.\n", (remaining / 1000) + 1);
    return;
  }
  if (undoStack.empty()) {
    logln("Undo: Kein Punkt zum Rückgängigmachen!");
    return;
  }
  lastUndoTime = now;
  score = undoStack.back();
  undoStack.pop_back();
  phase = (score.matchWinner >= 0) ? MATCH_OVER : PLAYING;
  logln("<<< Letzter Punkt rückgängig gemacht! >>>");
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
    logln("  === TIEBREAK! ===");
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

  logf("\n*** Satz gewonnen! Team %s | Satzstand %d:%d ***\n",
                team == 0 ? "A" : "B", score.sets[0], score.sets[1]);

  if (score.sets[team] == 2) {
    phase = MATCH_OVER;
    score.matchWinner = team;
    logln();
    logln("  ╔══════════════════════════╗");
    logf( "  ║   MATCH GEWONNEN!        ║\n");
    logf( "  ║   Team %s gewinnt!        ║\n", team == 0 ? "A" : "B");
    logf( "  ║   Sätze  %d:%d             ║\n", score.sets[0], score.sets[1]);
    logln("  ╚══════════════════════════╝\n");
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

    logf("Spieler %d → Team %s (%d/2)\n",
                  playerIndex + 1, team == 0 ? "A" : "B",
                  team == 0 ? assignedCount : assignedCount - 2);
    updateDisplay();

    if (assignedCount == (int)foundAddresses.size()) {
      logln("\n=== Teams komplett! Spiel beginnt! ===");
      logf("  Team A: ");
      for (int i = 0; i < MAX_REMOTES; i++)
        if (playerAssigned[i] && teamOf[i] == 0) logf("Spieler %d ", i + 1);
      logln();
      logf("  Team B: ");
      for (int i = 0; i < MAX_REMOTES; i++)
        if (playerAssigned[i] && teamOf[i] == 1) logf("Spieler %d ", i + 1);
      logln();
      phase = PLAYING;
      printScore();
    }
    return;
  }

  if (phase == MATCH_OVER) {
    logln("Match vorbei! ESP32 neu starten für neues Spiel.");
    return;
  }

  // Cooldown
  unsigned long now = millis();
  int team = teamOf[playerIndex];
  long remaining = COOLDOWN_MS - (long)(now - lastPointTime[team]);

  if (remaining > 0) {
    logf("Team %s: Cooldown noch %ld Sek.\n",
                  team == 0 ? "A" : "B", (remaining / 1000) + 1);
    return;
  }

  lastPointTime[team] = now;
  pushUndo();
  logf(">>> Team %s: Punkt! <<<\n", team == 0 ? "A" : "B");
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
      logf("[Spieler %d] Unbekannter HID Code: ", playerIndex + 1);
      for (int i = 0; i < length; i++) logf("%02X ", pData[i]);
      logln();
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
  logf("Verbinde Spieler %d (%s)...\n", index + 1, addr.toString().c_str());

  if (clients[index] == nullptr)
    clients[index] = NimBLEDevice::createClient();

  if (!clients[index]->connect(addr)) {
    logf("Verbindung zu Spieler %d fehlgeschlagen!\n", index + 1);
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
  logf("Spieler %d verbunden!\n", index + 1);
  return true;
}

class ScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice* dev) {
    if (dev->getName() != "AB Shutter3") return;
    if ((int)foundAddresses.size() >= MAX_REMOTES) return;
    NimBLEAddress addr = dev->getAddress();
    for (auto& a : foundAddresses) if (a == addr) return;
    logf("Remote %d gefunden: %s\n", (int)foundAddresses.size() + 1, addr.toString().c_str());
    foundAddresses.push_back(addr);
  }
};

// ── Setup / Loop ──────────────────────────────────────
void setup() {
  Serial.begin(921600);
  Serial.println("\n=== Padel Counter ===");

  // Display initialisieren
  HUB75_I2S_CFG mxconfig(64, 32, 1);
  mxconfig.driver = HUB75_I2S_CFG::FM6126A;  // bei Ghosting probieren; bei falschen Farben wieder entfernen
  dma_display = new MatrixPanel_I2S_DMA(mxconfig);
  dma_display->begin();
  dma_display->setBrightness8(DISPLAY_BRIGHTNESS);
  clrWhite  = dma_display->color565(255, 255, 255);
  clrBlack  = 0;
  clrGreen  = dma_display->color565(0,   255, 0);
  clrRed    = dma_display->color565(255, 50,  50);
  clrYellow = dma_display->color565(180, 180, 0);
  clrCyan   = dma_display->color565(0,   200, 255);

  // WiFi verbinden
  dma_display->clearScreen();
  dma_display->setTextSize(1);
  dma_display->setTextWrap(false);
  dma_display->setTextColor(clrYellow);
  dma_display->setCursor(13, 8);  dma_display->print("Padel");
  dma_display->setTextColor(clrCyan);
  dma_display->setCursor(4, 20);  dma_display->print("WiFi...");

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("WiFi verbinde mit %s", WIFI_SSID);
  int wifiRetry = 0;
  while (WiFi.status() != WL_CONNECTED && wifiRetry < 20) {
    delay(500);
    Serial.print(".");
    wifiRetry++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nWiFi verbunden! IP: %s\n", WiFi.localIP().toString().c_str());

    // OTA konfigurieren
    ArduinoOTA.setHostname("padel-counter");
    ArduinoOTA.setPassword(OTA_PASS);
    ArduinoOTA.onStart([]() {
      logln("OTA: Flash startet...");
    });
    ArduinoOTA.onEnd([]() {
      logln("\nOTA: Fertig! Neustart...");
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
      logf("OTA: %u%%\r", progress / (total / 100));
    });
    ArduinoOTA.onError([](ota_error_t error) {
      logf("OTA Fehler [%u]\n", error);
    });
    ArduinoOTA.begin();

    // Telnet Console starten (Port 23)
    TelnetStream.begin(23);
    logln("Telnet Console aktiv (Port 23)");
  } else {
    Serial.println("\nWiFi fehlgeschlagen — OTA/Telnet nicht verfügbar.");
  }

  logf("Vorteil-Regel: %s\n", advantageEnabled ? "AN" : "AUS (Golden Point)");
  logf("Schalte alle %d Remotes ein! Scan läuft %d Sek...\n", MAX_REMOTES, SCAN_DURATION);

  NimBLEDevice::init("ESP32_Padel");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  NimBLEScan* pScan = NimBLEDevice::getScan();
  pScan->setAdvertisedDeviceCallbacks(new ScanCallbacks());
  pScan->setActiveScan(true);
  pScan->setInterval(100);
  pScan->setWindow(99);

  // Scan sekündlich mit Display-Update
  for (int t = SCAN_DURATION; t > 0; t--) {
    dma_display->clearScreen();
    dma_display->setTextSize(1);
    dma_display->setTextWrap(false);
    char buf[20];
    dma_display->setTextColor(clrYellow);
    dma_display->setCursor(13, 2); dma_display->print("Padel");
    dma_display->setTextColor(clrGreen);
    snprintf(buf, sizeof(buf), "Remote: %d/%d", (int)foundAddresses.size(), MAX_REMOTES);
    dma_display->setCursor(0, 13); dma_display->print(buf);
    dma_display->setTextColor(clrWhite);
    snprintf(buf, sizeof(buf), "Noch: %ds", t);
    dma_display->setCursor(0, 23); dma_display->print(buf);
    pScan->start(1, t < SCAN_DURATION);
    ArduinoOTA.handle();
    if ((int)foundAddresses.size() >= MAX_REMOTES) break;
  }

  logf("\n%d Remote(s) gefunden.\n", (int)foundAddresses.size());

  if (foundAddresses.empty()) {
    logln("Keine Remotes gefunden! ESP32 neu starten.");
    return;
  }

  for (int i = 0; i < (int)foundAddresses.size(); i++) {
    connectRemote(i);
    delay(300);
  }

  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);
  pinMode(33, OUTPUT); digitalWrite(33, LOW);  // virtueller GND für Reset-Button

  logln("\n=== Team-Wahl: Erste 2 die drücken = Team A, letzte 2 = Team B ===\n");
  updateDisplay();
}

void loop() {
  ArduinoOTA.handle();

  // Reset-Button (GPIO32): neues Spiel starten
  static bool lastBtnState = HIGH;
  bool btnState = digitalRead(RESET_BUTTON_PIN);
  if (lastBtnState == HIGH && btnState == LOW) {
    delay(20); // Entprellen
    if (digitalRead(RESET_BUTTON_PIN) == LOW) {
      logln(">>> Reset: Neues Spiel (Teams bleiben)! <<<");
      score = PadelScore();
      undoStack.clear();
      lastPointTime[0] = 0; lastPointTime[1] = 0;
      lastUndoTime = 0;
      phase = PLAYING;
      printScore();
    }
  }
  lastBtnState = btnState;

  // Eingehende Telnet-Bytes verwerfen (nur Ausgabe, keine Eingabe)
  while (TelnetStream.available()) TelnetStream.read();

  // Display aktualisieren wenn Cooldown abläuft
  static bool wasCdA = false, wasCdB = false;
  bool cdA = isTeamInCooldown(0);
  bool cdB = isTeamInCooldown(1);
  if (wasCdA != cdA || wasCdB != cdB) {
    wasCdA = cdA;
    wasCdB = cdB;
    updateDisplay();
  }

  for (int i = 0; i < (int)foundAddresses.size(); i++) {
    bool connected = clientConnected[i] && clients[i] != nullptr && clients[i]->isConnected();
    if (!connected) {
      if (clientConnected[i]) logf("Spieler %d getrennt! Reconnect...\n", i + 1);
      clientConnected[i] = false;
      delay(1000);
      connectRemote(i);
    }
  }
  delay(500);
}
