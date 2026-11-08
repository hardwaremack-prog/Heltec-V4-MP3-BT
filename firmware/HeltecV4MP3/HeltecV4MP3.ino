/*
  Heltec V4 MP3 + BT — expanded pocket music player for the Heltec WiFi LoRa 32 V4 (ESP32-S3R2)
  ---------------------------------------------------------------------------------------------
  Everything from the V3 version, plus what the V4's 2 MB PSRAM and 16 MB flash make possible:
  * MP3, AAC (.aac ADTS), FLAC and WAV playback, with 64 KB PSRAM read-ahead buffers
  * Internet radio over Wi-Fi (http:// MP3/AAC streams) with live song titles; stations editable on the web page
  * Big libraries: 2000 items per folder, "Shuffle all music" over up to 5000 tracks, whole-card search
  * 2-band EQ (bass / treble), sleep timer with fade-out, VU meter on Now Playing
  * Battery gauge (V4 battery sense on GPIO1 / ADC_Ctrl GPIO37)
  * Bluetooth headsets via a KCX_BT_EMITTER module (scan / connect / forget), Wi-Fi web control panel,
    MP3 upload, wireless firmware updates

  Libraries: ESP8266Audio (earlephilhower) 2.4+, U8g2 (olikraus)
  Board: Heltec WiFi LoRa 32(V4), PSRAM: QSPI enabled, Flash 16MB, USB CDC On Boot: Enabled
  Uses the partitions.csv in this folder (2 x 4 MB app for OTA + ~7.9 MB LittleFS)
*/

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <Wire.h>
#include <LittleFS.h>
#include <U8g2lib.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include <Update.h>
#include "AudioFileSourceSD.h"
#include "AudioFileSourceID3.h"
#include "AudioFileSourceBuffer.h"
#include "AudioFileSourceICYStream.h"
#include "AudioGeneratorMP3.h"
#include "AudioGeneratorAAC.h"
#include "AudioGeneratorFLAC.h"
#include "AudioGeneratorWAV.h"
#include "AudioOutputI2S.h"
#include "qrcode.h"          // esp_qrcode (built into the ESP32 core)

#define FW_VERSION "2.0.3"
#define BOARD_NAME "Heltec V4"

// ---------------- Pins (Heltec WiFi LoRa 32 V4, ESP32-S3R2) ----------------
// Built in
#define PIN_OLED_SDA   17
#define PIN_OLED_SCL   18
#define PIN_OLED_RST   21
#define PIN_VEXT       36   // LOW = power to OLED
#define PIN_LED        35
#define PIN_PRG         0
#define PIN_VBAT        1   // battery sense (divider)
#define PIN_ADC_CTRL   37   // HIGH = connect battery divider
#define PIN_FEM_POWER   7   // LoRa front-end LDO: held LOW (LoRa PA off) to save power
// NOT used (taken on the V4): 2, 5, 7, 46 = LoRa PA/FEM, 19/20 = USB, 26 = PSRAM, 8-14 = LoRa SX1262

// microSD module (SPI) - "inline" layout: the module's CS/MOSI/CLK/MISO pins plug straight
// into four neighbouring J3 header pins (42, 41, 40, 39); only 3v3 and GND need jumpers.
// Shares pins with the GNSS connector: don't plug the GNSS kit in.
#define PIN_SD_CS      42
#define PIN_SD_MOSI    41
#define PIN_SD_SCK     40   // "CLK" on the module
#define PIN_SD_MISO    39

// PCM5102A I2S DAC
#define PIN_I2S_BCK    48
#define PIN_I2S_LRCK   47
#define PIN_I2S_DIN    33

// Optional external buttons (to GND)
#define PIN_BTN_UP      4
#define PIN_BTN_DOWN    6
#define PIN_BTN_OK      3
#define PIN_BTN_BACK   34

// KCX_BT_EMITTER (through a 3.3V<->5V level shifter). UART0 header pins are free on the V4 (USB is native).
#define PIN_BT_TX      43   // ESP32 TX -> module RX
#define PIN_BT_RX      44   // module TX -> ESP32 RX
#define PIN_BT_STATUS  38   // module LINK

// ---------------- Display ----------------
U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, PIN_OLED_RST, PIN_OLED_SCL, PIN_OLED_SDA);

// ---------------- PSRAM string arenas ----------------
struct Arena {
  char *buf = nullptr; size_t cap = 0, used = 0;
  bool init(size_t n) {
    if (psramFound()) buf = (char *)ps_malloc(n); else { n /= 16; buf = (char *)malloc(n); }
    cap = buf ? n : 0; used = 0; return buf != nullptr;
  }
  const char *add(const char *s) {
    size_t l = strlen(s) + 1;
    if (used + l > cap) return nullptr;
    char *p = buf + used; memcpy(p, s, l); used += l; return p;
  }
  void clear() { used = 0; }
};
template <typename T> T *bigAlloc(size_t n) {
  T *p = (T *)(psramFound() ? ps_calloc(n, sizeof(T)) : calloc(n, sizeof(T)));
  return p;
}

// ---------------- Audio ----------------
#define FILE_BUF  (64 * 1024)
#define RADIO_BUF (128 * 1024)
SPIClass sdSPI(HSPI);
AudioGenerator            *gen  = nullptr;
AudioFileSourceSD         *fsrc = nullptr;
AudioFileSourceID3        *id3  = nullptr;
AudioFileSourceICYStream  *icy  = nullptr;
AudioFileSourceBuffer     *bsrc = nullptr;
AudioOutputI2S            *i2s  = nullptr;
uint8_t *fileBuf = nullptr, *radioBuf = nullptr;
SemaphoreHandle_t audioMux;
Preferences prefs;

enum SrcType { SRC_NONE, SRC_FILE, SRC_RADIO };
volatile SrcType srcType = SRC_NONE;
volatile bool trackEnded = false, streamDropped = false;
bool  isPlaying = false, isPaused = false;
uint32_t playStartMs = 0, pausedAccum = 0, pauseStartMs = 0;

// metadata (fixed buffers: written from the audio task, read by the UI)
char tagTitle[96] = "", tagArtist[96] = "", streamTitle[128] = "", nowFile[96] = "", nowFmt[6] = "";

// ---------------- 2-band EQ + VU meter (wraps the I2S output) ----------------
class AudioOutputEQ : public AudioOutput {
public:
  AudioOutputEQ(AudioOutput *s) : sink(s) {}
  bool SetRate(int hz) override { hertz = hz; recalc(); return sink->SetRate(hz); }
  bool SetChannels(int c) override { channels = c; return sink->SetChannels(c); }
  bool SetGain(float f) override { return sink->SetGain(f); }
  bool begin() override { reset(); return sink->begin(); }
  bool stop() override { pending = false; return sink->stop(); }
  void flush() override { sink->flush(); }
  bool loop() override { return sink->loop(); }
  bool ConsumeSample(int16_t s[2]) override {
    if (!pending) {
      float a = s[0], b = (channels == 1) ? s[0] : s[1];
      if (active) { a = proc(0, a) * pre; b = proc(1, b) * pre; }
      outS[0] = clip(a); outS[1] = clip(b);
      uint16_t pa = abs(outS[0]), pb = abs(outS[1]);
      if (pa > peakL) peakL = pa;
      if (pb > peakR) peakR = pb;
      pending = true;
    }
    if (sink->ConsumeSample(outS)) { pending = false; return true; }
    return false;
  }
  void setEQ(int bassDb, int trebleDb) { bass = bassDb; treble = trebleDb; recalc(); }
  volatile uint16_t peakL = 0, peakR = 0;
private:
  AudioOutput *sink;
  bool pending = false, active = false;
  int16_t outS[2];
  int bass = 0, treble = 0;
  float pre = 1;
  float c[2][5];          // [filter][b0 b1 b2 a1 a2]
  float z[2][2][4];       // [channel][filter][x1 x2 y1 y2]
  static int16_t clip(float v) { return v > 32767 ? 32767 : (v < -32768 ? -32768 : (int16_t)v); }
  void reset() { memset(z, 0, sizeof(z)); pending = false; }
  void shelf(float *k, bool low, float f0, float db) {
    float fs = hertz > 0 ? hertz : 44100;
    float A = powf(10.f, db / 40.f), w0 = 2.f * PI * f0 / fs, cw = cosf(w0), sw = sinf(w0);
    float alpha = sw / 2.f * sqrtf(2.f), sA = 2.f * sqrtf(A) * alpha;
    float b0, b1, b2, a0, a1, a2;
    if (low) {
      b0 = A * ((A + 1) - (A - 1) * cw + sA); b1 = 2 * A * ((A - 1) - (A + 1) * cw); b2 = A * ((A + 1) - (A - 1) * cw - sA);
      a0 = (A + 1) + (A - 1) * cw + sA;       a1 = -2 * ((A - 1) + (A + 1) * cw);    a2 = (A + 1) + (A - 1) * cw - sA;
    } else {
      b0 = A * ((A + 1) + (A - 1) * cw + sA); b1 = -2 * A * ((A - 1) + (A + 1) * cw); b2 = A * ((A + 1) + (A - 1) * cw - sA);
      a0 = (A + 1) - (A - 1) * cw + sA;       a1 = 2 * ((A - 1) - (A + 1) * cw);      a2 = (A + 1) - (A - 1) * cw - sA;
    }
    k[0] = b0 / a0; k[1] = b1 / a0; k[2] = b2 / a0; k[3] = a1 / a0; k[4] = a2 / a0;
  }
  void recalc() {
    active = bass || treble;
    shelf(c[0], true, 120, bass);
    shelf(c[1], false, 6000, treble);
    int boost = max(0, max(bass, treble));
    pre = powf(10.f, -boost / 20.f);      // headroom so boosts don't clip
  }
  float proc(int ch, float x) {
    for (int f = 0; f < 2; f++) {
      float *k = c[f], *s = z[ch][f];
      float y = k[0] * x + k[1] * s[0] + k[2] * s[1] - k[3] * s[2] - k[4] * s[3];
      s[1] = s[0]; s[0] = x; s[3] = s[2]; s[2] = y; x = y;
    }
    return x;
  }
};
AudioOutputEQ *out = nullptr;

