// ---------------- KCX_BT_EMITTER (Bluetooth headset transmitter) ----------------
// The ESP32-S3 has no Classic Bluetooth, so the KCX module does the A2DP work.
// We talk to it with its serial "AT" commands (firmware V1.1 / V1.4 / V1.7):
//   AT+          alive check        -> OK+
//   AT+GMR?      version            -> OK+VERS:KCX_BT_RTX_V1.4
//   AT+STATUS?   connected?         -> OK+STATUS:0 / 1
//   AT+SCAN      list headsets      -> New Devices:1,MacAdd:0x..,Name:..
//   AT+ADDLINKADD=<12 hex>          remember a headset by MAC (up to 10)
//   AT+ADDLINKNAME=<name>           remember a headset by name (up to 10)
//   AT+VMLINK?   saved list         -> MEM_MacAdd 00:... / MEM_Name 00:...
//   AT+DELVMLINK forget all saved   (then it connects to any headset)
//   AT+PAIR      drop link, search & connect again
//   AT+DISCON    disconnect
#pragma once

HardwareSerial BTSerial(1);
bool     btUart = false;          // module answered on the serial wires
uint32_t btBaud = 0;
String   btVer;
int      btStatus = -1;           // -1 unknown, 0 no headset, 1 connected
bool     btScanning = false;
uint32_t btScanStart = 0;
uint32_t btLastLow = 0;           // LINK pin last seen low
uint32_t btLastStatusPoll = 0;

struct BtDev { String mac, name; };
#define BT_MAX_FOUND 16
#define BT_MAX_SAVED 20
BtDev btFound[BT_MAX_FOUND]; int btFoundN = 0;
BtDev btSaved[BT_MAX_SAVED]; int btSavedN = 0;
String btPendingMac;

#define BT_LOG_N 60
String   btLog[BT_LOG_N]; int btLogHead = 0, btLogCount = 0;
String   btLine;

#define BT_Q_N 10
String   btQ[BT_Q_N]; bool btQQuiet[BT_Q_N]; int btQHead = 0, btQCount = 0;
uint32_t btLastSend = 0;
bool     btQuietReply = false;

void btLogAdd(const String &s) {
  btLog[btLogHead] = s; btLogHead = (btLogHead + 1) % BT_LOG_N;
  if (btLogCount < BT_LOG_N) btLogCount++;
}

// queue a command; the module needs ~250 ms between commands
void btCmd(const String &c, bool quiet = false) {
  if (!btUart || btQCount >= BT_Q_N) return;
  int i = (btQHead + btQCount) % BT_Q_N;
  btQ[i] = c; btQQuiet[i] = quiet; btQCount++;
}

bool btConnected() {
  if (btStatus == 1) return true;
  if (btStatus == 0 && btUart) return false;
  return millis() - btLastLow > 1500;     // LINK pin steady high = connected
}

String btFoundName(const String &mac) {
  for (int i = 0; i < btFoundN; i++) if (btFound[i].mac == mac) return btFound[i].name;
  return "";
}

static String hex12(const String &s, int from) {
  String m;
  for (int i = from; i < (int)s.length() && m.length() < 12; i++) {
    char c = tolower(s[i]);
    if (isxdigit(c)) m += c; else break;
  }
  return m.length() == 12 ? m : String("");
}

void btAddFound(const String &mac, const String &name) {
  if (mac.isEmpty()) return;
  for (int i = 0; i < btFoundN; i++) if (btFound[i].mac == mac) { if (name.length()) btFound[i].name = name; return; }
  if (btFoundN < BT_MAX_FOUND) { btFound[btFoundN].mac = mac; btFound[btFoundN].name = name; btFoundN++; }
}

