// ---------------- Wi-Fi + web control panel ----------------
// Joins your home Wi-Fi if one is saved; otherwise (or if it can't connect)
// it makes its own hotspot "Heltec-MP3-XXXX" (password heltecmp3) at 192.168.4.1.
// Browse to the IP shown on the OLED (Wi-Fi menu) or http://heltec-mp3.local
#pragma once
#include "web_ui.h"

WebServer server(80);
bool     wifiOn = true;
String   wifiSsid, wifiPass, apName;
bool     apActive = false, staTrying = false, mdnsUp = false, serverUp = false;
uint32_t staStart = 0;
File     upFile; String upErr; String upPath;
bool     otaRunning = false; int otaPct = -1;
volatile bool webHit = false;     // a phone/PC has opened the page
bool     libChanged = false;
uint32_t rebootAt = 0;

String jstr(const String &s) {
  String o; o.reserve(s.length() + 2); o += '"';
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((uint8_t)c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
    else o += c;
  }
  return o + '"';
}

String wifiIP() {
  if (WiFi.status() == WL_CONNECTED) return WiFi.localIP().toString();
  if (apActive) return WiFi.softAPIP().toString();
  return "";
}

DNSServer dns;
void startAP() {
  WiFi.mode(WIFI_AP_STA);              // AP + STA so Wi-Fi scanning works from the hotspot
  WiFi.softAP(apName.c_str(), "heltecmp3");
  dns.start(53, "*", WiFi.softAPIP()); // phones that join the hotspot pop up the player page
  apActive = true;
}

void wifiStop() {
  server.stop(); serverUp = false;
  if (mdnsUp) { MDNS.end(); mdnsUp = false; }
  if (apActive) dns.stop();
  WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
  apActive = false; staTrying = false;
}

void setupRoutes();

void wifiStart() {
  if (!wifiOn) { wifiStop(); return; }
  uint8_t mac[6]; WiFi.macAddress(mac);
  char n[24]; snprintf(n, sizeof(n), "Heltec-MP3-%02X%02X", mac[4], mac[5]); apName = n;
  WiFi.persistent(false);
  WiFi.setHostname("heltec-mp3");
  apActive = false;
  if (wifiSsid.length()) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
    staTrying = true; staStart = millis();
  } else {
    startAP();
  }
  if (!serverUp) { setupRoutes(); server.begin(); serverUp = true; }
  if (!mdnsUp && MDNS.begin("heltec-mp3")) { MDNS.addService("http", "tcp", 80); mdnsUp = true; }
}

void wifiTick() {
  if (!wifiOn) return;
  if (staTrying) {
    if (WiFi.status() == WL_CONNECTED) {
      staTrying = false;
      if (apActive) { dns.stop(); WiFi.softAPdisconnect(true); WiFi.mode(WIFI_STA); apActive = false; }
      toast("Wi-Fi " + WiFi.localIP().toString(), 2500);
    } else if (!apActive && millis() - staStart > 15000) {
      startAP();     // couldn't join: open the hotspot, keep trying home Wi-Fi in the background
      WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
      toast("Hotspot on", 2000);
    }
  } else if (wifiSsid.length() && !apActive && WiFi.status() != WL_CONNECTED) {
    staTrying = true; staStart = millis();     // lost home Wi-Fi: same fallback
  }
  if (apActive) dns.processNextRequest();
  if (serverUp) server.handleClient();
  if (rebootAt && millis() > rebootAt) ESP.restart();
}