// ---------------- Settings ----------------
enum PlayMode { MODE_NORMAL, MODE_REPEAT_ALL, MODE_REPEAT_ONE, MODE_SHUFFLE, MODE_COUNT };
const char *modeNames[] = { "Normal", "Repeat all", "Repeat one", "Shuffle" };
const char *modeIcons[] = { "", "RA", "R1", "SH" };
int  volume = 12, playMode = MODE_NORMAL, screenIdx = 1, brightIdx = 2;
const uint16_t screenTimes[] = { 0, 30, 60, 300 };
const char *screenNames[]    = { "Always on", "30 sec", "1 min", "5 min" };
const uint8_t brightVals[]   = { 10, 90, 255 };
const char *brightNames[]    = { "Low", "Medium", "High" };
bool resumeOn = true, visOn = true, connectAtStart = true;
int  eqBass = 0, eqTreble = 0;               // -6..+6 dB
const int sleepMins[] = { 0, 15, 30, 45, 60, 90 };
int  sleepIdx = 0; uint32_t sleepEndMs = 0; float fadeGain = 1.0f;

// ---------------- Library data (PSRAM) ----------------
#define MAX_ENTRIES 2000
#define MAX_PL      5000
struct Entry { const char *name; bool dir; uint32_t size; };
Entry  *entries = nullptr; int entryCount = 0; Arena dirArena;
String  curDir = "/";
int     listSel = 0, listTop = 0;

const char **playlist = nullptr; int plCount = 0, plIndex = -1; Arena plArena;
int    *shuffleOrder = nullptr; int shufflePos = 0;
String  plDir = "/", plLabel = "";
bool    plFromIndex = false;

const char **libIndex = nullptr; int idxCount = 0; Arena idxArena; bool idxReady = false;

// ---------------- Radio ----------------
#define MAX_STATIONS 50
String stName[MAX_STATIONS], stUrl[MAX_STATIONS]; int stCount = 0; int stPlaying = -1;
int radioRetries = 0; uint32_t radioRetryAt = 0;

// ---------------- Battery ----------------
int battMv = 0, battPct = -1; uint32_t battNext = 0;

// ---------------- UI state ----------------
enum Screen { SCR_HOME, SCR_BROWSE, SCR_NOW, SCR_CONTROLS, SCR_SETTINGS, SCR_NOSD, SCR_INFO, SCR_BT, SCR_WIFI, SCR_RADIO, SCR_CONNECT };
Screen screen = SCR_HOME;
int  menuSel = 0, menuTop = 0;
uint32_t lastInputMs = 0;
bool displayAsleep = false;
String toastMsg; uint32_t toastUntil = 0;
bool sdOk = false, fsOk = false, extButtons = false;
uint16_t vuL = 0, vuR = 0;

// ---------------- Input ----------------
enum Ev { EV_NONE, EV_UP, EV_DOWN, EV_OK, EV_BACK, EV_UP_LONG, EV_DOWN_LONG, EV_OK_LONG };
struct InputEvent { Ev ev; bool prg; };
struct Btn { uint8_t pin; bool repeat; bool down = false, longFired = false; uint32_t t0 = 0, lastRep = 0, lastChange = 0; };
Btn bUp{PIN_BTN_UP, true}, bDown{PIN_BTN_DOWN, true}, bOk{PIN_BTN_OK, false}, bBack{PIN_BTN_BACK, false};

Ev pollBtn(Btn &b, Ev shortEv, Ev longEv) {
  bool pressed = digitalRead(b.pin) == LOW;
  uint32_t now = millis();
  if (pressed != b.down && now - b.lastChange > 25) {
    b.lastChange = now; b.down = pressed;
    if (pressed) { b.t0 = now; b.longFired = false; b.lastRep = now; extButtons = true; }
    else if (!b.longFired) return shortEv;
    return EV_NONE;
  }
  if (b.down && !b.longFired && now - b.t0 > 600) { b.longFired = true; return longEv; }
  return EV_NONE;
}

struct { bool down = false; uint32_t t0 = 0, lastChange = 0, releaseAt = 0; int clicks = 0; bool longSent = false, xlongSent = false; } prg;
enum PrgEv { P_NONE, P_CLICK, P_DOUBLE, P_LONG, P_XLONG };
// PRG gestures. Long presses act when you LET GO, and the screen says what letting go will do:
//   tap = down, double-tap = up, hold 0.6-1.5 s then let go = OK, hold 1.5 s+ then let go = Back.
// (Before v2.0.2 "Back" fired by itself at 1.5 s while still held, so holding to select often went Back.)
PrgEv pollPrg() {
  bool pressed = digitalRead(PIN_PRG) == LOW;
  uint32_t now = millis();
  if (pressed != prg.down && now - prg.lastChange > 25) {
    prg.lastChange = now; prg.down = pressed;
    if (pressed) { prg.t0 = now; }
    else {
      uint32_t held = now - prg.t0;
      if (held >= 1500) { prg.clicks = 0; return P_XLONG; }
      if (held >= 600)  { prg.clicks = 0; return P_LONG; }
      prg.clicks++; prg.releaseAt = now;
      if (prg.clicks >= 2) { prg.clicks = 0; return P_DOUBLE; }
    }
    return P_NONE;
  }
  if (!prg.down && prg.clicks == 1 && now - prg.releaseAt > 280) { prg.clicks = 0; return P_CLICK; }
  return P_NONE;
}
uint32_t prgHeldMs() { return prg.down ? millis() - prg.t0 : 0; }

uint32_t elapsedMs();

// ---------------- Helpers ----------------
void toast(const String &m, uint16_t ms = 1200) { toastMsg = m; toastUntil = millis() + ms; }
void lockA()   { xSemaphoreTake(audioMux, portMAX_DELAY); }
void unlockA() { xSemaphoreGive(audioMux); }

enum AType { T_NONE, T_MP3, T_AAC, T_FLAC, T_WAV };
AType audioType(const char *n) {
  const char *dot = strrchr(n, '.');
  if (!dot) return T_NONE;
  if (!strcasecmp(dot, ".mp3")) return T_MP3;
  if (!strcasecmp(dot, ".aac")) return T_AAC;
  if (!strcasecmp(dot, ".flac")) return T_FLAC;
  if (!strcasecmp(dot, ".wav")) return T_WAV;
  return T_NONE;
}
const char *typeName(AType t) { const char *n[] = { "", "MP3", "AAC", "FLAC", "WAV" }; return n[t]; }
bool isAudio(const char *n) { return audioType(n) != T_NONE; }
String joinPath(const String &dir, const String &name) { return dir.endsWith("/") ? dir + name : dir + "/" + name; }
String baseName(const String &p) { int i = p.lastIndexOf('/'); return i >= 0 ? p.substring(i + 1) : p; }
String parentDir(const String &p) { int i = p.lastIndexOf('/'); return i > 0 ? p.substring(0, i) : String("/"); }
String stripExt(const String &n) { int i = n.lastIndexOf('.'); return i > 0 ? n.substring(0, i) : n; }
void applyVolume() { if (out) out->SetGain((volume / 20.0f) * 1.2f * fadeGain); }
void applyEQ() { if (out) { lockA(); out->setEQ(eqBass, eqTreble); unlockA(); } }

void saveSettings() {
  prefs.putInt("vol", volume); prefs.putInt("mode", playMode); prefs.putInt("scr", screenIdx);
  prefs.putInt("bri", brightIdx); prefs.putBool("res", resumeOn); prefs.putBool("vis", visOn);
  prefs.putInt("bass", eqBass); prefs.putInt("treb", eqTreble);
  prefs.putBool("conn", connectAtStart);
}
void loadSettings() {
  volume = constrain(prefs.getInt("vol", 12), 0, 20);
  playMode = constrain(prefs.getInt("mode", 0), 0, MODE_COUNT - 1);
  screenIdx = constrain(prefs.getInt("scr", 1), 0, 3);
  brightIdx = constrain(prefs.getInt("bri", 2), 0, 2);
  resumeOn = prefs.getBool("res", true); visOn = prefs.getBool("vis", true);
  eqBass = constrain(prefs.getInt("bass", 0), -6, 6); eqTreble = constrain(prefs.getInt("treb", 0), -6, 6);
  connectAtStart = prefs.getBool("conn", true);
}