void btParse(String l) {
  l.trim();
  if (l.isEmpty()) return;
  bool quiet = btQuietReply && (l.startsWith("OK+STATUS") || l == "OK+");
  if (!quiet && !l.startsWith("SCAN....")) btLogAdd("< " + l);
  if (l.startsWith("OK+STATUS")) { btStatus = l.endsWith("1") ? 1 : 0; btQuietReply = false; return; }
  if (l.startsWith("OK+VERS")) { int p = l.indexOf(':'); btVer = p > 0 ? l.substring(p + 1) : l; btVer.trim(); return; }
  if (l.startsWith("OK+SCAN")) { btScanning = true; btScanStart = millis(); return; }
  if (l.startsWith("OK+VMLINK") || l.startsWith("OK+ADDLINK")) { btSavedN = 0; return; }
  if (l.startsWith("Delete_Vmlink")) { btSavedN = 0; return; }
  if (l.startsWith("POWER ON")) { btStatus = -1; return; }
  if (l.startsWith("CONNECTED") || l.indexOf("CONNECT OK") >= 0) btStatus = 1;
  if (l.startsWith("DISCONNECT")) btStatus = 0;

  // saved list: "MEM_MacAdd 00:50c4a134373f" / "VM_MacAdd0=50c4a134373f Last_Add=..."
  if (l.startsWith("MEM_MacAdd") || l.startsWith("VM_MacAdd")) {
    int p = l.indexOf(l.startsWith("MEM") ? ':' : '=');
    String m = p > 0 ? hex12(l, p + 1 + (l.substring(p + 1, p + 3) == "0x" ? 2 : 0)) : "";
    if (m.length() && btSavedN < BT_MAX_SAVED) { btSaved[btSavedN].mac = m; btSaved[btSavedN].name = btFoundName(m); btSavedN++; }
    return;
  }
  if (l.startsWith("MEM_Name") || l.startsWith("VM_Name")) {
    int p = l.indexOf(l.startsWith("MEM") ? ':' : '=');
    if (p > 0 && btSavedN < BT_MAX_SAVED) { String n = l.substring(p + 1); n.trim(); btSaved[btSavedN].mac = ""; btSaved[btSavedN].name = n; btSavedN++; }
    return;
  }
  // scan results: MAC and name may be on one line or two
  int mx = l.indexOf("0x");
  String mac = mx >= 0 ? hex12(l, mx + 2) : "";
  int nx = l.indexOf("Name:"); if (nx < 0) nx = l.indexOf("Name=");
  String name = nx >= 0 ? l.substring(nx + 5) : "";
  name.trim();
  if (mac.length()) {
    if (name.length()) { btAddFound(mac, name); btPendingMac = ""; }
    else { btAddFound(mac, ""); btPendingMac = mac; }
  } else if (nx >= 0 && btPendingMac.length()) {
    btAddFound(btPendingMac, name); btPendingMac = "";
  }
}

bool btProbe(uint32_t baud) {
  BTSerial.end();
  BTSerial.begin(baud, SERIAL_8N1, PIN_BT_RX, PIN_BT_TX);
  delay(30);
  while (BTSerial.available()) BTSerial.read();
  for (int attempt = 0; attempt < 2; attempt++) {
    BTSerial.print("AT+\r\n");
    String r; uint32_t t = millis();
    while (millis() - t < 350) {
      while (BTSerial.available()) r += (char)BTSerial.read();
      if (r.indexOf("OK+") >= 0) return true;
      delay(5);
    }
  }
  return false;
}

void btBegin() {
  pinMode(PIN_BT_STATUS, INPUT_PULLDOWN);
  btLastLow = millis();
  btUart = false;
  const uint32_t bauds[] = { 115200, 9600, 57600, 38400, 19200 };
  for (uint32_t b : bauds) if (btProbe(b)) { btUart = true; btBaud = b; break; }
  if (!btUart) { BTSerial.end(); btLogAdd("No reply from KCX module on GPIO " + String(PIN_BT_RX) + "/" + String(PIN_BT_TX)); return; }
  btLogAdd("KCX module found at " + String(btBaud) + " baud");
  btCmd("AT+GMR?"); btCmd("AT+STATUS?"); btCmd("AT+VMLINK?");
}

void btPoll() {
  if (digitalRead(PIN_BT_STATUS) == LOW) btLastLow = millis();
  if (!btUart) return;
  while (BTSerial.available()) {
    char c = BTSerial.read();
    if (c == '\n' || c == '\r') { if (btLine.length()) btParse(btLine); btLine = ""; }
    else if (btLine.length() < 200) btLine += c;
  }
  uint32_t now = millis();
  if (btQCount && now - btLastSend > 250) {
    String c = btQ[btQHead]; bool q = btQQuiet[btQHead];
    btQHead = (btQHead + 1) % BT_Q_N; btQCount--;
    BTSerial.print(c); BTSerial.print("\r\n");
    btLastSend = now;
    btQuietReply = q;
    if (!q) btLogAdd("> " + c);
  }
  if (now - btLastStatusPoll > 4000) { btLastStatusPoll = now; btCmd("AT+STATUS?", true); }
  if (btScanning && now - btScanStart > 20000) btScanning = false;
}

// ---- high level actions (used by OLED menu and web page) ----
void btScan()       { btFoundN = 0; btPendingMac = ""; btScanning = true; btScanStart = millis(); btCmd("AT+SCAN"); }
void btReconnect()  { btCmd("AT+PAIR"); }
void btDisconnect() { btCmd("AT+DISCON"); btStatus = 0; }
void btForgetAll()  { btCmd("AT+DELVMLINK"); btCmd("AT+VMLINK?"); }
void btRefreshSaved() { btCmd("AT+VMLINK?"); }
bool btIsSaved(const String &mac) { for (int i = 0; i < btSavedN; i++) if (btSaved[i].mac == mac) return true; return false; }
// remember this headset and (re)connect to it
void btConnectTo(const String &mac) {
  if (mac.length() != 12) return;
  if (!btIsSaved(mac)) btCmd("AT+ADDLINKADD=" + mac);
  btCmd("AT+PAIR");
  btCmd("AT+VMLINK?");
}
void btAddName(const String &name) {
  if (name.isEmpty() || name.length() > 30) return;
  btCmd("AT+ADDLINKNAME=" + name); btCmd("AT+PAIR"); btCmd("AT+VMLINK?");
}
