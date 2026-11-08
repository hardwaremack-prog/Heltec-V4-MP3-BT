// Renders the Heltec MP3 OLED screens with the real U8g2 library + fonts,
// mirroring the drawing code in HeltecMP3.ino, to PBM files.
#include <stdio.h>
#include <string.h>
#include "u8g2.h"

static u8g2_t u8g2;
static u8g2_t *g = &u8g2;

static uint8_t nocom(u8x8_t *u, uint8_t m, uint8_t a, void *p) { return 1; }
static void save(const char *name) {
  uint8_t *buf = u8g2_GetBufferPtr(g);
  char p[128]; snprintf(p, sizeof p, "out/%s.pbm", name);
  FILE *f = fopen(p, "w"); fprintf(f, "P1\n128 64\n");
  for (int y = 0; y < 64; y++) { for (int x = 0; x < 128; x++) fprintf(f, "%d ", (buf[(y / 8) * 128 + x] >> (y % 8) & 1) ? 1 : 0); fprintf(f, "\n"); }
  fclose(f);
}

static int BATT = 72, SLEEP = 0;
static void header(const char *title, int vol, const char *mode, int bt) {
  u8g2_SetFont(g, u8g2_font_6x10_tf);
  u8g2_DrawStr(g, 0, 8, title);
  int x = 128; char v[8]; snprintf(v, 8, "%d", vol);
  if (BATT >= 0) { x -= 12; u8g2_DrawFrame(g, x, 2, 10, 6); u8g2_DrawBox(g, x + 10, 4, 1, 2); int w = (BATT * 8 + 50) / 100; if (w) u8g2_DrawBox(g, x + 1, 3, w, 4); x -= 3; } else x -= 1;
  x -= u8g2_GetStrWidth(g, v); u8g2_DrawStr(g, x, 8, v);
  x -= 8; u8g2_DrawBox(g, x, 3, 2, 4); u8g2_DrawTriangle(g, x + 2, 5, x + 5, 1, x + 5, 9);
  if (mode && *mode) { x -= u8g2_GetStrWidth(g, mode) + 3; u8g2_DrawStr(g, x, 8, mode); }
  if (SLEEP) { x -= 8; u8g2_DrawStr(g, x, 8, "z"); }
  if (bt) { x -= 8; u8g2_SetFont(g, u8g2_font_open_iconic_embedded_1x_t); u8g2_DrawGlyph(g, x, 9, 74); u8g2_SetFont(g, u8g2_font_6x10_tf); }
  u8g2_DrawHLine(g, 0, 10, 128);
}

// items: label strings; dirs: 1 = folder icon
static void list(const char **items, const int *dirs, int count, int sel, int top) {
  const int rows = 5, rowH = 10, y0 = 13;
  u8g2_SetFont(g, u8g2_font_6x10_tf);
  for (int r = 0; r < rows && top + r < count; r++) {
    int i = top + r, y = y0 + r * rowH;
    if (i == sel) { u8g2_DrawBox(g, 0, y, 122, rowH); u8g2_SetDrawColor(g, 0); }
    int tx = 2;
    if (dirs && dirs[i]) { u8g2_DrawFrame(g, 2, y + 3, 7, 5); u8g2_DrawHLine(g, 2, y + 2, 3); tx = 12; }
    u8g2_SetClipWindow(g, tx, y, 121, y + rowH);
    u8g2_DrawStr(g, tx, y + 8, items[i]);
    u8g2_SetMaxClipWindow(g);
    u8g2_SetDrawColor(g, 1);
  }
  if (count > rows) {
    int h = 50 * rows / count; if (h < 4) h = 4;
    int yy = 13 + (50 - h) * sel / (count - 1);
    u8g2_DrawVLine(g, 125, 13, 50); u8g2_DrawBox(g, 124, yy, 3, h);
  }
}

static void toast(const char *m) {
  u8g2_SetFont(g, u8g2_font_6x10_tf);
  int w = u8g2_GetStrWidth(g, m) + 8, x = (128 - w) / 2;
  u8g2_SetDrawColor(g, 0); u8g2_DrawBox(g, x - 1, 24, w + 2, 17); u8g2_SetDrawColor(g, 1);
  u8g2_DrawRFrame(g, x, 25, w, 15, 3);
  u8g2_DrawStr(g, x + 4, 36, m);
}