#include "bt.h"

// ---------------- Battery ----------------
void readBattery() {
  digitalWrite(PIN_ADC_CTRL, HIGH); delay(3);
  uint32_t mv = 0; for (int i = 0; i < 8; i++) mv += analogReadMilliVolts(PIN_VBAT);
  digitalWrite(PIN_ADC_CTRL, LOW);
  battMv = (int)(mv / 8 * 4.9f * 1.045f);
  if (battMv < 2600) { battPct = -1; return; }          // no battery (USB only)
  const int v[] = { 3300, 3500, 3600, 3700, 3750, 3800, 3870, 3950, 4050, 4150 };
  const int p[] = { 0, 5, 12, 25, 35, 45, 58, 72, 87, 100 };
  battPct = 0;
  for (int i = 0; i < 10; i++) if (battMv >= v[i]) battPct = p[i];
}

// ---------------- SD ----------------
bool mountSD() {
  lockA();
  SD.end(); sdSPI.end();
  sdSPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  bool ok = SD.begin(PIN_SD_CS, sdSPI, 25000000);
  if (!ok) ok = SD.begin(PIN_SD_CS, sdSPI, 4000000);
  unlockA();
  return ok && SD.cardType() != CARD_NONE;
}

int cmpEntry(const void *a, const void *b) {
  const Entry *x = (const Entry *)a, *y = (const Entry *)b;
  if (x->dir != y->dir) return x->dir ? -1 : 1;
  return strcasecmp(x->name, y->name);
}

void loadDir(const String &dir) {
  entryCount = 0; dirArena.clear();
  lockA(); File d = SD.open(dir); unlockA();
  if (d && d.isDirectory()) {
    for (;;) {
      lockA(); File f = d.openNextFile(); unlockA();    // one entry at a time keeps music smooth
      if (!f) break;
      String n = baseName(String(f.name())); bool isDir = f.isDirectory(); uint32_t sz = isDir ? 0 : f.size();
      lockA(); f.close(); unlockA();
      if (n.startsWith(".") || n == "System Volume Information") continue;
      if (!isDir && !isAudio(n.c_str())) continue;
      const char *p = dirArena.add(n.c_str());
      if (!p || entryCount >= MAX_ENTRIES) break;
      entries[entryCount++] = { p, isDir, sz };
    }
    lockA(); d.close(); unlockA();
  }
  qsort(entries, entryCount, sizeof(Entry), cmpEntry);
  curDir = dir; listSel = 0; listTop = 0;
}

void buildPlaylistFromCurrent() {
  plArena.clear(); plCount = 0; plDir = curDir; plFromIndex = false;
  plLabel = curDir == "/" ? String("SD card") : baseName(curDir);
  for (int i = 0; i < entryCount && plCount < MAX_PL; i++) if (!entries[i].dir) {
    const char *p = plArena.add(joinPath(curDir, entries[i].name).c_str());
    if (!p) break;
    playlist[plCount++] = p;
  }
}
void buildShuffle(int startIdx) {
  for (int i = 0; i < plCount; i++) shuffleOrder[i] = i;
  for (int i = plCount - 1; i > 0; i--) { int j = esp_random() % (i + 1); int t = shuffleOrder[i]; shuffleOrder[i] = shuffleOrder[j]; shuffleOrder[j] = t; }
  for (int i = 0; i < plCount; i++) if (shuffleOrder[i] == startIdx) { shuffleOrder[i] = shuffleOrder[0]; shuffleOrder[0] = startIdx; break; }
  shufflePos = 0;
}

void stopTrack();
void drawProgressScreen(const char *title, const String &line);

// whole-card index for "Shuffle all music" and search
bool buildIndex(bool showProgress) {
  if (!sdOk) return false;
  if (plFromIndex) { stopTrack(); plCount = 0; plFromIndex = false; }
  idxArena.clear(); idxCount = 0; idxReady = false;
  String stack[24]; int sp = 0; stack[sp++] = "/";
  uint32_t lastDraw = 0;
  while (sp > 0 && idxCount < MAX_PL) {
    String dir = stack[--sp];
    lockA(); File d = SD.open(dir); unlockA();
    if (!d) continue;
    for (;;) {
      lockA(); File f = d.openNextFile(); unlockA();
      if (!f) break;
      String n = baseName(String(f.name())); bool isDir = f.isDirectory();
      lockA(); f.close(); unlockA();
      if (n.startsWith(".") || n == "System Volume Information") continue;
      String full = joinPath(dir, n);
      if (isDir) { if (sp < 24) stack[sp++] = full; }
      else if (isAudio(n.c_str())) {
        const char *p = idxArena.add(full.c_str());
        if (!p) break;
        libIndex[idxCount++] = p;
        if (idxCount >= MAX_PL) break;
      }
      if (showProgress && millis() - lastDraw > 150) { lastDraw = millis(); drawProgressScreen("Scanning card", String(idxCount) + " songs found"); }
    }
    lockA(); d.close(); unlockA();
  }
  idxReady = true;
  return idxCount > 0;
}

// ---------------- Playback ----------------
void id3Callback(void *, const char *type, bool isUnicode, const char *str) {
  if (isUnicode || !str || !*str) return;
  if (!strcmp(type, "Title")) strlcpy(tagTitle, str, sizeof(tagTitle));
  else if ((!strcmp(type, "Artist") || !strcmp(type, "Band")) && !tagArtist[0]) strlcpy(tagArtist, str, sizeof(tagArtist));
}
void icyCallback(void *, const char *type, bool, const char *str) {
  if (!strcmp(type, "StreamTitle") && str) strlcpy(streamTitle, str, sizeof(streamTitle));
}

void stopTrackLocked() {
  if (gen) { if (gen->isRunning()) gen->stop(); delete gen; gen = nullptr; }
  if (bsrc) { delete bsrc; bsrc = nullptr; }
  if (id3)  { delete id3;  id3  = nullptr; }
  if (fsrc) { delete fsrc; fsrc = nullptr; }
  if (icy)  { delete icy;  icy  = nullptr; }
  srcType = SRC_NONE;
}
void stopTrack() { lockA(); stopTrackLocked(); unlockA(); isPlaying = false; isPaused = false; stPlaying = -1; digitalWrite(PIN_LED, LOW); }

AudioGenerator *makeGen(AType t) {
  switch (t) {
    case T_AAC:  return new AudioGeneratorAAC();
    case T_FLAC: return new AudioGeneratorFLAC();
    case T_WAV:  return new AudioGeneratorWAV();
    default:     return new AudioGeneratorMP3();
  }
}

bool startFile(const char *path) {
  AType t = audioType(path);
  lockA();
  stopTrackLocked();
  tagTitle[0] = tagArtist[0] = streamTitle[0] = 0;
  fsrc = new AudioFileSourceSD(path);
  if (!fsrc->isOpen()) { stopTrackLocked(); unlockA(); toast("Can't open file"); return false; }
  AudioFileSource *s = fsrc;
  if (t == T_MP3 || t == T_AAC) { id3 = new AudioFileSourceID3(fsrc); id3->RegisterMetadataCB(id3Callback, nullptr); s = id3; }
  bsrc = fileBuf ? new AudioFileSourceBuffer(s, fileBuf, FILE_BUF) : new AudioFileSourceBuffer(s, 8192);
  gen = makeGen(t);
  applyVolume();
  bool ok = gen->begin(bsrc, out);
  srcType = SRC_FILE; trackEnded = false;
  unlockA();
  if (!ok) { stopTrack(); toast(String("Bad ") + typeName(t)); return false; }
  strlcpy(nowFmt, typeName(t), sizeof(nowFmt));
  strlcpy(nowFile, baseName(String(path)).c_str(), sizeof(nowFile));
  return true;
}

bool playIndex(int idx) {
  if (idx < 0 || idx >= plCount) return false;
  stPlaying = -1;
  if (!startFile(playlist[idx])) { isPlaying = false; return false; }
  plIndex = idx; isPlaying = true; isPaused = false;
  playStartMs = millis(); pausedAccum = 0;
  if (resumeOn) prefs.putString("rpath", playlist[idx]);
  digitalWrite(PIN_LED, HIGH);
  return true;
}

