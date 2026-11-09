// MTG life counter for the M5Stack Dial v1.1
//
// Main screen : turn knob = change life, press = open menu
// Menu        : turn knob = scroll, press = select, tap screen = back to life
// Edit screen : turn knob = change value, press = confirm & return to life,
//               tap screen = cancel (back to menu)
//
// Commander damage also costs life: confirming +N commander damage
// subtracts N from your life total (and reducing it gives the life back).
//
// Commander art: the phone page in docs/index.html sends a 240x240 JPEG
// over Bluetooth LE. It is stored in flash and drawn behind the life total.

#include <M5Dial.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <NimBLEDevice.h>

// ---------- tunables ----------
static constexpr int  ENC_COUNTS_PER_DETENT = 4;   // raw encoder counts per knob click
static constexpr int  ENC_DIRECTION         = 1;   // set to -1 if clockwise goes down
static constexpr bool SOUND_ENABLED         = false;
static constexpr int  MAX_OPPONENTS         = 5;
static constexpr uint32_t DELTA_SHOW_MS     = 2000; // how long "+3 / -5" stays visible
static constexpr uint32_t SAVE_DELAY_MS     = 1500; // write to flash after this much idle

// ---------- colors ----------
static constexpr uint16_t C_BG      = TFT_BLACK;
static constexpr uint16_t C_TEXT    = TFT_WHITE;
static constexpr uint16_t C_DIM     = 0x7BEF;  // grey
static constexpr uint16_t C_LIFE    = 0x2E9F;  // blue-ish
static constexpr uint16_t C_LOW     = TFT_ORANGE;
static constexpr uint16_t C_DEAD    = TFT_RED;
static constexpr uint16_t C_POISON  = 0x47E0;  // green
static constexpr uint16_t C_CMDR    = 0xB81F;  // purple
static constexpr uint16_t C_ACCENT  = TFT_YELLOW;

// ---------- game state (persisted) ----------
struct Game {
  uint32_t magic;
  int16_t  life;
  int16_t  poison;
  int16_t  cmdr[MAX_OPPONENTS];
  uint8_t  opponents;
  int16_t  startLife;
};
static constexpr uint32_t GAME_MAGIC = 0x4D544701;  // "MTG" v1

static Game game;
static Preferences prefs;
static bool     saveDirty  = false;
static uint32_t lastChange = 0;

static void newGame() {
  game.life   = game.startLife;
  game.poison = 0;
  for (auto &c : game.cmdr) c = 0;
}

static void loadGame() {
  prefs.begin("mtg", false);
  bool ok = prefs.getBytesLength("game") == sizeof(Game) &&
            prefs.getBytes("game", &game, sizeof(Game)) == sizeof(Game) &&
            game.magic == GAME_MAGIC;
  if (!ok) {
    game = {};
    game.magic     = GAME_MAGIC;
    game.startLife = 40;
    game.opponents = 3;
    newGame();
  }
}

static void markChanged() {
  saveDirty  = true;
  lastChange = millis();
}

static void maybeSave() {
  if (saveDirty && millis() - lastChange > SAVE_DELAY_MS) {
    prefs.putBytes("game", &game, sizeof(Game));
    saveDirty = false;
  }
}

// ---------- commander art (received over BLE, stored in flash) ----------
// Protocol (all on one GATT service):
//   CTRL write 0x01 + uint32 LE size  -> begin transfer
//   DATA write <bytes> ...            -> append chunks
//   CTRL write 0x02                   -> finish; CTRL read gives "OK" or "ERR ..."
//   CTRL write 0x03                   -> clear art
static const char *BLE_NAME  = "MTG Dial";
static const char *SVC_UUID  = "8d2b0001-5c3a-4e8f-9b7a-2f1e6c4d3a10";
static const char *CTRL_UUID = "8d2b0002-5c3a-4e8f-9b7a-2f1e6c4d3a10";
static const char *DATA_UUID = "8d2b0003-5c3a-4e8f-9b7a-2f1e6c4d3a10";

static constexpr size_t MAX_ART_BYTES = 64 * 1024;
static const char *ART_PATH = "/art.jpg";

static uint8_t *artBuf = nullptr;  // JPEG being displayed (owned by main loop)
static size_t   artLen = 0;
static bool     fsOk   = false;

// transfer state shared with the BLE task
static uint8_t *volatile rxBuf = nullptr;
static volatile size_t rxExpected = 0, rxGot = 0;
static volatile bool   rxActive = false, rxDone = false, artClearReq = false;
static volatile bool   bleConnected = false;
static NimBLECharacteristic *ctrlChr = nullptr;