static void vu(int x, int y, int l, int r) {
  int lv[2] = { l, r };
  for (int c = 0; c < 2; c++) { u8g2_DrawFrame(g, x + c * 5, y, 4, 11); if (lv[c]) u8g2_DrawBox(g, x + c * 5 + 1, y + 10 - lv[c], 2, lv[c]); }
}
static void np(const char *state, const char *mode, const char *title, const char *sub, float prog, int live, const char *t, const char *pos, int playing) {
  header(state, 12, mode, 1);
  u8g2_SetFont(g, u8g2_font_7x13B_tf);
  u8g2_SetClipWindow(g, 0, 12, 127, 27); u8g2_DrawStr(g, 0, 25, title); u8g2_SetMaxClipWindow(g);
  u8g2_SetFont(g, u8g2_font_6x10_tf);
  u8g2_SetClipWindow(g, 0, 29, 127, 40); u8g2_DrawStr(g, 0, 38, sub); u8g2_SetMaxClipWindow(g);
  u8g2_DrawFrame(g, 0, 43, 128, 6);
  if (live) { for (int x = 2; x < 2 + (int)(124 * prog); x += 2) u8g2_DrawVLine(g, x, 44, 4); }
  else u8g2_DrawBox(g, 1, 44, (int)(126 * prog), 4);
  u8g2_DrawStr(g, 0, 62, t);
  u8g2_DrawStr(g, 128 - u8g2_GetStrWidth(g, pos), 62, pos);
  if (playing) u8g2_DrawTriangle(g, 52, 53, 52, 63, 60, 58); else { u8g2_DrawBox(g, 52, 53, 3, 10); u8g2_DrawBox(g, 57, 53, 3, 10); }
  if (playing && 128 - u8g2_GetStrWidth(g, pos) > 78) vu(66, 53, 8, 6);
}
static void progressScreen(const char *title, const char *line) {
  u8g2_SetFont(g, u8g2_font_7x13B_tf); u8g2_DrawStr(g, (128 - u8g2_GetStrWidth(g, title)) / 2, 26, title);
  u8g2_SetFont(g, u8g2_font_6x10_tf); u8g2_DrawStr(g, (128 - u8g2_GetStrWidth(g, line)) / 2, 44, line);
}
int main(void) {
  u8g2_Setup_ssd1306_128x64_noname_f(g, U8G2_R0, nocom, nocom);
  u8g2_ClearBuffer(g); progressScreen("Heltec V4 MP3", "v2.0.0  starting..."); save("01_boot");
  const char *home[] = { "Browse music", "Now playing", "Internet radio", "Resume last", "Shuffle folder", "Shuffle all music", "Bluetooth", "Wi-Fi", "Settings", "About" };
  u8g2_ClearBuffer(g); header("Heltec MP3", 12, "SH", 1); list(home, NULL, 10, 2, 0); save("02_home");
  u8g2_ClearBuffer(g); np("Playing", "SH", "Midnight Drive", "The Night Cars", 0.42f, 0, "1:23", "3/14", 1); save("03_now_playing_flac");
  const char *st[] = { "> SomaFM Groove Salad", "SomaFM Drone Zone", "SomaFM Secret Agent", "SomaFM Lush", "SomaFM Indie Pop", "Radio Paradise" };
  u8g2_ClearBuffer(g); header("Radio", 12, "", 1); list(st, NULL, 6, 0, 0); save("04_radio_list");
  u8g2_ClearBuffer(g); progressScreen("Tuning in...", "SomaFM Groove Salad"); save("05_tuning_in");
  u8g2_ClearBuffer(g); np("Radio", "", "SomaFM Groove Salad", "Bonobo - Kerala", 0.86f, 1, "12:07", "LIVE", 1); save("06_now_playing_radio");
  u8g2_ClearBuffer(g); progressScreen("Scanning card", "1874 songs found"); save("07_scanning_card");
  u8g2_ClearBuffer(g); np("Playing", "SH", "Neon Rain", "All music", 0.12f, 0, "0:24", "211/2318", 1); save("08_shuffle_all");
  const char *set[] = { "Volume +  (12)", "Volume -  (12)", "Mode: Shuffle", "Bass: +4 dB", "Treble: +2 dB", "Sleep timer: 30 min", "Visualizer: On" };
  SLEEP = 1;
  u8g2_ClearBuffer(g); header("Settings", 12, "SH", 1); list(set, NULL, 12, 3, 2); save("09_settings_eq");
  const char *ctl[] = { "Pause", "Next track", "Previous track", "Volume +  (12)", "Volume -  (12)", "Mode: Shuffle", "Sleep: 30 min", "Stop" };
  u8g2_ClearBuffer(g); header("Controls", 12, "SH", 1); list(ctl, NULL, 10, 6, 2); save("10_controls_sleep");
  SLEEP = 0;
  const char *bt[] = { "Headset: none", "Scanning... (3)", "* JBL TUNE510BT", "+ Sony WH-1000XM4", "+ Soundcore Q20", "Reconnect", "Forget saved", "Disconnect", "Back" };
  u8g2_ClearBuffer(g); header("Bluetooth", 12, "SH", 0); list(bt, NULL, 9, 3, 0); save("11_bluetooth");
  const char *wf[] = { "Wi-Fi: On", "IP: 192.168.1.58", "Net: HomeNet", "heltec-mp3.local", "Forget home Wi-Fi", "Back" };
  u8g2_ClearBuffer(g); header("Wi-Fi", 12, "SH", 1); list(wf, NULL, 6, 1, 0); save("12_wifi");
  u8g2_ClearBuffer(g); header("About", 12, "SH", 1);
  u8g2_SetFont(g, u8g2_font_6x10_tf);
  u8g2_DrawStr(g, 0, 23, "Heltec V4 MP3 v2.0.0"); u8g2_DrawStr(g, 0, 34, "IP 192.168.1.58");
  u8g2_DrawStr(g, 0, 45, "Battery 3.95V 72%"); u8g2_DrawStr(g, 0, 56, "hold=OK  long=back");
  save("13_about");
  BATT = -1;
  u8g2_ClearBuffer(g);
  u8g2_SetFont(g, u8g2_font_7x13B_tf); u8g2_DrawStr(g, 18, 14, "No SD card");
  u8g2_SetFont(g, u8g2_font_5x7_tf);
  u8g2_DrawStr(g, 0, 26, "SCK40 MOSI41 MISO42 CS39"); u8g2_DrawStr(g, 0, 35, "3.3V + GND, FAT32 card"); u8g2_DrawStr(g, 0, 44, "Radio works on Wi-Fi");
  u8g2_SetFont(g, u8g2_font_6x10_tf); u8g2_DrawStr(g, 0, 60, "OK: retry  BACK: home");
  save("14_no_sd");
  u8g2_ClearBuffer(g); np("Paused", "", "Nothing Else Matters", "Live at the Roxy", 0.71f, 0, "4:02", "WAV", 0); save("15_paused");
  return 0;
}