void drawConnecting(const String &name);
bool playStation(int i) {
  if (i < 0 || i >= stCount) return false;
  if (WiFi.status() != WL_CONNECTED) { toast("Needs home Wi-Fi", 2000); return false; }
  drawConnecting(stName[i]);
  String url = stUrl[i]; String lu = url; lu.toLowerCase();
  lockA();
  stopTrackLocked();
  tagTitle[0] = tagArtist[0] = streamTitle[0] = 0;
  icy = new AudioFileSourceICYStream();
  icy->RegisterMetadataCB(icyCallback, nullptr);
  icy->SetReconnect(3, 500);
  bool ok = icy->open(url.c_str());
  if (ok) {
    bsrc = radioBuf ? new AudioFileSourceBuffer(icy, radioBuf, RADIO_BUF) : new AudioFileSourceBuffer(icy, 16384);
    bool aac = lu.indexOf("aac") >= 0;
    gen = aac ? (AudioGenerator *)new AudioGeneratorAAC() : (AudioGenerator *)new AudioGeneratorMP3();
    applyVolume();
    ok = gen->begin(bsrc, out);
    strlcpy(nowFmt, aac ? "AAC" : "MP3", sizeof(nowFmt));
    srcType = SRC_RADIO; streamDropped = false;
  }
  unlockA();
  if (!ok) { stopTrack(); toast("Station not reachable", 2000); return false; }
  stPlaying = i; isPlaying = true; isPaused = false; playStartMs = millis(); pausedAccum = 0; radioRetries = 0;
  strlcpy(nowFile, stName[i].c_str(), sizeof(nowFile));
  prefs.putInt("lastSt", i);
  digitalWrite(PIN_LED, HIGH);
  return true;
}

void togglePause() {
  if (!isPlaying) return;
  isPaused = !isPaused;
  if (isPaused) pauseStartMs = millis(); else pausedAccum += millis() - pauseStartMs;
  digitalWrite(PIN_LED, isPaused ? LOW : HIGH);
}
uint32_t elapsedMs() {
  if (!isPlaying) return 0;
  uint32_t now = isPaused ? pauseStartMs : millis();
  return now - playStartMs - pausedAccum;
}

int nextIndex(bool user) {
  if (plCount == 0) return -1;
  if (playMode == MODE_REPEAT_ONE && !user) return plIndex;
  if (playMode == MODE_SHUFFLE) { shufflePos++; if (shufflePos >= plCount) buildShuffle(-1); return shuffleOrder[shufflePos]; }
  int n = plIndex + 1;
  if (n >= plCount) return (playMode == MODE_NORMAL && !user) ? -1 : 0;
  return n;
}
int prevIndex() {
  if (plCount == 0) return -1;
  if (playMode == MODE_SHUFFLE) { if (shufflePos > 0) shufflePos--; return shuffleOrder[shufflePos]; }
  return plIndex > 0 ? plIndex - 1 : plCount - 1;
}
void nextTrack(bool user) {
  if (stPlaying >= 0) { playStation((stPlaying + 1) % stCount); return; }
  int n = nextIndex(user);
  if (n < 0) { stopTrack(); toast("End of list"); return; }
  playIndex(n);
}
void prevTrack() {
  if (stPlaying >= 0) { playStation((stPlaying + stCount - 1) % stCount); return; }
  if (isPlaying && elapsedMs() > 3000) { playIndex(plIndex); return; }
  int p = prevIndex(); if (p >= 0) playIndex(p);
}

// Audio runs on core 1 at high priority; Wi-Fi lives on core 0
void audioTask(void *) {
  for (;;) {
    bool worked = false;
    lockA();
    if (gen && gen->isRunning() && !isPaused) {
      if (!gen->loop()) {
        gen->stop();
        if (srcType == SRC_RADIO) streamDropped = true; else trackEnded = true;
      }
      worked = true;
    }
    unlockA();
    vTaskDelay(worked ? 1 : pdMS_TO_TICKS(10));
  }
}

// ---------------- Radio stations (LittleFS /stations.txt, "Name|URL" per line) ----------------
void defaultStations() {
  const char *d[][2] = {
    { "SomaFM Groove Salad", "http://ice1.somafm.com/groovesalad-128-mp3" },
    { "SomaFM Drone Zone",   "http://ice1.somafm.com/dronezone-128-mp3" },
    { "SomaFM Secret Agent", "http://ice1.somafm.com/secretagent-128-mp3" },
    { "SomaFM Lush",         "http://ice1.somafm.com/lush-128-mp3" },
    { "SomaFM Indie Pop",    "http://ice1.somafm.com/indiepop-128-mp3" },
    { "Radio Paradise",      "http://stream.radioparadise.com/mp3-128" },
  };
  stCount = 0;
  for (auto &s : d) { stName[stCount] = s[0]; stUrl[stCount] = s[1]; stCount++; }
}
void saveStations() {
  if (!fsOk) return;
  File f = LittleFS.open("/stations.txt", "w");
  if (!f) return;
  for (int i = 0; i < stCount; i++) { f.print(stName[i]); f.print('|'); f.println(stUrl[i]); }
  f.close();
}
void loadStations() {
  stCount = 0;
  if (fsOk && LittleFS.exists("/stations.txt")) {
    File f = LittleFS.open("/stations.txt", "r");
    while (f && f.available() && stCount < MAX_STATIONS) {
      String l = f.readStringUntil('\n'); l.trim();
      int p = l.indexOf('|');
      if (p > 0) { stName[stCount] = l.substring(0, p); stUrl[stCount] = l.substring(p + 1); stCount++; }
    }
    if (f) f.close();
  }
  if (stCount == 0) { defaultStations(); saveStations(); }
}
bool addStation(const String &name, const String &url) {
  if (stCount >= MAX_STATIONS || !url.startsWith("http://") || name.isEmpty()) return false;
  stName[stCount] = name; stName[stCount].replace("|", "/"); stUrl[stCount] = url; stCount++;
  saveStations(); return true;
}
void delStation(int i) {
  if (i < 0 || i >= stCount) return;
  if (stPlaying == i) stopTrack(); else if (stPlaying > i) stPlaying--;
  for (int k = i; k < stCount - 1; k++) { stName[k] = stName[k + 1]; stUrl[k] = stUrl[k + 1]; }
  stCount--; saveStations();
}

// ---------------- Drawing ----------------
void drawBattery(int x) {
  if (battPct < 0) return;
  oled.drawFrame(x, 2, 10, 6); oled.drawBox(x + 10, 4, 1, 2);
  int w = (battPct * 8 + 50) / 100; if (w > 0) oled.drawBox(x + 1, 3, w, 4);
}
void drawHeader(const char *title) {
  oled.setFont(u8g2_font_6x10_tf);
  oled.drawStr(0, 8, title);
  int x = 128;
  if (battPct >= 0) { x -= 12; drawBattery(x); x -= 3; } else x -= 1;
  char v[8]; snprintf(v, sizeof(v), "%d", volume);
  x -= oled.getStrWidth(v); oled.drawStr(x, 8, v);
  x -= 8; oled.drawBox(x, 3, 2, 4); oled.drawTriangle(x + 2, 5, x + 5, 1, x + 5, 9);
  if (modeIcons[playMode][0] && stPlaying < 0) { x -= oled.getStrWidth(modeIcons[playMode]) + 3; oled.drawStr(x, 8, modeIcons[playMode]); }
  if (sleepEndMs) { x -= 8; oled.drawStr(x, 8, "z"); }
  if (btConnected()) { x -= 8; oled.setFont(u8g2_font_open_iconic_embedded_1x_t); oled.drawGlyph(x, 9, 74); oled.setFont(u8g2_font_6x10_tf); }
  oled.drawHLine(0, 10, 128);
}

typedef String (*LabelFn)(int i, bool &isDir);
void drawList(int count, int sel, int &top, LabelFn label) {
  const int rows = 5, rowH = 10, y0 = 13;
  if (sel < top) top = sel;
  if (sel >= top + rows) top = sel - rows + 1;
  oled.setFont(u8g2_font_6x10_tf);
  for (int r = 0; r < rows && top + r < count; r++) {
    int i = top + r; int y = y0 + r * rowH;
    bool isDir = false; String s = label(i, isDir);
    if (i == sel) { oled.drawBox(0, y, 122, rowH); oled.setDrawColor(0); }
    int tx = 2;
    if (isDir) { oled.drawFrame(2, y + 3, 7, 5); oled.drawHLine(2, y + 2, 3); tx = 12; }
    int w = oled.getStrWidth(s.c_str()); int maxW = 120 - tx; int off = 0;
    if (i == sel && w > maxW) { int span = w - maxW + 20; off = ((millis() / 40) % (span + 40)); off = off < 20 ? 0 : min(off - 20, w - maxW); }
    oled.setClipWindow(tx, y, 121, y + rowH);
    oled.drawStr(tx - off, y + 8, s.c_str());
    oled.setMaxClipWindow();
    oled.setDrawColor(1);
  }
  if (count > rows) {
    int h = max(4, 50 * rows / count); int yy = 13 + (50 - h) * sel / max(1, count - 1);
    oled.drawVLine(125, 13, 50); oled.drawBox(124, yy, 3, h);
  }
  if (count == 0) oled.drawStr(20, 38, "(empty)");
}