// ---- JSON builders ----
String statusJson() {
  float prog = 0;
  lockA();
  if (isPlaying && srcType == SRC_FILE && fsrc && fsrc->getSize() > 0) prog = (float)fsrc->getPos() / fsrc->getSize();
  if (srcType == SRC_RADIO && bsrc) prog = (float)bsrc->getFillLevel() / RADIO_BUF;
  unlockA();
  String j = "{";
  j += "\"playing\":" + String(isPlaying ? "true" : "false");
  j += ",\"paused\":" + String(isPaused ? "true" : "false");
  j += ",\"radio\":" + String(stPlaying >= 0 ? "true" : "false");
  j += ",\"station\":" + String(stPlaying);
  j += ",\"streamTitle\":" + jstr(String(streamTitle));
  j += ",\"title\":" + jstr(String(tagTitle));
  j += ",\"artist\":" + jstr(String(tagArtist));
  j += ",\"file\":" + jstr(String(nowFile));
  j += ",\"format\":" + jstr(String(nowFmt));
  j += ",\"dir\":" + jstr(plDir);
  j += ",\"list\":" + jstr(plLabel);
  j += ",\"index\":" + String(plIndex + 1) + ",\"count\":" + String(plCount);
  j += ",\"elapsed\":" + String(elapsedMs() / 1000);
  j += ",\"progress\":" + String(prog, 4);
  j += ",\"volume\":" + String(volume);
  j += ",\"mode\":" + String(playMode);
  j += ",\"bass\":" + String(eqBass) + ",\"treble\":" + String(eqTreble);
  j += ",\"sleep\":" + String(sleepEndMs ? (int32_t)(sleepEndMs - millis()) / 1000 : 0);
  j += ",\"vu\":[" + String(vuL) + "," + String(vuR) + "]";
  j += ",\"batt\":{\"mv\":" + String(battMv) + ",\"pct\":" + String(battPct) + "}";
  j += ",\"sd\":" + String(sdOk ? "true" : "false");
  j += ",\"bt\":{\"uart\":" + String(btUart ? "true" : "false") + ",\"connected\":" + String(btConnected() ? "true" : "false") + "}";
  j += "}";
  return j;
}

String btJson() {
  String j = "{\"uart\":" + String(btUart ? "true" : "false");
  j += ",\"baud\":" + String(btBaud);
  j += ",\"version\":" + jstr(btVer);
  j += ",\"status\":" + String(btStatus);
  j += ",\"connected\":" + String(btConnected() ? "true" : "false");
  j += ",\"scanning\":" + String(btScanning ? "true" : "false");
  j += ",\"rx\":" + String(PIN_BT_RX) + ",\"tx\":" + String(PIN_BT_TX) + ",\"link\":" + String(PIN_BT_STATUS);
  j += ",\"found\":[";
  for (int i = 0; i < btFoundN; i++) { if (i) j += ','; j += "{\"mac\":" + jstr(btFound[i].mac) + ",\"name\":" + jstr(btFound[i].name) + ",\"saved\":" + String(btIsSaved(btFound[i].mac) ? "true" : "false") + "}"; }
  j += "],\"saved\":[";
  for (int i = 0; i < btSavedN; i++) { if (i) j += ','; j += "{\"mac\":" + jstr(btSaved[i].mac) + ",\"name\":" + jstr(btSaved[i].name.length() ? btSaved[i].name : btFoundName(btSaved[i].mac)) + "}"; }
  j += "],\"log\":[";
  for (int k = 0; k < btLogCount; k++) {
    int i = (btLogHead - btLogCount + k + BT_LOG_N) % BT_LOG_N;
    if (k) j += ','; j += jstr(btLog[i]);
  }
  j += "]}";
  return j;
}

void sendJson(const String &j, int code = 200) { server.send(code, "application/json", j); }
void sendOk() { sendJson("{\"ok\":true}"); }
void sendErr(const String &m) { sendJson("{\"ok\":false,\"error\":" + jstr(m) + "}", 400); }

bool safePath(const String &p) { return p.startsWith("/") && p.indexOf("..") < 0; }