static void setStatus(const char *s) { if (ctrlChr) ctrlChr->setValue(std::string(s)); }

class ServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *srv, NimBLEConnInfo &info) override {
    bleConnected = true;
    // ask for a short connection interval so transfers are quick (iOS min 15ms)
    srv->updateConnParams(info.getConnHandle(), 12, 24, 0, 400);
  }
  void onDisconnect(NimBLEServer *, NimBLEConnInfo &, int) override {
    bleConnected = false;
    rxActive = false;
    NimBLEDevice::startAdvertising();
  }
};

class CtrlCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &) override {
    NimBLEAttValue v = c->getValue();
    if (v.length() == 0) return;
    const uint8_t *p = v.data();
    switch (p[0]) {
      case 0x01: {  // begin
        if (v.length() < 5) { setStatus("ERR header"); return; }
        if (rxDone) { setStatus("ERR busy"); return; }
        uint32_t size = p[1] | (p[2] << 8) | (p[3] << 16) | ((uint32_t)p[4] << 24);
        if (size == 0 || size > MAX_ART_BYTES) { setStatus("ERR size"); return; }
        rxActive = false;
        free(rxBuf);
        rxBuf = (uint8_t *)malloc(size);
        if (!rxBuf) { setStatus("ERR memory"); return; }
        rxExpected = size;
        rxGot = 0;
        rxActive = true;
        setStatus("RX");
        break;
      }
      case 0x02: {  // end
        if (!rxActive) { setStatus("ERR state"); return; }
        rxActive = false;
        if (rxGot != rxExpected) { setStatus("ERR short"); return; }
        if (rxBuf[0] != 0xFF || rxBuf[1] != 0xD8) { setStatus("ERR not jpeg"); return; }
        rxDone = true;  // main loop takes ownership of rxBuf
        setStatus("OK");
        break;
      }
      case 0x03:
        artClearReq = true;
        setStatus("CLEARED");
        break;
    }
  }
};

class DataCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &) override {
    if (!rxActive) return;
    NimBLEAttValue v = c->getValue();
    size_t n = min((size_t)v.length(), (size_t)(rxExpected - rxGot));
    memcpy(rxBuf + rxGot, v.data(), n);
    rxGot += n;
  }
};