void drawProgressScreen(const char *title, const String &line) {
  oled.clearBuffer();
  oled.setFont(u8g2_font_7x13B_tf); oled.drawStr((128 - oled.getStrWidth(title)) / 2, 26, title);
  oled.setFont(u8g2_font_6x10_tf); oled.drawStr((128 - oled.getStrWidth(line.c_str())) / 2, 44, line.c_str());
  oled.sendBuffer();
}
void drawConnecting(const String &name) { if (!displayAsleep) drawProgressScreen("Tuning in...", name.length() > 20 ? name.substring(0, 20) : name); }

void startFromBrowser(int fileIdxInEntries);
void setEQ(int b, int t); void setSleep(int idx); bool shuffleAll(); void resumeLast(); bool playPath(const String &path);
#include "web.h"

// ---- menus ----
const char *homeItems[] = { "Browse music", "Now playing", "Internet radio", "Resume last", "Shuffle folder", "Shuffle all music",
                            "Bluetooth", "Wi-Fi", "Settings", "About" };
const int homeCount = 10;
enum { H_BROWSE, H_NOW, H_RADIO, H_RESUME, H_SHUF, H_SHUFALL, H_BT, H_WIFI, H_SET, H_ABOUT };
String homeLabel(int i, bool &d) { d = false; return homeItems[i]; }

String browseLabel(int i, bool &d) {
  if (i == 0 && curDir != "/") { d = true; return ".."; }
  int k = curDir != "/" ? i - 1 : i;
  d = entries[k].dir;
  return d ? String(entries[k].name) : stripExt(entries[k].name);
}
int browseCount() { return entryCount + (curDir != "/" ? 1 : 0); }

String radioLabel(int i, bool &d) { d = false; return (i == stPlaying ? "> " : "") + stName[i]; }

const int ctrlCount = 10;
String ctrlLabel(int i, bool &d) {
  d = false;
  switch (i) {
    case 0: return isPaused || !isPlaying ? "Play" : "Pause";
    case 1: return stPlaying >= 0 ? "Next station" : "Next track";
    case 2: return stPlaying >= 0 ? "Previous station" : "Previous track";
    case 3: return "Volume +  (" + String(volume) + ")";
    case 4: return "Volume -  (" + String(volume) + ")";
    case 5: return String("Mode: ") + modeNames[playMode];
    case 6: return "Sleep: " + (sleepIdx ? String(sleepMins[sleepIdx]) + " min" : String("Off"));
    case 7: return "Stop";
    case 8: return "Back to Now playing";
    case 9: return "Browse music";
  }
  return "";
}

const int setCount = 13;
String sgn(int v) { return v > 0 ? "+" + String(v) : String(v); }
String setLabel(int i, bool &d) {
  d = false;
  switch (i) {
    case 0: return "Volume +  (" + String(volume) + ")";
    case 1: return "Volume -  (" + String(volume) + ")";
    case 2: return String("Mode: ") + modeNames[playMode];
    case 3: return "Bass: " + sgn(eqBass) + " dB";
    case 4: return "Treble: " + sgn(eqTreble) + " dB";
    case 5: return "Sleep timer: " + (sleepIdx ? String(sleepMins[sleepIdx]) + " min" : String("Off"));
    case 6: return String("Visualizer: ") + (visOn ? "On" : "Off");
    case 7: return String("Screen off: ") + screenNames[screenIdx];
    case 8: return String("Brightness: ") + brightNames[brightIdx];
    case 9: return String("Resume: ") + (resumeOn ? "On" : "Off");
    case 10: return String("Start: ") + (connectAtStart ? "Wi-Fi QR" : "Home");
    case 11: return "Re-scan SD card";
    case 12: return "Back";
  }
  return "";
}

int btItemCount() { return 2 + btFoundN + 4; }
String btLabel(int i, bool &d) {
  d = false;
  if (i == 0) return !btUart ? String("Module: not wired") : (btConnected() ? String("Headset: connected") : String("Headset: none"));
  if (i == 1) return btScanning ? String("Scanning... (") + btFoundN + ")" : String("Scan for headsets");
  int k = i - 2;
  if (k < btFoundN) { String n = btFound[k].name.length() ? btFound[k].name : btFound[k].mac; return (btIsSaved(btFound[k].mac) ? "* " : "+ ") + n; }
  k -= btFoundN;
  const char *t[] = { "Reconnect", "Forget saved", "Disconnect", "Back" };
  return t[k];
}

const int wifiCount = 6;
String wifiLabel(int i, bool &d) {
  d = false;
  bool sta = WiFi.status() == WL_CONNECTED;
  switch (i) {
    case 0: return String("Wi-Fi: ") + (wifiOn ? "On" : "Off");
    case 1: { String ip = wifiIP(); return "IP: " + (wifiOn ? (ip.length() ? ip : String("connecting...")) : String("-")); }
    case 2: return sta ? "Net: " + WiFi.SSID() : (apActive ? "Hotspot: " + apName : (wifiSsid.length() ? "Joining: " + wifiSsid : String("Net: none")));
    case 3: return apActive && !sta ? String("Pass: heltecmp3") : String("heltec-mp3.local");
    case 4: return "Forget home Wi-Fi";
    case 5: return "Back";
  }
  return "";
}

void drawVU(int x, int y) {
  // two 3px bars, 10px tall, log-ish scale
  uint16_t lv[2] = { vuL, vuR };
  for (int c = 0; c < 2; c++) {
    int h = lv[c] < 60 ? 0 : (int)(10 * (log10f(lv[c]) - 1.78f) / (4.52f - 1.78f));
    h = constrain(h, 0, 10);
    oled.drawFrame(x + c * 5, y, 4, 11);
    if (h) oled.drawBox(x + c * 5 + 1, y + 10 - h, 2, h);
  }
}