void setupRoutes() {
  server.on("/", HTTP_GET, []() {
    webHit = true;
    server.sendHeader("Cache-Control", "no-cache");
    server.send_P(200, "text/html", WEB_UI);
  });

  server.on("/api/status", HTTP_GET, []() { webHit = true; sendJson(statusJson()); });

  server.on("/api/cmd", HTTP_POST, []() {
    String a = server.arg("a");
    if (a == "toggle") { if (isPlaying) togglePause(); else if (plCount) playIndex(max(0, plIndex)); }
    else if (a == "next") nextTrack(true);
    else if (a == "prev") prevTrack();
    else if (a == "stop") stopTrack();
    else if (a == "eq") setEQ(server.arg("bass").toInt(), server.arg("treble").toInt());
    else if (a == "sleep") { int m = server.arg("v").toInt(), k = 0; for (int i = 0; i < 6; i++) if (sleepMins[i] == m) k = i; setSleep(k); }
    else if (a == "shuffleall") { if (!shuffleAll()) { sendErr("No music found on the SD card"); return; } }
    else if (a == "resume") resumeLast();
    else if (a == "vol") { volume = constrain(server.arg("v").toInt(), 0, 20); lockA(); applyVolume(); unlockA(); prefs.putInt("vol", volume); }
    else if (a == "mode") { playMode = constrain(server.arg("v").toInt(), 0, MODE_COUNT - 1); if (playMode == MODE_SHUFFLE && plCount) buildShuffle(plIndex); saveSettings(); }
    else { sendErr("unknown command"); return; }
    sendJson(statusJson());
  });

  // list a folder: {dir, entries:[{n,d,s}]}
  server.on("/api/ls", HTTP_GET, []() {
    if (!sdOk) { sendErr("No SD card"); return; }
    String dir = server.hasArg("dir") ? server.arg("dir") : "/";
    if (!safePath(dir)) { sendErr("bad path"); return; }
    String j = "{\"dir\":" + jstr(dir) + ",\"entries\":[";
    int n = 0;
    lockA(); File d = SD.open(dir); unlockA();
    if (!d || !d.isDirectory()) { sendErr("Folder not found"); return; }
    for (;;) {
      lockA(); File f = d.openNextFile(); unlockA();     // one entry at a time so music never stutters
      if (!f) break;
      String nm = baseName(String(f.name())); bool isD = f.isDirectory(); uint32_t sz = isD ? 0 : f.size();
      lockA(); f.close(); unlockA();
      if (nm.startsWith(".") || nm == "System Volume Information") continue;
      if (n++) j += ',';
      j += "{\"n\":" + jstr(nm) + ",\"d\":" + String(isD ? "true" : "false") + ",\"s\":" + String(sz) + "}";
      if (n > 600) break;
    }
    lockA(); d.close(); unlockA();
    j += "]}";
    sendJson(j);
  });

  server.on("/api/play", HTTP_POST, []() {
    String path = server.hasArg("path") ? server.arg("path") : joinPath(server.arg("dir"), server.arg("file"));
    if (!safePath(path)) { sendErr("bad path"); return; }
    if (playPath(path)) sendJson(statusJson()); else sendErr("Couldn't play that file");
  });

  // whole-card search (builds the index on first use)
  server.on("/api/search", HTTP_GET, []() {
    if (!sdOk) { sendErr("No SD card"); return; }
    if (!idxReady) buildIndex(false);
    String q = server.arg("q"); q.toLowerCase(); q.trim();
    String j = "{\"total\":" + String(idxCount) + ",\"results\":[";
    int n = 0;
    for (int i = 0; i < idxCount && n < 200; i++) {
      if (q.length()) { String l = libIndex[i]; l.toLowerCase(); if (l.indexOf(q) < 0) continue; }
      if (n++) j += ',';
      j += jstr(String(libIndex[i]));
    }
    sendJson(j + "]}");
  });

  // ---- Internet radio ----
  server.on("/api/radio", HTTP_GET, []() {
    String j = "{\"wifi\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",\"playing\":" + String(stPlaying) + ",\"stations\":[";
    for (int i = 0; i < stCount; i++) { if (i) j += ','; j += "{\"name\":" + jstr(stName[i]) + ",\"url\":" + jstr(stUrl[i]) + "}"; }
    sendJson(j + "]}");
  });
  server.on("/api/radio", HTTP_POST, []() {
    String a = server.arg("a");
    if (a == "play") { if (!playStation(server.arg("i").toInt())) { sendErr(WiFi.status() == WL_CONNECTED ? "Station not reachable" : "Radio needs the player on your home Wi-Fi"); return; } screen = SCR_NOW; }
    else if (a == "add") { if (!addStation(server.arg("name"), server.arg("url"))) { sendErr("Need a name and an http:// stream URL (https isn't supported)"); return; } }
    else if (a == "del") delStation(server.arg("i").toInt());
    else if (a == "up") { int i = server.arg("i").toInt(); if (i > 0 && i < stCount) { String n = stName[i], u = stUrl[i]; stName[i] = stName[i-1]; stUrl[i] = stUrl[i-1]; stName[i-1] = n; stUrl[i-1] = u; if (stPlaying == i) stPlaying--; else if (stPlaying == i-1) stPlaying++; saveStations(); } }
    else if (a == "reset") { stopTrack(); defaultStations(); saveStations(); }
    else { sendErr("unknown action"); return; }
    sendOk();
  });

  server.on("/api/delete", HTTP_POST, []() {
    String p = server.arg("path");
    if (!safePath(p) || p == "/") { sendErr("bad path"); return; }
    if (isPlaying && stPlaying < 0 && plIndex >= 0 && p == playlist[plIndex]) { sendErr("That song is playing. Stop it first."); return; }
    lockA();
    File f = SD.open(p); bool isD = f && f.isDirectory(); if (f) f.close();
    bool ok = isD ? SD.rmdir(p) : SD.remove(p);
    unlockA();
    libChanged = true;
    if (ok) sendOk(); else sendErr(isD ? "Folder must be empty" : "Delete failed");
  });

  server.on("/api/mkdir", HTTP_POST, []() {
    String p = server.arg("path");
    if (!safePath(p)) { sendErr("bad path"); return; }
    lockA(); bool ok = SD.mkdir(p); unlockA();
    libChanged = true;
    if (ok) sendOk(); else sendErr("Could not create folder");
  });

  // upload MP3s straight to the SD card
  server.on("/api/upload", HTTP_POST, []() {
    libChanged = true;
    if (upErr.length()) sendErr(upErr); else sendOk();
  }, []() {
    HTTPUpload &u = server.upload();
    if (u.status == UPLOAD_FILE_START) {
      upErr = "";
      String dir = server.arg("dir"); if (!safePath(dir)) dir = "/";
      upPath = joinPath(dir, baseName(u.filename));
      lockA();
      if (SD.exists(upPath)) SD.remove(upPath);
      upFile = SD.open(upPath, FILE_WRITE);
      unlockA();
      if (!upFile) upErr = "Can't write to SD card";
    } else if (u.status == UPLOAD_FILE_WRITE) {
      if (upFile) { lockA(); size_t w = upFile.write(u.buf, u.currentSize); unlockA(); if (w != u.currentSize) upErr = "SD card full?"; }
    } else if (u.status == UPLOAD_FILE_END) {
      if (upFile) { lockA(); upFile.close(); unlockA(); }
    } else if (u.status == UPLOAD_FILE_ABORTED) {
      if (upFile) { lockA(); upFile.close(); SD.remove(upPath); unlockA(); }
      upErr = "Upload aborted";
    }
  });

  // ---- Bluetooth ----
  server.on("/api/bt", HTTP_GET, []() { sendJson(btJson()); });
  server.on("/api/bt", HTTP_POST, []() {
    String a = server.arg("a");
    if (a == "retry") { btBegin(); sendJson(btJson()); return; }
    if (!btUart) { sendErr("The KCX module isn't answering on its serial wires."); return; }
    if (a == "scan") btScan();
    else if (a == "connect") btConnectTo(server.arg("mac"));
    else if (a == "addname") btAddName(server.arg("name"));
    else if (a == "pair") btReconnect();
    else if (a == "disconnect") btDisconnect();
    else if (a == "forget") btForgetAll();
    else if (a == "saved") btRefreshSaved();
    else if (a == "raw") { String c = server.arg("cmd"); c.trim(); if (!c.startsWith("AT")) { sendErr("Commands start with AT"); return; } btCmd(c); }
    else { sendErr("unknown action"); return; }
    sendJson(btJson());
  });

  // ---- Wi-Fi ----
  server.on("/api/wifi", HTTP_GET, []() {
    String j = "{\"on\":" + String(wifiOn ? "true" : "false");
    j += ",\"ssid\":" + jstr(wifiSsid);
    j += ",\"connected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false");
    j += ",\"ip\":" + jstr(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String(""));
    j += ",\"rssi\":" + String(WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0);
    j += ",\"ap\":" + String(apActive ? "true" : "false");
    j += ",\"apName\":" + jstr(apName);
    j += ",\"apIp\":" + jstr(apActive ? WiFi.softAPIP().toString() : String(""));
    j += "}";
    sendJson(j);
  });
  server.on("/api/wifi/scan", HTTP_GET, []() {
    if (WiFi.getMode() == WIFI_AP) WiFi.mode(WIFI_AP_STA);
    int n = WiFi.scanNetworks();
    String j = "[";
    for (int i = 0; i < n && i < 30; i++) {
      if (i) j += ',';
      j += "{\"ssid\":" + jstr(WiFi.SSID(i)) + ",\"rssi\":" + String(WiFi.RSSI(i)) + ",\"open\":" + String(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "true" : "false") + "}";
    }
    WiFi.scanDelete();
    sendJson(j + "]");
  });
  server.on("/api/wifi", HTTP_POST, []() {
    String s = server.arg("ssid"), p = server.arg("pass");
    if (server.arg("forget") == "1") { s = ""; p = ""; }
    prefs.putString("wssid", s); prefs.putString("wpass", p);
    sendJson("{\"ok\":true,\"rebooting\":true}");
    rebootAt = millis() + 1200;
  });

  // ---- System ----
  server.on("/api/sys", HTTP_GET, []() {
    String j = "{\"fw\":" + jstr(FW_VERSION);
    j += ",\"uptime\":" + String(millis() / 1000);
    j += ",\"heap\":" + String(ESP.getFreeHeap());
    j += ",\"psram\":" + String(ESP.getPsramSize()) + ",\"psramFree\":" + String(ESP.getFreePsram());
    j += ",\"flash\":" + String(ESP.getFlashChipSize());
    j += ",\"board\":" + jstr(BOARD_NAME);
    j += ",\"batt\":{\"mv\":" + String(battMv) + ",\"pct\":" + String(battPct) + "}";
    j += ",\"songs\":" + String(idxReady ? idxCount : -1);
    if (sdOk) {
      lockA(); uint64_t tot = SD.totalBytes(), used = SD.usedBytes(); unlockA();
      j += ",\"sdTotal\":" + String((uint32_t)(tot / 1048576)) + ",\"sdUsed\":" + String((uint32_t)(used / 1048576));
    }
    j += "}";
    sendJson(j);
  });
  server.on("/api/reboot", HTTP_POST, []() { sendOk(); rebootAt = millis() + 800; });

  // over-the-air firmware update (use heltec-mp3-app-vX.bin)
  server.on("/update", HTTP_POST, []() {
    bool ok = !Update.hasError();
    sendJson(ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":" + jstr(Update.errorString()) + "}", ok ? 200 : 500);
    otaRunning = false;
    if (ok) rebootAt = millis() + 1000;
  }, []() {
    HTTPUpload &u = server.upload();
    if (u.status == UPLOAD_FILE_START) {
      stopTrack(); otaRunning = true; otaPct = 0;
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
    } else if (u.status == UPLOAD_FILE_WRITE) {
      if (Update.write(u.buf, u.currentSize) != u.currentSize) Update.printError(Serial);
      otaPct = (u.totalSize / 1024) % 1000;
      oled.clearBuffer(); oled.setFont(u8g2_font_7x13B_tf); oled.drawStr(14, 28, "Updating...");
      oled.setFont(u8g2_font_6x10_tf); oled.drawStr(14, 46, (String(u.totalSize / 1024) + " KB received").c_str()); oled.sendBuffer();
    } else if (u.status == UPLOAD_FILE_END) {
      if (!Update.end(true)) Update.printError(Serial);
    } else if (u.status == UPLOAD_FILE_ABORTED) {
      Update.abort(); otaRunning = false;
    }
  });

  server.onNotFound([]() {
    // captive-portal style: send phones that join the hotspot to the player page
    webHit = true;
    if (apActive) { server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/"); server.send(302, "text/plain", ""); }
    else server.send(404, "text/plain", "Not found");
  });
}