static void bleInit() {
  NimBLEDevice::init(BLE_NAME);
  NimBLEDevice::setMTU(247);
  NimBLEServer *srv = NimBLEDevice::createServer();
  srv->setCallbacks(new ServerCB());
  NimBLEService *svc = srv->createService(SVC_UUID);
  ctrlChr = svc->createCharacteristic(
      CTRL_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
  ctrlChr->setCallbacks(new CtrlCB());
  setStatus("idle");
  NimBLECharacteristic *dataChr = svc->createCharacteristic(
      DATA_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  dataChr->setCallbacks(new DataCB());
  svc->start();
  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(SVC_UUID);
  adv->setName(BLE_NAME);
  adv->enableScanResponse(true);
  adv->start();
}

static void artLoad() {
  if (!fsOk) return;
  File f = LittleFS.open(ART_PATH, "r");
  if (!f) return;
  size_t n = f.size();
  if (n > 0 && n <= MAX_ART_BYTES && (artBuf = (uint8_t *)malloc(n))) {
    artLen = f.read(artBuf, n);
  }
  f.close();
}

// ---------- UI state ----------
enum class Screen { Main, Menu, Edit, ConfirmReset };
enum class Item   { Poison, Cmdr, Opponents, StartLife, NewGame, ClearArt, Back };

struct MenuItem { Item kind; int idx; };

static Screen   screen = Screen::Main;
static MenuItem menu[MAX_OPPONENTS + 7];
static int      menuCount = 0;
static int      menuSel   = 0;

static MenuItem editItem;
static int      editValue = 0, editOrig = 0, editMin = 0, editMax = 0;
static bool     confirmYes = false;

static int      lifeDelta   = 0;
static uint32_t lifeDeltaMs = 0;

static bool dirty = true;
static M5Canvas canvas(&M5Dial.Display);
static long encLast = 0;

static void buildMenu() {
  menuCount = 0;
  menu[menuCount++] = {Item::Poison, 0};
  for (int i = 0; i < game.opponents; i++) menu[menuCount++] = {Item::Cmdr, i};
  menu[menuCount++] = {Item::NewGame, 0};
  menu[menuCount++] = {Item::StartLife, 0};
  menu[menuCount++] = {Item::Opponents, 0};
  if (artBuf) menu[menuCount++] = {Item::ClearArt, 0};
  menu[menuCount++] = {Item::Back, 0};
}

static String itemLabel(const MenuItem &m) {
  switch (m.kind) {
    case Item::Poison:    return "Poison";
    case Item::Cmdr:      return "Cmdr Dmg: Opp " + String(m.idx + 1);
    case Item::Opponents: return "Opponents";
    case Item::StartLife: return "Starting Life";
    case Item::NewGame:   return "New Game";
    case Item::ClearArt:  return "Clear Art";
    case Item::Back:      return "Back";
  }
  return "";
}

// Returns false for items that have no numeric value.
static bool itemValue(const MenuItem &m, int &out) {
  switch (m.kind) {
    case Item::Poison:    out = game.poison;       return true;
    case Item::Cmdr:      out = game.cmdr[m.idx];  return true;
    case Item::Opponents: out = game.opponents;    return true;
    case Item::StartLife: out = game.startLife;    return true;
    default:              return false;
  }
}

static uint16_t itemColor(const MenuItem &m) {
  switch (m.kind) {
    case Item::Poison: return C_POISON;
    case Item::Cmdr:   return C_CMDR;
    case Item::NewGame:return C_LOW;
    default:           return C_TEXT;
  }
}

static void beep(int freq = 3000, int ms = 8) {
  if (SOUND_ENABLED) M5Dial.Speaker.tone(freq, ms);
}

// ---------- drawing ----------
static void text(const String &s, int x, int y, const lgfx::IFont *font,
                 float size, uint16_t color) {
  canvas.setFont(font);
  canvas.setTextSize(size);
  canvas.setTextColor(color);
  canvas.setTextDatum(middle_center);
  canvas.drawString(s, x, y);
}

// Text with a black outline so it stays readable over card art.
static void textOutlined(const String &s, int x, int y, const lgfx::IFont *font,
                         float size, uint16_t color, int w = 2) {
  if (artBuf) {
    for (int dx = -w; dx <= w; dx += w)
      for (int dy = -w; dy <= w; dy += w)
        if (dx || dy) text(s, x + dx, y + dy, font, size, C_BG);
  }
  text(s, x, y, font, size, color);
}

static bool isLethal() {
  if (game.life <= 0 || game.poison >= 10) return true;
  for (int i = 0; i < game.opponents; i++)
    if (game.cmdr[i] >= 21) return true;
  return false;
}

static void drawMain() {
  bool lethal = isLethal();
  uint16_t lifeColor = lethal ? C_DEAD : (game.life <= 10 ? C_LOW : C_LIFE);

  if (artBuf) canvas.drawJpg(artBuf, artLen, 0, 0);

  // ring showing life relative to starting life
  float frac = game.startLife > 0 ? (float)game.life / game.startLife : 0;
  frac = constrain(frac, 0.0f, 1.0f);
  canvas.fillArc(120, 120, 119, 112, 0, 360, 0x18E3);
  if (frac > 0)
    canvas.fillArc(120, 120, 119, 112, -90, -90 + 360 * frac, lifeColor);

  textOutlined(String(game.life), 120, 118, &fonts::Font7, 2,
               lethal ? C_DEAD : C_TEXT, 3);

  if (lifeDelta != 0 && millis() - lifeDeltaMs < DELTA_SHOW_MS) {
    String d = (lifeDelta > 0 ? "+" : "") + String(lifeDelta);
    textOutlined(d, 120, 45, &fonts::FreeSansBold12pt7b, 1,
                 lifeDelta > 0 ? C_POISON : C_DEAD);
  }

  if (bleConnected) canvas.fillCircle(120, 24, 3, C_LIFE);
  if (rxActive && rxExpected)
    textOutlined("Receiving art " + String(rxGot * 100 / rxExpected) + "%",
                 120, 222, &fonts::Font2, 1, C_ACCENT);

  // status line: poison + commander damage (only when non-zero)
  String st;
  if (game.poison) st += "P" + String(game.poison) + " ";
  bool anyCmdr = false;
  for (int i = 0; i < game.opponents; i++) anyCmdr |= game.cmdr[i] != 0;
  if (anyCmdr) {
    st += "C";
    for (int i = 0; i < game.opponents; i++)
      st += (i ? "/" : "") + String(game.cmdr[i]);
  }
  if (st.length()) textOutlined(st, 120, 185, &fonts::FreeSans9pt7b, 1,
                                artBuf ? C_TEXT : C_DIM);
  if (lethal) textOutlined("LETHAL", 120, 205, &fonts::FreeSansBold9pt7b, 1, C_DEAD);
}

static void drawMenuRow(int i, int y, bool selected) {
  const MenuItem &m = menu[(i + menuCount) % menuCount];
  if (selected) {
    text(itemLabel(m), 120, y - 14, &fonts::FreeSansBold12pt7b, 1, itemColor(m));
    int v;
    if (itemValue(m, v))
      text(String(v), 120, y + 20, &fonts::FreeSansBold18pt7b, 1, C_TEXT);
  } else {
    text(itemLabel(m), 120, y, &fonts::FreeSans9pt7b, 1, C_DIM);
  }
}

static void drawMenu() {
  canvas.drawCircle(120, 120, 118, C_DIM);
  text("MENU", 120, 22, &fonts::Font2, 1, C_ACCENT);
  drawMenuRow(menuSel - 1, 55, false);
  drawMenuRow(menuSel, 120, true);
  drawMenuRow(menuSel + 1, 185, false);
  text(String(menuSel + 1) + "/" + String(menuCount), 120, 218, &fonts::Font2, 1, C_DIM);
}

static void drawEdit() {
  uint16_t col = itemColor(editItem);
  canvas.drawCircle(120, 120, 118, col);
  canvas.drawCircle(120, 120, 117, col);
  text(itemLabel(editItem), 120, 45, &fonts::FreeSansBold12pt7b, 1, col);
  text(String(editValue), 120, 115, &fonts::Font7, 1.6, C_TEXT);
  int d = editValue - editOrig;
  String was = "was " + String(editOrig);
  if (d) was += "  (" + String(d > 0 ? "+" : "") + String(d) + ")";
  text(was, 120, 170, &fonts::FreeSans9pt7b, 1, C_DIM);
  if (editItem.kind == Item::Cmdr && d)
    text("life " + String(game.life) + " -> " + String(game.life - d), 120, 192,
         &fonts::Font2, 1, C_LIFE);
  text("press=OK  tap=cancel", 120, 214, &fonts::Font2, 1, C_DIM);
}

static void drawConfirm() {
  canvas.drawCircle(120, 120, 118, C_LOW);
  text("New Game?", 120, 70, &fonts::FreeSansBold12pt7b, 1, C_LOW);
  text("Life -> " + String(game.startLife), 120, 100, &fonts::FreeSans9pt7b, 1, C_DIM);
  int bx[2] = {75, 165};
  const char *lbl[2] = {"NO", "YES"};
  for (int i = 0; i < 2; i++) {
    bool sel = (i == 1) == confirmYes;
    if (sel) canvas.fillRoundRect(bx[i] - 36, 130, 72, 40, 10, i ? C_DEAD : C_DIM);
    else     canvas.drawRoundRect(bx[i] - 36, 130, 72, 40, 10, C_DIM);
    text(lbl[i], bx[i], 151, &fonts::FreeSansBold12pt7b, 1, sel ? C_BG : C_DIM);
  }
}

static void render() {
  canvas.fillScreen(C_BG);
  switch (screen) {
    case Screen::Main:         drawMain();    break;
    case Screen::Menu:         drawMenu();    break;
    case Screen::Edit:         drawEdit();    break;
    case Screen::ConfirmReset: drawConfirm(); break;
  }
  canvas.pushSprite(0, 0);
}

// ---------- input handling ----------
static void openEdit(const MenuItem &m) {
  editItem = m;
  itemValue(m, editOrig);
  editValue = editOrig;
  switch (m.kind) {
    case Item::Poison:    editMin = 0; editMax = 99;            break;
    case Item::Cmdr:      editMin = 0; editMax = 99;            break;
    case Item::Opponents: editMin = 1; editMax = MAX_OPPONENTS; break;
    case Item::StartLife: editMin = 1; editMax = 999;           break;
    default: break;
  }
  screen = Screen::Edit;
}

static void applyEdit() {
  int d = editValue - editOrig;
  switch (editItem.kind) {
    case Item::Poison:    game.poison = editValue; break;
    case Item::Cmdr:
      game.cmdr[editItem.idx] = editValue;
      game.life -= d;  // commander damage is also life loss
      if (d) { lifeDelta = -d; lifeDeltaMs = millis(); }
      break;
    case Item::Opponents: game.opponents = editValue; buildMenu(); break;
    case Item::StartLife: game.startLife = editValue; break;
    default: break;
  }
  if (d) markChanged();
}

static void onTurn(int steps) {
  switch (screen) {
    case Screen::Main:
      game.life = constrain(game.life + steps, -999, 9999);
      if (millis() - lifeDeltaMs > DELTA_SHOW_MS) lifeDelta = 0;
      lifeDelta += steps;
      lifeDeltaMs = millis();
      markChanged();
      break;
    case Screen::Menu:
      menuSel = ((menuSel + steps) % menuCount + menuCount) % menuCount;
      break;
    case Screen::Edit:
      editValue = constrain(editValue + steps, editMin, editMax);
      break;
    case Screen::ConfirmReset:
      confirmYes = !confirmYes;
      break;
  }
  beep();
  dirty = true;
}

static void onPress() {
  switch (screen) {
    case Screen::Main:
      buildMenu();
      menuSel = 0;
      screen = Screen::Menu;
      break;
    case Screen::Menu: {
      const MenuItem &m = menu[menuSel];
      if (m.kind == Item::Back)         screen = Screen::Main;
      else if (m.kind == Item::NewGame) { confirmYes = false; screen = Screen::ConfirmReset; }
      else if (m.kind == Item::ClearArt) { artClearReq = true; screen = Screen::Main; }
      else                              openEdit(m);
      break;
    }
    case Screen::Edit:
      applyEdit();
      screen = Screen::Main;
      break;
    case Screen::ConfirmReset:
      if (confirmYes) { newGame(); lifeDelta = 0; markChanged(); screen = Screen::Main; }
      else            screen = Screen::Menu;
      break;
  }
  beep(1800, 25);
  dirty = true;
}

static void onTap() {
  switch (screen) {
    case Screen::Menu:         screen = Screen::Main; break;
    case Screen::Edit:         screen = Screen::Menu; break;
    case Screen::ConfirmReset: screen = Screen::Menu; break;
    default: return;
  }
  beep(1200, 20);
  dirty = true;
}

// Hand-off of BLE results to the main loop (file I/O + drawing happen here).
static void artPoll() {
  if (rxDone) {
    free(artBuf);
    artBuf = rxBuf;
    artLen = rxExpected;
    rxBuf  = nullptr;
    rxDone = false;
    if (fsOk) {
      File f = LittleFS.open(ART_PATH, "w");
      if (f) { f.write(artBuf, artLen); f.close(); }
    }
    dirty = true;
  }
  if (artClearReq) {
    artClearReq = false;
    free(artBuf);
    artBuf = nullptr;
    artLen = 0;
    if (fsOk) LittleFS.remove(ART_PATH);
    dirty = true;
  }

  // refresh the main screen for transfer progress / connection dot
  static bool lastConn = false;
  static size_t lastPct = 0;
  size_t pct = (rxActive && rxExpected) ? rxGot * 100 / rxExpected + 1 : 0;
  if (bleConnected != lastConn || (pct / 5) != (lastPct / 5)) {
    lastConn = bleConnected;
    lastPct  = pct;
    if (screen == Screen::Main) dirty = true;
  }
}

// ---------- Arduino entry points ----------
void setup() {
  auto cfg = M5.config();
  M5Dial.begin(cfg, true, false);  // encoder on, RFID off
  M5Dial.Speaker.setVolume(80);

  canvas.setColorDepth(16);
  canvas.createSprite(240, 240);

  loadGame();
  fsOk = LittleFS.begin(true);  // formats the storage partition on first boot
  artLoad();
  bleInit();
  encLast = M5Dial.Encoder.read();
  render();
}

void loop() {
  M5Dial.update();

  long raw  = M5Dial.Encoder.read();
  int steps = (raw - encLast) / ENC_COUNTS_PER_DETENT;
  if (steps) {
    encLast += (long)steps * ENC_COUNTS_PER_DETENT;
    onTurn(steps * ENC_DIRECTION);
  }

  if (M5Dial.BtnA.wasPressed()) onPress();
  if (M5Dial.Touch.getDetail().wasClicked()) onTap();

  // let the "+3 / -5" indicator fade out on its own
  static bool deltaVisible = false;
  bool nowVisible = lifeDelta != 0 && millis() - lifeDeltaMs < DELTA_SHOW_MS;
  if (nowVisible != deltaVisible) { deltaVisible = nowVisible; dirty = true; }

  artPoll();
  if (dirty) { render(); dirty = false; }
  maybeSave();
  delay(5);
}