void drawNowPlaying() {
  bool radio = stPlaying >= 0;
  drawHeader(isPlaying ? (isPaused ? "Paused" : (radio ? "Radio" : "Playing")) : "Stopped");
  oled.setFont(u8g2_font_7x13B_tf);
  String title = radio ? String(nowFile) : (tagTitle[0] ? String(tagTitle) : (nowFile[0] ? stripExt(String(nowFile)) : String("Nothing playing")));
  int w = oled.getStrWidth(title.c_str()); int off = 0;
  if (w > 128) { int span = w - 128; off = (millis() / 35) % (span + 80); off = off < 40 ? 0 : min(off - 40, span); }
  oled.setClipWindow(0, 12, 127, 27); oled.drawStr(-off, 25, title.c_str()); oled.setMaxClipWindow();

  oled.setFont(u8g2_font_6x10_tf);
  String sub = radio ? (streamTitle[0] ? String(streamTitle) : String("Live stream")) : (tagArtist[0] ? String(tagArtist) : plLabel);
  int sw = oled.getStrWidth(sub.c_str()); int so = 0;
  if (sw > 128) { int span = sw - 128; so = (millis() / 45) % (span + 80); so = so < 40 ? 0 : min(so - 40, span); }
  oled.setClipWindow(0, 29, 127, 40); oled.drawStr(-so, 38, sub.c_str()); oled.setMaxClipWindow();

  float prog = 0;
  lockA();
  if (!radio && isPlaying && fsrc && fsrc->getSize() > 0) prog = (float)fsrc->getPos() / fsrc->getSize();
  if (radio && bsrc) prog = (float)bsrc->getFillLevel() / RADIO_BUF;
  unlockA();
  oled.drawFrame(0, 43, 128, 6);
  if (radio) { for (int x = 2; x < 2 + (int)(124 * constrain(prog, 0.f, 1.f)); x += 2) oled.drawVLine(x, 44, 4); }   // buffer level
  else oled.drawBox(1, 44, (int)(126 * constrain(prog, 0.f, 1.f)), 4);

  uint32_t s = elapsedMs() / 1000; char t[16];
  snprintf(t, sizeof(t), "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
  oled.drawStr(0, 62, t);
  String r = radio ? String("LIVE") : (plCount ? String(plIndex + 1) + "/" + String(plCount) : String(nowFmt));
  oled.drawStr(128 - oled.getStrWidth(r.c_str()), 62, r.c_str());
  if (isPlaying && !isPaused) oled.drawTriangle(52, 53, 52, 63, 60, 58);
  else { oled.drawBox(52, 53, 3, 10); oled.drawBox(57, 53, 3, 10); }
  if (visOn && isPlaying && 128 - oled.getStrWidth(r.c_str()) > 78) drawVU(66, 53);   // skip if the counter is wide
}

void drawAbout() {
  drawHeader("About");
  oled.setFont(u8g2_font_6x10_tf);
  oled.drawStr(0, 23, "Heltec V4 MP3 v" FW_VERSION);
  String l2 = wifiIP().length() ? "IP " + wifiIP() : String("PSRAM ") + (ESP.getPsramSize() / 1024) + " KB";
  oled.drawStr(0, 34, l2.c_str());
  String l3 = battPct >= 0 ? "Battery " + String(battMv / 1000.0, 2) + "V " + battPct + "%" : String("Power: USB");
  oled.drawStr(0, 45, l3.c_str());
  oled.drawStr(0, 56, "hold=OK  long=back");
}

void drawNoSD() {
  oled.setFont(u8g2_font_7x13B_tf); oled.drawStr(18, 14, "No SD card");
  oled.setFont(u8g2_font_5x7_tf);
  oled.drawStr(0, 26, "CS42 MOSI41 CLK40 MISO39");
  oled.drawStr(0, 35, "3.3V + GND, FAT32 card");
  oled.drawStr(0, 44, "Radio works on Wi-Fi");
  oled.setFont(u8g2_font_6x10_tf); oled.drawStr(0, 60, "OK: retry  BACK: home");
}

// ---- Connect screen: QR code + Wi-Fi details, shown at start until a phone/PC opens the page ----
bool connectFromMenu = false;
uint8_t qrBits[33 * 33]; int qrSize = 0; String qrText;
void qrStore(esp_qrcode_handle_t q) {
  int n = esp_qrcode_get_size(q); if (n > 33) { qrSize = 0; return; }
  qrSize = n;
  for (int y = 0; y < n; y++) for (int x = 0; x < n; x++) qrBits[y * n + x] = esp_qrcode_get_module(q, x, y);
}
void makeQR(const String &t) {
  if (t == qrText) return;
  qrText = t; qrSize = 0;
  esp_qrcode_config_t cfg = { qrStore, 4, ESP_QRCODE_ECC_LOW };
  esp_qrcode_generate(&cfg, t.c_str());
}
void drawQR(int x0, int y0) {
  int sc = qrSize * 2 <= 60 ? 2 : 1;
  int box = qrSize * sc + 4;
  oled.drawBox(x0, y0, box, box);                 // light background (quiet zone)
  oled.setDrawColor(0);
  for (int y = 0; y < qrSize; y++) for (int x = 0; x < qrSize; x++)
    if (qrBits[y * qrSize + x]) oled.drawBox(x0 + 2 + x * sc, y0 + 2 + y * sc, sc, sc);
  oled.setDrawColor(1);
}
void drawConnect() {
  bool sta = WiFi.status() == WL_CONNECTED;
  oled.setFont(u8g2_font_5x7_tf);
  // text column starts right of the QR code; every line is kept to what fits (5 px per character)
  auto lines = [](int x, std::initializer_list<String> ls) { int y = 8; for (auto &l : ls) { oled.drawStr(x, y, l.c_str()); y += 8; } };
  auto qrW = []() { return qrSize ? qrSize * (qrSize * 2 <= 60 ? 2 : 1) + 4 : 0; };
  String skip = connectFromMenu ? "PRG: back" : "PRG: skip";
  if (sta) {
    String ip = WiFi.localIP().toString();
    makeQR("http://" + ip + "/");
    if (qrSize) drawQR(0, 1);
    lines(qrW() + 5, { "Scan or open:", ip, "heltec-mp3", "  .local", "", "", skip });
  } else if (apActive) {
    makeQR("WIFI:T:WPA;S:" + apName + ";P:heltecmp3;;");
    if (qrSize) drawQR(0, 1);
    String a = apName.substring(0, 11), b = apName.length() > 11 ? apName.substring(11) : String("");
    lines(qrW() + 4, { "Join Wi-Fi:", a, b, "pw heltecmp3", "then open", "192.168.4.1", skip });
  } else {
    oled.setFont(u8g2_font_7x13B_tf); oled.drawStr(4, 20, "Joining Wi-Fi");
    oled.setFont(u8g2_font_6x10_tf);
    String n = wifiSsid.length() > 20 ? wifiSsid.substring(0, 20) : wifiSsid;
    oled.drawStr(4, 36, n.c_str());
    int dots = (millis() / 400) % 4; String d = ""; for (int i = 0; i < dots; i++) d += ". ";
    oled.drawStr(4, 48, d.c_str());
    oled.setFont(u8g2_font_5x7_tf); oled.drawStr(4, 62, skip.c_str());
  }
}

void render() {
  if (displayAsleep || otaRunning) return;
  oled.clearBuffer();
  switch (screen) {
    case SCR_HOME:     drawHeader("Heltec MP3"); drawList(homeCount, menuSel, menuTop, homeLabel); break;
    case SCR_BROWSE: {
      String t = curDir == "/" ? String("Music") : baseName(curDir);
      if (t.length() > 10) t = t.substring(0, 9) + "~";
      drawHeader(t.c_str()); drawList(browseCount(), listSel, listTop, browseLabel); break; }
    case SCR_NOW:      drawNowPlaying(); break;
    case SCR_CONTROLS: drawHeader("Controls"); drawList(ctrlCount, menuSel, menuTop, ctrlLabel); break;
    case SCR_SETTINGS: drawHeader("Settings"); drawList(setCount, menuSel, menuTop, setLabel); break;
    case SCR_NOSD:     drawNoSD(); break;
    case SCR_INFO:     drawAbout(); break;
    case SCR_BT:       drawHeader("Bluetooth"); drawList(btItemCount(), menuSel, menuTop, btLabel); break;
    case SCR_WIFI:     drawHeader("Wi-Fi"); drawList(wifiCount, menuSel, menuTop, wifiLabel); break;
    case SCR_RADIO:    drawHeader("Radio"); drawList(stCount, menuSel, menuTop, radioLabel); break;
    case SCR_CONNECT:  drawConnect(); break;
  }
  // PRG hold meter + "let go" hint
  uint32_t h = prgHeldMs();
  if (h > 150 && !displayAsleep) {
    int w = min(128, (int)(h * 128 / 1500));
    oled.setDrawColor(0); oled.drawBox(0, 61, 128, 3); oled.setDrawColor(1);
    oled.drawBox(0, 62, w, 2); oled.drawPixel(51, 61);
    if (h >= 600) {
      const char *hint = h >= 1500 ? "Let go: Back" : "Let go: OK";
      oled.setFont(u8g2_font_6x10_tf);
      int tw = oled.getStrWidth(hint) + 10, tx = (128 - tw) / 2;
      oled.setDrawColor(0); oled.drawBox(tx - 3, 43, tw + 6, 18); oled.setDrawColor(1);
      oled.drawRBox(tx, 46, tw, 13, 3);
      oled.setDrawColor(0); oled.drawStr(tx + 5, 56, hint); oled.setDrawColor(1);
    }
  }
  if (millis() < toastUntil) {
    oled.setFont(u8g2_font_6x10_tf);
    int w = oled.getStrWidth(toastMsg.c_str()) + 8; int x = (128 - w) / 2;
    oled.setDrawColor(0); oled.drawBox(x - 1, 24, w + 2, 17); oled.setDrawColor(1);
    oled.drawRFrame(x, 25, w, 15, 3); oled.drawStr(x + 4, 36, toastMsg.c_str());
  }
  oled.sendBuffer();
}

// ---------------- Actions ----------------
void goScreen(Screen s) { screen = s; menuSel = 0; menuTop = 0; }
void openBrowser() { if (!sdOk) { goScreen(SCR_NOSD); return; } screen = SCR_BROWSE; if (entryCount == 0 && curDir == "/") loadDir("/"); }

void startFromBrowser(int k) {
  buildPlaylistFromCurrent();
  String full = joinPath(curDir, entries[k].name);
  int idx = 0;
  for (int i = 0; i < plCount; i++) if (full == playlist[i]) { idx = i; break; }
  if (playMode == MODE_SHUFFLE) buildShuffle(idx);
  if (playIndex(idx)) screen = SCR_NOW;
}
// play one file by full path, with its folder as the playlist
bool playPath(const String &path) {
  if (!sdOk) return false;
  String dir = parentDir(path), name = baseName(path);
  loadDir(dir);
  for (int i = 0; i < entryCount; i++) if (!entries[i].dir && name == entries[i].name) {
    startFromBrowser(i); listSel = i + (curDir != "/" ? 1 : 0); return isPlaying;
  }
  return false;
}
void resumeLast() {
  String p = prefs.getString("rpath", "");
  if (!sdOk || p.isEmpty()) { toast("Nothing to resume"); return; }
  if (!playPath(p)) toast("Track not found");
}
void shuffleFolder() {
  if (!sdOk) { goScreen(SCR_NOSD); return; }
  if (entryCount == 0) loadDir(curDir);
  buildPlaylistFromCurrent();
  if (plCount == 0) { toast("No music here"); return; }
  playMode = MODE_SHUFFLE; saveSettings();
  buildShuffle(esp_random() % plCount);
  if (playIndex(shuffleOrder[0])) screen = SCR_NOW;
}
bool shuffleAll() {
  if (!sdOk) { goScreen(SCR_NOSD); return false; }
  if (!buildIndex(true)) { toast("No music found"); return false; }
  plFromIndex = true; plCount = idxCount; plDir = "/"; plLabel = "All music";
  for (int i = 0; i < idxCount; i++) playlist[i] = libIndex[i];
  playMode = MODE_SHUFFLE; saveSettings();
  buildShuffle(esp_random() % plCount);
  bool ok = playIndex(shuffleOrder[0]);
  if (ok) screen = SCR_NOW;
  return ok;
}
void changeVolume(int d) { volume = constrain(volume + d, 0, 20); lockA(); applyVolume(); unlockA(); prefs.putInt("vol", volume); }
void cycleMode() { playMode = (playMode + 1) % MODE_COUNT; if (playMode == MODE_SHUFFLE && plCount) buildShuffle(plIndex); saveSettings(); toast(modeNames[playMode]); }
void setSleep(int idx) {
  sleepIdx = constrain(idx, 0, 5);
  sleepEndMs = sleepIdx ? millis() + sleepMins[sleepIdx] * 60000UL : 0;
  if (fadeGain < 1) { fadeGain = 1; lockA(); applyVolume(); unlockA(); }
}
void cycleSleep() { setSleep((sleepIdx + 1) % 6); toast(sleepIdx ? "Sleep in " + String(sleepMins[sleepIdx]) + " min" : String("Sleep off")); }
void setEQ(int b, int t) { eqBass = constrain(b, -6, 6); eqTreble = constrain(t, -6, 6); applyEQ(); saveSettings(); }
void cycleEQ(int &v) { v += 2; if (v > 6) v = -6; applyEQ(); saveSettings(); }

void moveSel(int &sel, int count, int d) { if (count == 0) return; sel += d; if (sel < 0) sel = count - 1; if (sel >= count) sel = 0; }

void handle(InputEvent e) {
  Ev ev = e.ev;
  switch (screen) {
    case SCR_HOME:
      if (ev == EV_UP || ev == EV_UP_LONG) moveSel(menuSel, homeCount, -1);
      else if (ev == EV_DOWN || ev == EV_DOWN_LONG) moveSel(menuSel, homeCount, 1);
      else if (ev == EV_OK) {
        switch (menuSel) {
          case H_BROWSE: openBrowser(); break;
          case H_NOW: screen = SCR_NOW; break;
          case H_RADIO: goScreen(SCR_RADIO); if (stPlaying >= 0) menuSel = stPlaying; break;
          case H_RESUME: resumeLast(); break;
          case H_SHUF: shuffleFolder(); break;
          case H_SHUFALL: shuffleAll(); break;
          case H_BT: goScreen(SCR_BT); if (btUart) btRefreshSaved(); break;
          case H_WIFI: goScreen(SCR_WIFI); break;
          case H_SET: goScreen(SCR_SETTINGS); break;
          case H_ABOUT: screen = SCR_INFO; break;
        }
      }
      break;

    case SCR_BROWSE: {
      int cnt = browseCount();
      if (ev == EV_UP) moveSel(listSel, cnt, -1);
      else if (ev == EV_DOWN) moveSel(listSel, cnt, 1);
      else if (ev == EV_UP_LONG) listSel = max(0, listSel - 5);
      else if (ev == EV_DOWN_LONG) listSel = min(cnt - 1, listSel + 5);
      else if (ev == EV_OK && cnt) {
        if (curDir != "/" && listSel == 0) { loadDir(parentDir(curDir)); break; }
        int k = curDir != "/" ? listSel - 1 : listSel;
        if (entries[k].dir) loadDir(joinPath(curDir, entries[k].name));
        else startFromBrowser(k);
      } else if (ev == EV_BACK) {
        if (curDir == "/") goScreen(SCR_HOME); else loadDir(parentDir(curDir));
      } else if (ev == EV_OK_LONG) screen = isPlaying ? SCR_NOW : SCR_HOME;
      break; }

    case SCR_RADIO:
      if (ev == EV_UP || ev == EV_UP_LONG) moveSel(menuSel, stCount, -1);
      else if (ev == EV_DOWN || ev == EV_DOWN_LONG) moveSel(menuSel, stCount, 1);
      else if (ev == EV_OK && stCount) { if (playStation(menuSel)) screen = SCR_NOW; }
      else if (ev == EV_BACK) { goScreen(SCR_HOME); menuSel = H_RADIO; }
      break;

    case SCR_NOW:
      if (e.prg) {
        if (ev == EV_DOWN) nextTrack(true);
        else if (ev == EV_UP) togglePause();
        else if (ev == EV_OK) goScreen(SCR_CONTROLS);
        else if (ev == EV_BACK) goScreen(SCR_HOME);
      } else {
        if (ev == EV_UP) changeVolume(1);
        else if (ev == EV_DOWN) changeVolume(-1);
        else if (ev == EV_OK) { if (isPlaying) togglePause(); else if (plCount) playIndex(max(0, plIndex)); }
        else if (ev == EV_UP_LONG) nextTrack(true);
        else if (ev == EV_DOWN_LONG) prevTrack();
        else if (ev == EV_OK_LONG) goScreen(SCR_CONTROLS);
        else if (ev == EV_BACK) { if (stPlaying >= 0) { goScreen(SCR_RADIO); menuSel = stPlaying; } else openBrowser(); }
      }
      break;

    case SCR_CONTROLS:
      if (ev == EV_UP || ev == EV_UP_LONG) moveSel(menuSel, ctrlCount, -1);
      else if (ev == EV_DOWN || ev == EV_DOWN_LONG) moveSel(menuSel, ctrlCount, 1);
      else if (ev == EV_BACK) screen = SCR_NOW;
      else if (ev == EV_OK) {
        switch (menuSel) {
          case 0: if (isPlaying) togglePause(); else if (plCount) playIndex(max(0, plIndex)); break;
          case 1: nextTrack(true); break;
          case 2: prevTrack(); break;
          case 3: changeVolume(1); break;
          case 4: changeVolume(-1); break;
          case 5: cycleMode(); break;
          case 6: cycleSleep(); break;
          case 7: stopTrack(); screen = SCR_NOW; break;
          case 8: screen = SCR_NOW; break;
          case 9: openBrowser(); break;
        }
      }
      break;

    case SCR_SETTINGS:
      if (ev == EV_UP || ev == EV_UP_LONG) moveSel(menuSel, setCount, -1);
      else if (ev == EV_DOWN || ev == EV_DOWN_LONG) moveSel(menuSel, setCount, 1);
      else if (ev == EV_BACK) { goScreen(SCR_HOME); menuSel = H_SET; }
      else if (ev == EV_OK) {
        switch (menuSel) {
          case 0: changeVolume(1); break;
          case 1: changeVolume(-1); break;
          case 2: cycleMode(); break;
          case 3: cycleEQ(eqBass); break;
          case 4: cycleEQ(eqTreble); break;
          case 5: cycleSleep(); break;
          case 6: visOn = !visOn; saveSettings(); break;
          case 7: screenIdx = (screenIdx + 1) % 4; saveSettings(); break;
          case 8: brightIdx = (brightIdx + 1) % 3; oled.setContrast(brightVals[brightIdx]); saveSettings(); break;
          case 9: resumeOn = !resumeOn; saveSettings(); break;
          case 10: connectAtStart = !connectAtStart; saveSettings(); break;
          case 11: stopTrack(); sdOk = mountSD(); entryCount = 0; curDir = "/"; idxReady = false; plCount = 0;
                   toast(sdOk ? "SD card OK" : "No SD card"); break;
          case 12: goScreen(SCR_HOME); menuSel = H_SET; break;
        }
      }
      break;

    case SCR_NOSD:
      if (ev == EV_OK) {
        toast("Mounting..."); render();
        sdOk = mountSD();
        if (sdOk) { loadDir("/"); screen = SCR_BROWSE; toast("SD card OK"); } else toast("Still no card");
      } else if (ev == EV_BACK) goScreen(SCR_HOME);
      break;

    case SCR_INFO: if (ev != EV_NONE) { goScreen(SCR_HOME); menuSel = H_ABOUT; } break;

    case SCR_CONNECT:
      if (ev != EV_NONE) { if (connectFromMenu) { goScreen(SCR_WIFI); menuSel = 1; } else goScreen(SCR_HOME); connectFromMenu = false; }
      break;

    case SCR_BT: {
      int cnt = btItemCount();
      if (ev == EV_UP || ev == EV_UP_LONG) moveSel(menuSel, cnt, -1);
      else if (ev == EV_DOWN || ev == EV_DOWN_LONG) moveSel(menuSel, cnt, 1);
      else if (ev == EV_BACK) { goScreen(SCR_HOME); menuSel = H_BT; }
      else if (ev == EV_OK) {
        int i = menuSel, k = i - 2 - btFoundN;
        if (i == cnt - 1) { goScreen(SCR_HOME); menuSel = H_BT; break; }
        if (!btUart) {
          if (i == 0) { btBegin(); toast(btUart ? "Module found" : "No reply - check wires", 1800); }
          else toast("Wire module TX/RX", 1800);
          break;
        }
        if (i == 0) { btCmd("AT+STATUS?"); toast(btVer.length() ? btVer : String("KCX module")); }
        else if (i == 1) { btScan(); toast("Scanning ~15 s", 1500); }
        else if (i - 2 < btFoundN) { btConnectTo(btFound[i - 2].mac); toast("Connecting...", 1500); }
        else if (k == 0) { btReconnect(); toast("Searching...", 1500); }
        else if (k == 1) { btForgetAll(); toast("Forgot all", 1500); }
        else if (k == 2) { btDisconnect(); toast("Disconnected", 1500); }
      }
      break; }

    case SCR_WIFI:
      if (ev == EV_UP || ev == EV_UP_LONG) moveSel(menuSel, wifiCount, -1);
      else if (ev == EV_DOWN || ev == EV_DOWN_LONG) moveSel(menuSel, wifiCount, 1);
      else if (ev == EV_BACK) { goScreen(SCR_HOME); menuSel = H_WIFI; }
      else if (ev == EV_OK) {
        if (menuSel == 0) { wifiOn = !wifiOn; prefs.putBool("wifi", wifiOn); wifiStart(); toast(wifiOn ? "Wi-Fi on" : "Wi-Fi off"); }
        else if ((menuSel == 1 || menuSel == 2 || menuSel == 3) && wifiOn) { connectFromMenu = true; screen = SCR_CONNECT; }
        else if (menuSel == 4) {
          wifiSsid = ""; wifiPass = ""; prefs.putString("wssid", ""); prefs.putString("wpass", "");
          if (wifiOn) { WiFi.disconnect(); wifiStart(); }
          toast("Hotspot mode", 1500);
        } else if (menuSel == 5) { goScreen(SCR_HOME); menuSel = H_WIFI; }
      }
      break;
  }
}

InputEvent readInput() {
  switch (pollPrg()) {
    case P_CLICK:  return {EV_DOWN, true};
    case P_DOUBLE: return {EV_UP, true};
    case P_LONG:   return {EV_OK, true};
    case P_XLONG:  return {EV_BACK, true};
    default: break;
  }
  Ev e;
  if ((e = pollBtn(bUp,   EV_UP,   EV_UP_LONG))   != EV_NONE) return {e, false};
  if ((e = pollBtn(bDown, EV_DOWN, EV_DOWN_LONG)) != EV_NONE) return {e, false};
  if ((e = pollBtn(bOk,   EV_OK,   EV_OK_LONG))   != EV_NONE) return {e, false};
  if ((e = pollBtn(bBack, EV_BACK, EV_BACK))      != EV_NONE) return {e, false};
  uint32_t now = millis();
  if (screen != SCR_NOW) {
    if (bUp.down && bUp.longFired && now - bUp.lastRep > 120)       { bUp.lastRep = now;   return {EV_UP, false}; }
    if (bDown.down && bDown.longFired && now - bDown.lastRep > 120) { bDown.lastRep = now; return {EV_DOWN, false}; }
  }
  return {EV_NONE, false};
}

// ---------------- Setup / loop ----------------
void setup() {
  Serial.begin(115200);
  pinMode(PIN_VEXT, OUTPUT); digitalWrite(PIN_VEXT, LOW);
  pinMode(PIN_FEM_POWER, OUTPUT); digitalWrite(PIN_FEM_POWER, LOW);     // LoRa PA unpowered
  pinMode(PIN_LED, OUTPUT); digitalWrite(PIN_LED, LOW);
  pinMode(PIN_ADC_CTRL, OUTPUT); digitalWrite(PIN_ADC_CTRL, LOW);
  analogSetPinAttenuation(PIN_VBAT, ADC_2_5db);
  pinMode(PIN_PRG, INPUT_PULLUP);
  pinMode(PIN_BTN_UP, INPUT_PULLUP); pinMode(PIN_BTN_DOWN, INPUT_PULLUP);
  pinMode(PIN_BTN_OK, INPUT_PULLUP); pinMode(PIN_BTN_BACK, INPUT_PULLUP);
  delay(200);   // cold power-up: give the OLED (powered through Vext) time to come up

  prefs.begin("heltecmp3", false);
  loadSettings();

  oled.begin();
  delay(60);
  oled.begin();            // second init: some OLEDs ignore the first one right after power-up
  oled.setBusClock(800000);
  oled.setContrast(brightVals[brightIdx]);
  drawProgressScreen("Heltec V4 MP3", "v" FW_VERSION "  starting...");

  // PSRAM: library tables + big audio buffers
  entries = bigAlloc<Entry>(MAX_ENTRIES);
  playlist = bigAlloc<const char *>(MAX_PL);
  libIndex = bigAlloc<const char *>(MAX_PL);
  shuffleOrder = bigAlloc<int>(MAX_PL);
  dirArena.init(256 * 1024); plArena.init(384 * 1024); idxArena.init(512 * 1024);
  if (psramFound()) { fileBuf = (uint8_t *)ps_malloc(FILE_BUF); radioBuf = (uint8_t *)ps_malloc(RADIO_BUF); }

  fsOk = LittleFS.begin(true);
  loadStations();
  readBattery();

  audioMux = xSemaphoreCreateMutex();
  i2s = new AudioOutputI2S();
  i2s->SetPinout(PIN_I2S_BCK, PIN_I2S_LRCK, PIN_I2S_DIN);
  out = new AudioOutputEQ(i2s);
  out->setEQ(eqBass, eqTreble);
  applyVolume();
  xTaskCreatePinnedToCore(audioTask, "audio", 12288, nullptr, 3, nullptr, 1);

  drawProgressScreen("Heltec V4 MP3", "SD card...");
  for (int i = 0; i < 3 && !sdOk; i++) { if (i) delay(300); sdOk = mountSD(); }   // cold cards can be slow to wake
  if (sdOk) loadDir("/");

  drawProgressScreen("Heltec V4 MP3", "Bluetooth module...");
  btBegin();

  drawProgressScreen("Heltec V4 MP3", "Wi-Fi...");
  wifiOn = prefs.getBool("wifi", true);
  wifiSsid = prefs.getString("wssid", "");
  wifiPass = prefs.getString("wpass", "");
  wifiStart();

  screen = (wifiOn && connectAtStart) ? SCR_CONNECT : SCR_HOME;
  webHit = false;
  lastInputMs = millis();
  Serial.printf("Heltec V4 MP3 %s  PSRAM:%u  SD:%s  BT module:%s\n", FW_VERSION, (unsigned)ESP.getPsramSize(), sdOk ? "ok" : "missing", btUart ? "ok" : "not found");
}

uint32_t lastDraw = 0;
void loop() {
  InputEvent e = readInput();
  if (e.ev != EV_NONE) {
    lastInputMs = millis();
    if (displayAsleep) { displayAsleep = false; oled.setPowerSave(0); }
    else handle(e);
    lastDraw = 0;
  }

  btPoll();
  wifiTick();
  if (screen == SCR_CONNECT && webHit && !connectFromMenu) {
    webHit = false; goScreen(SCR_HOME); toast("Connected!", 1500);
  }
  static uint32_t nextSdTry = 10000;
  if (!sdOk && millis() > nextSdTry) {        // card inserted later, or slow to start: keep trying
    nextSdTry = millis() + 10000;
    if ((sdOk = mountSD())) { loadDir("/"); toast("SD card found", 1500); if (screen == SCR_NOSD) goScreen(SCR_HOME); }
  }
  if (libChanged) { libChanged = false; idxReady = false; if (sdOk) { int s = listSel; loadDir(curDir); listSel = min(s, max(0, browseCount() - 1)); } }
  if (trackEnded) { trackEnded = false; nextTrack(false); }

  // radio stream dropped: retry a few times
  if (streamDropped && stPlaying >= 0) {
    if (!radioRetryAt) radioRetryAt = millis() + 2000;
    else if (millis() > radioRetryAt) {
      radioRetryAt = 0; streamDropped = false;
      int st = stPlaying;
      if (++radioRetries <= 3) { int r = radioRetries; if (playStation(st)) radioRetries = r; }
      else { stopTrack(); toast("Stream lost", 2000); }
    }
  }

  // sleep timer: fade out over the last 15 s, then pause
  if (sleepEndMs) {
    int32_t left = (int32_t)(sleepEndMs - millis());
    if (left <= 0) { if (isPlaying && !isPaused) togglePause(); sleepEndMs = 0; sleepIdx = 0; fadeGain = 1; lockA(); applyVolume(); unlockA(); }
    else if (left < 15000) { fadeGain = left / 15000.0f; lockA(); applyVolume(); unlockA(); }
  }

  // VU meter: grab and decay peaks
  if (out) {
    uint16_t pl = out->peakL, pr = out->peakR; out->peakL = out->peakR = 0;
    vuL = max<uint16_t>(pl, vuL * 0.8f); vuR = max<uint16_t>(pr, vuR * 0.8f);
  }

  if (millis() > battNext) { battNext = millis() + 10000; readBattery(); }

  uint16_t to = screenTimes[screenIdx];
  if (to && !displayAsleep && screen != SCR_CONNECT && millis() - lastInputMs > to * 1000UL) { displayAsleep = true; oled.setPowerSave(1); }
  if (!displayAsleep && millis() - lastDraw > 50) { lastDraw = millis(); render(); }
  delay(2);
}
