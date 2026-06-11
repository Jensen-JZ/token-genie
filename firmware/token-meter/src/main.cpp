// TokenGenie — Claude Code / Codex usage meter on a round AMOLED (LVGL).
// Board: Waveshare ESP32-S3-Touch-AMOLED-1.75 (CO5300 QSPI 466x466, CST9217 touch).
//
//   swipe left/right     -> change page (Claude / Codex / Info); PWR click = next
//   hold screen at boot  -> wipe WiFi + open setup portal (deliberate; no misfire)
// WiFi order: saved creds -> hardcoded seed (secrets.h) -> WiFiManager portal.
//
// Data: GET https://ccusage.peritrix.com/usage?key=...  (host usage_server.py)
//   { "updated":<epoch>,
//     "claude": { "five_hour": {"util":<0-100>,"reset_at":<epoch>}, "seven_day": {...} },
//     "codex":  { "five_hour": {...}, "seven_day": {...} }, "ok":<bool> }
//   util = used %% (arc/bar fill). reset_at = epoch secs (countdown = reset_at - now).
//
// Time: NTP (configTime, GMT+8) synced into the onboard PCF85063 RTC. Battery: AXP2101.
// Rendering: LVGL 8.4 with a full-screen PSRAM draw buffer (full_refresh) so a serial
//   'D' command can still dump the framebuffer as a screenshot.

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <lvgl.h>
#include <Wire.h>
#include <time.h>
#include "Arduino_GFX_Library.h"
#include "pin_config.h"
#include "XPowersLib.h"
#include "SensorPCF85063.hpp"
#include "TouchDrvCSTXXX.hpp"
#include "secrets.h"

// ---- display + LVGL ----------------------------------------------------------
static Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
static Arduino_CO5300 *gfx = new Arduino_CO5300(
    bus, LCD_RESET, 0 /* rotation */, LCD_WIDTH, LCD_HEIGHT, 6, 0, 0, 0);

static const int SIZE = 466;
static lv_disp_draw_buf_t draw_buf;
static lv_disp_drv_t disp_drv;
static uint32_t lastTick;

static uint16_t *g_fb = nullptr;   // full-screen mirror in PSRAM, for 'D' screenshot dump
static void disp_flush(lv_disp_drv_t *d, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = area->x2 - area->x1 + 1, h = area->y2 - area->y1 + 1;
  gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)color_p, w, h);
  if (g_fb) {                        // mirror this dirty region into the screenshot buffer
    uint16_t *src = (uint16_t *)color_p;
    for (uint32_t row = 0; row < h; row++)
      memcpy(&g_fb[(area->y1 + row) * SIZE + area->x1], &src[row * w], w * 2);
  }
  lv_disp_flush_ready(d);
}
static void disp_rounder(lv_disp_drv_t *d, lv_area_t *a) {
  if (a->x1 % 2) a->x1--;
  if (a->y1 % 2) a->y1--;
  if (a->x2 % 2 == 0) a->x2++;
  if (a->y2 % 2 == 0) a->y2++;
}

// ---- touch (IRQ-driven) ------------------------------------------------------
static TouchDrvCST92xx touch;
static bool touchOK = false;
static int16_t tx[5], ty[5];
static bool touch_pressed() {
  return touchOK && touch.getPoint(tx, ty, touch.getSupportTouchPoint()) > 0;
}

// ---- power (AXP2101) + RTC (PCF85063) ----------------------------------------
static XPowersPMU pmu;
static bool pmuOK = false;
static SensorPCF85063 rtc;
static bool rtcOK = false;
static int batPct = -1;
static bool charging = false;
static uint16_t batMv = 0;     // last good battery voltage (mV); shared I2C bus glitches read as 0
static void read_power() {
  if (!pmuOK) return;
  // Shared I2C bus (touch polled every loop for swipe) occasionally corrupts a PMU read.
  // Debounce VBUS so one bad read doesn't grey the charge bolt; ignore glitch voltages.
  static uint8_t vbusMiss = 0;
  if (pmu.isVbusIn()) vbusMiss = 0; else if (vbusMiss < 250) vbusMiss++;
  charging = (vbusMiss < 3);   // "unplugged" only after ~3 consecutive misses (~3s)
  batPct = pmu.isBatteryConnect() ? pmu.getBatteryPercent() : -1;
  uint16_t v = pmu.getBattVoltage();
  if (v > 2500) batMv = v;     // plausible Li-ion only; drop 0 / garbage
}
static int batt_ui() { return batPct < 0 ? 100 : batPct; }

// ---- data --------------------------------------------------------------------
struct Win { int util = -1; long reset_at = 0; };
struct AgentUsage { Win h5, d7; };
struct Snap { AgentUsage claude, codex; bool valid = false; long updated = 0; bool ok = false; } snap;

static int page = 0;             // 0 = Claude, 1 = Codex
static int lang = 0;             // 0 = English, 1 = 中文, 2 = Deutsch
static const char *pickStr(const char *en, const char *zh, const char *de) {  // pick by language
  return lang == 2 ? de : (lang == 1 ? zh : en);
}
static uint32_t toastUntil = 0;  // timezone toast hide deadline (millis)
static uint32_t pwrDownAt = 0;   // PWR pressed-at (for the 10s power-off hold)
static bool pwrHeld = false;
static const uint32_t FETCH_INTERVAL_MS = 60000;
static uint32_t lastFetch = 0;
static char g_status[40] = "";
static bool g_wifi = false;

// sync-status tracking
static uint32_t lastOkFetchMs = 0;   // millis() of the last HTTP 200 from host
static bool lastFetchOk = false;
static bool syncing = false;

// ---- palette -----------------------------------------------------------------
#define COL_CLAUDE  lv_color_hex(0xF4894F)
#define COL_CODEX   lv_color_hex(0x22D3A6)
#define COL_DANGER  lv_color_hex(0xFF5D5D)
#define COL_TRACK   lv_color_hex(0x23262C)
#define COL_DIM     lv_color_hex(0x6D6F75)
#define COL_LABEL   lv_color_hex(0x9A9CA2)

// ---- time helpers ------------------------------------------------------------
static void reset_str(char *buf, size_t n, long reset_at, bool is7d) {
  if (reset_at <= 0) { snprintf(buf, n, "--"); return; }
  time_t now = time(nullptr);
  long rem = reset_at - (long)now;
  if (rem < 0) rem = 0;
  bool countdown = !is7d || rem < 86400;
  if (countdown) {
    int h = rem / 3600, m = (rem % 3600) / 60;
    if (h > 0) snprintf(buf, n, lang == 1 ? "%d时%02d分" : "%dh%02dm", h, m);
    else       snprintf(buf, n, lang == 1 ? "%d分" : "%dm", m);
  } else {
    time_t t = reset_at; struct tm lt; localtime_r(&t, &lt);
    snprintf(buf, n, "%d/%d %02d:%02d", lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min);
  }
}
// timezone: POSIX TZ strings WITH DST rules — localtime() switches summer/winter by itself.
// Default = host's (China, UTC+8). Double-click PWR cycles through these.
static const char *TZ_POSIX[] = {
  "PST8PDT,M3.2.0,M11.1.0",        // US Pacific
  "EST5EDT,M3.2.0,M11.1.0",        // US Eastern
  "GMT0BST,M3.5.0/1,M10.5.0",      // UK
  "CET-1CEST,M3.5.0,M10.5.0/3",    // Central Europe
  "MSK-3",                          // Moscow (no DST)
  "PKT-5",                          // Pakistan (no DST)
  "CST-8",                          // China (no DST)
  "JST-9",                          // Japan (no DST)
  "AEST-10AEDT,M10.1.0,M4.1.0/3",  // Australia/Sydney (southern-hemisphere DST)
};
static const int NTZ = sizeof(TZ_POSIX) / sizeof(TZ_POSIX[0]);
static int tzIndex = 6;   // default China (UTC+8)
static void apply_tz() {
  setenv("TZ", TZ_POSIX[tzIndex], 1);
  tzset();
}
// current UTC offset in hours, DST included (read back from localtime)
static int cur_utc_offset() {
  time_t now = time(nullptr);
  if (now < 1700000000) return 8;
  struct tm gm; gmtime_r(&now, &gm); gm.tm_isdst = -1;
  return (int)((now - mktime(&gm)) / 3600);   // local - UTC offset, DST included
}
static void clock_str(char *buf, size_t n) {
  time_t now = time(nullptr);
  if (now < 1700000000) { snprintf(buf, n, "--:--"); return; }
  struct tm lt; localtime_r(&now, &lt);
  snprintf(buf, n, "%02d:%02d", lt.tm_hour, lt.tm_min);
}
static void time_sync() {
  apply_tz();   // default = host timezone (UTC+8); changed via double-click PWR
  if (rtcOK) {
    RTC_DateTime d = rtc.getDateTime();
    if (d.getYear() >= 2024) {
      struct tm tmv = {};
      tmv.tm_year = d.getYear() - 1900; tmv.tm_mon = d.getMonth() - 1; tmv.tm_mday = d.getDay();
      tmv.tm_hour = d.getHour(); tmv.tm_min = d.getMinute(); tmv.tm_sec = d.getSecond();
      time_t e = mktime(&tmv);
      struct timeval tv = { .tv_sec = e, .tv_usec = 0 };
      settimeofday(&tv, nullptr);
    }
  }
  if (WiFi.status() != WL_CONNECTED) return;
  configTime(8 * 3600, 0, "pool.ntp.org", "ntp.aliyun.com", "time.cloudflare.com");
  struct tm ti;
  if (getLocalTime(&ti, 8000)) {
    if (rtcOK) rtc.setDateTime(RTC_DateTime(ti));
    Serial.printf("[time] NTP ok %04d-%02d-%02d %02d:%02d\n",
                  ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday, ti.tm_hour, ti.tm_min);
  } else {
    Serial.println("[time] NTP failed (using RTC if available)");
  }
}

// ---- sync status -------------------------------------------------------------
// Two-leg data path:  vendor API --(1)--> host(usage_server) --(2)--> this watch
//   leg 2 = can we reach the host (HTTP 200 recently)
//   leg 1 = did the host reach the vendor APIs this cycle (host's `ok` field)
enum SyncState { SYNC_OK, SYNC_SYNCING, SYNC_STALE, SYNC_OFFLINE };
static SyncState sync_state() {
  if (syncing) return SYNC_SYNCING;
  if (!g_wifi || lastOkFetchMs == 0 || !lastFetchOk ||
      (millis() - lastOkFetchMs) > 90000) return SYNC_OFFLINE;  // leg 2 down
  if (!snap.ok) return SYNC_STALE;                              // leg 1 down (host can't reach vendors)
  return SYNC_OK;
}
// relative age of the host's snapshot: "30s ago" / "5m ago" / "2h ago"
static void age_str(char *buf, size_t n) {
  time_t now = time(nullptr);
  if (snap.updated <= 0 || now < 1700000000) { buf[0] = 0; return; }
  long age = (long)now - snap.updated;
  if (age < 0) age = 0;
  if (age < 60)        snprintf(buf, n, lang == 1 ? "%ld秒前" : (lang == 2 ? "vor %lds" : "%lds ago"), age);
  else if (age < 3600) snprintf(buf, n, lang == 1 ? "%ld分前" : (lang == 2 ? "vor %ldm" : "%ldm ago"), age / 60);
  else                 snprintf(buf, n, lang == 1 ? "%ld时前" : (lang == 2 ? "vor %ldh" : "%ldh ago"), age / 3600);
}

// ---- LVGL UI -----------------------------------------------------------------
static lv_obj_t *uiArc;
static lv_obj_t *uiDot, *uiName, *uiClock, *uiBatt;
static lv_obj_t *uiToast;   // centered timezone toast
LV_IMG_DECLARE(img_claude);
LV_IMG_DECLARE(img_codex);
LV_FONT_DECLARE(lv_font_zh_12);
LV_FONT_DECLARE(lv_font_zh_14);
LV_FONT_DECLARE(lv_font_zh_16);
static lv_obj_t *uiLogo;                       // agent logo image
static lv_obj_t *uiTag[2], *uiBar[2], *uiPct[2], *uiReset[2];
static lv_obj_t *uiNode[3], *uiNodeLbl[3], *uiLink[2], *uiSyncDot, *uiSyncTxt;
static lv_obj_t *uiPortal;   // centered WiFi-setup notice (label, shown in portal mode)
static lv_obj_t *uiPageDot[4];
static lv_obj_t *uiSetupBox;   // page-4 WiFi setup screen
static lv_obj_t *uiSetTitle, *uiSetStat, *uiSetSsid, *uiSetIp, *uiSetBtn, *uiSetBtnLbl;
static lv_obj_t *uiCancelBtn, *uiCancelLbl;   // shown in portal mode to bail out
static lv_obj_t *uiInfoBox;   // page-3 scroll viewport
static lv_obj_t *infoHdr[4];  // page-3 section headers (NETWORK/POWER/DATA/SYSTEM)
static lv_obj_t *infoKey[16]; // page-3 row keys (dim left column)
static lv_obj_t *infoVal[16]; // page-3 value labels (filled by fill_info, in build order)
static lv_obj_t *uiInfoFoot;  // page-3 copyright footer

static lv_obj_t *mk_label(lv_obj_t *par, const lv_font_t *font, lv_color_t color,
                          lv_align_t align, int x, int y, const char *txt) {
  lv_obj_t *l = lv_label_create(par);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, color, 0);
  lv_label_set_text(l, txt);
  lv_obj_align(l, align, x, y);
  return l;
}

static void build_ui() {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  // edge usage arc — gap centered at the bottom (rotation 135, sweep 270)
  uiArc = lv_arc_create(scr);
  lv_obj_set_size(uiArc, 452, 452);
  lv_obj_center(uiArc);
  lv_arc_set_rotation(uiArc, 135);
  lv_arc_set_bg_angles(uiArc, 0, 270);
  lv_arc_set_range(uiArc, 0, 100);
  lv_arc_set_value(uiArc, 0);
  lv_obj_remove_style(uiArc, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(uiArc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(uiArc, 16, LV_PART_MAIN);
  lv_obj_set_style_arc_width(uiArc, 16, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(uiArc, COL_TRACK, LV_PART_MAIN);
  lv_obj_set_style_arc_color(uiArc, COL_CLAUDE, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(uiArc, true, LV_PART_INDICATOR);

  // header: dot + name
  uiDot = lv_obj_create(scr);
  lv_obj_set_size(uiDot, 9, 9);
  lv_obj_set_style_radius(uiDot, 5, 0);
  lv_obj_set_style_border_width(uiDot, 0, 0);
  lv_obj_set_style_bg_color(uiDot, COL_CLAUDE, 0);
  uiName = mk_label(scr, &lv_font_montserrat_20, lv_color_white(), LV_ALIGN_TOP_MID, 8, 40, "CLAUDE CODE");
  lv_obj_align_to(uiDot, uiName, LV_ALIGN_OUT_LEFT_MID, -8, 0);

  // clock + battery — flex row, auto-centered as a unit (batt width varies with the bolt)
  lv_obj_t *hdrRow = lv_obj_create(scr);
  lv_obj_remove_style_all(hdrRow);
  lv_obj_set_size(hdrRow, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(hdrRow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(hdrRow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(hdrRow, 16, 0);
  lv_obj_align(hdrRow, LV_ALIGN_TOP_MID, 0, 72);
  uiClock = lv_label_create(hdrRow);
  lv_obj_set_style_text_font(uiClock, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(uiClock, lv_color_hex(0x9098A0), 0);
  lv_label_set_text(uiClock, "--:--");
  uiBatt = lv_label_create(hdrRow);
  lv_obj_set_style_text_font(uiBatt, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(uiBatt, lv_color_hex(0xB0B4BC), 0);
  lv_label_set_recolor(uiBatt, true);
  lv_label_set_text(uiBatt, "100%");

  // agent logo (Claude pixel creature / Codex app icon), centered
  uiLogo = lv_img_create(scr);
  lv_img_set_src(uiLogo, &img_claude);
  lv_obj_align(uiLogo, LV_ALIGN_TOP_MID, 0, 120);

  // two meter rows (usage / weekly)
  const char *tags[2] = { "USAGE", "WEEKLY" };
  int my[2] = { 232, 286 };
  for (int i = 0; i < 2; i++) {
    uiTag[i] = mk_label(scr, &lv_font_montserrat_14, COL_LABEL, LV_ALIGN_TOP_LEFT, 52, my[i], tags[i]);
    uiBar[i] = lv_bar_create(scr);
    lv_obj_set_size(uiBar[i], 150, 10);
    lv_obj_align(uiBar[i], LV_ALIGN_TOP_LEFT, 130, my[i] + 3);
    lv_obj_set_style_radius(uiBar[i], 5, LV_PART_MAIN);
    lv_obj_set_style_radius(uiBar[i], 5, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(uiBar[i], lv_color_hex(0x1B1C20), LV_PART_MAIN);
    lv_obj_set_style_bg_color(uiBar[i], COL_CLAUDE, LV_PART_INDICATOR);
    lv_bar_set_range(uiBar[i], 0, 100);
    lv_bar_set_value(uiBar[i], 0, LV_ANIM_OFF);
    uiPct[i]   = mk_label(scr, &lv_font_montserrat_28, lv_color_white(), LV_ALIGN_TOP_RIGHT, -64, my[i] - 8, "--");
    uiReset[i] = mk_label(scr, &lv_font_montserrat_14, COL_DIM, LV_ALIGN_TOP_RIGHT, -56, my[i] + 22, "--");
  }

  // sync-status card: link diagram (CLAUDE/CODEX -> SERVER -> TERMINAL) + status line
  const int linkY = 354, nodeX[3] = { SIZE / 2 - 82, SIZE / 2, SIZE / 2 + 82 };
  for (int i = 0; i < 2; i++) {           // links first, nodes drawn on top
    uiLink[i] = lv_obj_create(scr);
    lv_obj_set_size(uiLink[i], 70, 3);
    lv_obj_set_style_radius(uiLink[i], 2, 0);
    lv_obj_set_style_border_width(uiLink[i], 0, 0);
    lv_obj_clear_flag(uiLink[i], LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(uiLink[i], nodeX[i] + 6, linkY - 1);
  }
  for (int i = 0; i < 3; i++) {
    uiNode[i] = lv_obj_create(scr);
    lv_obj_set_size(uiNode[i], 11, 11);
    lv_obj_set_style_radius(uiNode[i], LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(uiNode[i], 0, 0);
    lv_obj_clear_flag(uiNode[i], LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(uiNode[i], nodeX[i] - 5, linkY - 5);
  }
  const char *nlbl[3] = { "CLAUDE", "SERVER", "TERMINAL" };
  for (int i = 0; i < 3; i++)
    uiNodeLbl[i] = mk_label(scr, &lv_font_montserrat_12, lv_color_hex(0x808288),
                            LV_ALIGN_TOP_MID, nodeX[i] - SIZE / 2, linkY + 12, nlbl[i]);
  uiSyncTxt = mk_label(scr, &lv_font_montserrat_14, lv_color_hex(0xB0B4BC), LV_ALIGN_TOP_MID, 8, 388, "...");
  uiSyncDot = lv_obj_create(scr);
  lv_obj_set_size(uiSyncDot, 8, 8);
  lv_obj_set_style_radius(uiSyncDot, 4, 0);
  lv_obj_set_style_border_width(uiSyncDot, 0, 0);
  lv_obj_clear_flag(uiSyncDot, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_align_to(uiSyncDot, uiSyncTxt, LV_ALIGN_OUT_LEFT_MID, -8, 0);

  // page dots (4 pages now: Claude / Codex / Info / Setup)
  for (int i = 0; i < 4; i++) {
    uiPageDot[i] = lv_obj_create(scr);
    lv_obj_set_style_border_width(uiPageDot[i], 0, 0);
    lv_obj_set_style_radius(uiPageDot[i], 4, 0);
    lv_obj_clear_flag(uiPageDot[i], LV_OBJ_FLAG_SCROLLABLE);
  }

  // page-3 info/diagnostics list (left-aligned multiline label, hidden unless on page 3)
  // page-3: a fixed scroll viewport (the round screen's middle band). Inside, a flex
  // column of colored section headers + key/value rows (real 2-column alignment).
  // Vertical finger-drag scrolls it (handled in loop()).
  uiInfoBox = lv_obj_create(scr);
  lv_obj_set_size(uiInfoBox, 330, 322);
  lv_obj_align(uiInfoBox, LV_ALIGN_TOP_MID, 0, 70);
  lv_obj_set_style_bg_opa(uiInfoBox, LV_OPA_0, 0);
  lv_obj_set_style_border_width(uiInfoBox, 0, 0);
  lv_obj_set_style_pad_left(uiInfoBox, 18, 0);
  lv_obj_set_style_pad_right(uiInfoBox, 10, 0);
  lv_obj_set_style_pad_ver(uiInfoBox, 2, 0);
  lv_obj_set_flex_flow(uiInfoBox, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(uiInfoBox, 6, 0);
  lv_obj_set_scroll_dir(uiInfoBox, LV_DIR_VER);
  lv_obj_clear_flag(uiInfoBox, LV_OBJ_FLAG_SCROLL_ELASTIC);   // hard edges, no rubber-band whitespace
  lv_obj_clear_flag(uiInfoBox, LV_OBJ_FLAG_SCROLL_MOMENTUM);
  lv_obj_set_scrollbar_mode(uiInfoBox, LV_SCROLLBAR_MODE_ACTIVE);
  lv_obj_set_style_bg_color(uiInfoBox, lv_color_hex(0x4A4E58), LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_opa(uiInfoBox, LV_OPA_70, LV_PART_SCROLLBAR);
  lv_obj_set_style_width(uiInfoBox, 3, LV_PART_SCROLLBAR);
  lv_obj_add_flag(uiInfoBox, LV_OBJ_FLAG_HIDDEN);

  struct { const char *hdr; uint32_t col; const char *keys[6]; } sect[] = {
    { "NETWORK", 0x7AA2F7, { "SSID", "IP", "Gateway", "Signal", "MAC", nullptr } },
    { "POWER",   0x9ECE6A, { "Battery", "Voltage", "Uptime", nullptr } },
    { "DATA",    0x56C5C0, { "Source", "Refresh", "Timezone", "Updated", "Cloud", nullptr } },
    { "SYSTEM",  0x9098A0, { "Free RAM", "Clock", "Firmware", nullptr } },
  };
  int vi = 0, si = 0;
  for (auto &s : sect) {
    lv_obj_t *h = lv_label_create(uiInfoBox);
    lv_obj_set_style_text_font(h, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(h, lv_color_hex(s.col), 0);
    lv_obj_set_style_pad_top(h, si ? 10 : 0, 0);   // breathing room above each section
    lv_label_set_text(h, s.hdr);
    infoHdr[si++] = h;
    for (int k = 0; s.keys[k]; k++) {
      lv_obj_t *row = lv_obj_create(uiInfoBox);
      lv_obj_remove_style_all(row);
      lv_obj_set_width(row, lv_pct(100));
      lv_obj_set_height(row, LV_SIZE_CONTENT);
      lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
      lv_obj_t *kl = lv_label_create(row);
      lv_obj_set_width(kl, 96);
      lv_obj_set_style_text_font(kl, &lv_font_montserrat_14, 0);
      lv_obj_set_style_text_color(kl, lv_color_hex(0x7C828C), 0);
      lv_label_set_text(kl, s.keys[k]);
      infoKey[vi] = kl;
      lv_obj_t *vl = lv_label_create(row);
      lv_obj_set_flex_grow(vl, 1);
      lv_label_set_long_mode(vl, LV_LABEL_LONG_WRAP);
      lv_obj_set_style_text_font(vl, &lv_font_montserrat_14, 0);
      lv_obj_set_style_text_color(vl, lv_color_hex(0xD2D6DE), 0);
      lv_label_set_text(vl, "");
      infoVal[vi++] = vl;
    }
  }
  // copyright footer (scrolls in at the very bottom)
  uiInfoFoot = lv_label_create(uiInfoBox);
  lv_obj_set_width(uiInfoFoot, lv_pct(100));
  lv_obj_set_style_pad_top(uiInfoFoot, 16, 0);
  lv_obj_set_style_text_align(uiInfoFoot, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(uiInfoFoot, &lv_font_zh_14, 0);   // has the © / · glyphs
  lv_obj_set_style_text_color(uiInfoFoot, lv_color_hex(0x60656E), 0);
  lv_label_set_text(uiInfoFoot, "TokenGenie · © 2026 Jensen-JZ");

  // page-4: WiFi setup screen — connection status + a tappable "start setup" button.
  // (touch is polled manually, so the button is hit-tested in loop(); no LVGL indev.)
  uiSetupBox = lv_obj_create(scr);
  lv_obj_set_size(uiSetupBox, 380, 360);
  lv_obj_center(uiSetupBox);
  lv_obj_set_style_bg_opa(uiSetupBox, LV_OPA_0, 0);
  lv_obj_set_style_border_width(uiSetupBox, 0, 0);
  lv_obj_clear_flag(uiSetupBox, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(uiSetupBox, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(uiSetupBox, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_add_flag(uiSetupBox, LV_OBJ_FLAG_HIDDEN);

  uiSetTitle = lv_label_create(uiSetupBox);
  lv_obj_set_style_text_font(uiSetTitle, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(uiSetTitle, lv_color_hex(0x9098A0), 0);
  lv_obj_set_style_pad_bottom(uiSetTitle, 18, 0);
  lv_label_set_text(uiSetTitle, "WiFi SETUP");

  uiSetStat = lv_label_create(uiSetupBox);
  lv_obj_set_style_text_font(uiSetStat, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(uiSetStat, lv_color_hex(0x3CCB7F), 0);
  lv_label_set_text(uiSetStat, "");

  uiSetSsid = lv_label_create(uiSetupBox);
  lv_obj_set_style_text_font(uiSetSsid, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(uiSetSsid, lv_color_hex(0xD2D6DE), 0);
  lv_obj_set_style_pad_top(uiSetSsid, 8, 0);
  lv_label_set_text(uiSetSsid, "");

  uiSetIp = lv_label_create(uiSetupBox);
  lv_obj_set_style_text_font(uiSetIp, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(uiSetIp, lv_color_hex(0x7C828C), 0);
  lv_obj_set_style_pad_bottom(uiSetIp, 30, 0);   // gap before the button
  lv_label_set_text(uiSetIp, "");

  uiSetBtn = lv_obj_create(uiSetupBox);
  lv_obj_set_size(uiSetBtn, 210, 60);
  lv_obj_set_style_radius(uiSetBtn, 30, 0);
  lv_obj_set_style_bg_color(uiSetBtn, lv_color_hex(0x35B8FF), 0);
  lv_obj_set_style_border_width(uiSetBtn, 0, 0);
  lv_obj_clear_flag(uiSetBtn, LV_OBJ_FLAG_SCROLLABLE);
  uiSetBtnLbl = lv_label_create(uiSetBtn);
  lv_obj_center(uiSetBtnLbl);
  lv_obj_set_style_text_font(uiSetBtnLbl, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(uiSetBtnLbl, lv_color_hex(0x05131C), 0);
  lv_label_set_text(uiSetBtnLbl, "Start Setup");

  // WiFi-setup notice (centered label; shown over the logo area in portal mode — no
  // full-screen opaque overlay, which stalled the full_refresh render loop)
  uiPortal = lv_label_create(scr);
  lv_obj_set_style_text_font(uiPortal, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(uiPortal, lv_color_hex(0x35B8FF), 0);
  lv_obj_set_style_text_align(uiPortal, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(uiPortal, "WiFi SETUP\n\njoin hotspot:\nTokenGenie-Setup");
  lv_obj_align(uiPortal, LV_ALIGN_CENTER, 0, -40);
  lv_obj_add_flag(uiPortal, LV_OBJ_FLAG_HIDDEN);

  // Cancel button (portal mode only) — bail out of config back to the dashboard
  uiCancelBtn = lv_obj_create(scr);
  lv_obj_set_size(uiCancelBtn, 180, 54);
  lv_obj_align(uiCancelBtn, LV_ALIGN_CENTER, 0, 140);
  lv_obj_set_style_radius(uiCancelBtn, 27, 0);
  lv_obj_set_style_bg_color(uiCancelBtn, lv_color_hex(0x2A2D33), 0);
  lv_obj_set_style_border_width(uiCancelBtn, 1, 0);
  lv_obj_set_style_border_color(uiCancelBtn, lv_color_hex(0x55585F), 0);
  lv_obj_clear_flag(uiCancelBtn, LV_OBJ_FLAG_SCROLLABLE);
  uiCancelLbl = lv_label_create(uiCancelBtn);
  lv_obj_center(uiCancelLbl);
  lv_obj_set_style_text_font(uiCancelLbl, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(uiCancelLbl, lv_color_hex(0xC2C6CE), 0);
  lv_label_set_text(uiCancelLbl, "Cancel");
  lv_obj_add_flag(uiCancelBtn, LV_OBJ_FLAG_HIDDEN);

  // timezone toast — a big centered pill that flashes for ~2s when the timezone changes
  uiToast = lv_label_create(scr);
  lv_obj_set_style_text_font(uiToast, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(uiToast, lv_color_white(), 0);
  lv_obj_set_style_bg_color(uiToast, lv_color_hex(0x202428), 0);
  lv_obj_set_style_bg_opa(uiToast, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(uiToast, 16, 0);
  lv_obj_set_style_radius(uiToast, 12, 0);
  lv_label_set_text(uiToast, "UTC+8");
  lv_obj_center(uiToast);
  lv_obj_add_flag(uiToast, LV_OBJ_FLAG_HIDDEN);
}

// update widgets for the current page + live data
// hide/show the dashboard CORE widgets (arc/header/meter/sync-card) — excludes dots & info
static void set_core_hidden(bool hide) {
  lv_obj_t *objs[] = { uiArc, uiDot, uiName, uiClock, uiBatt, uiLogo,
    uiTag[0], uiTag[1], uiBar[0], uiBar[1], uiPct[0], uiPct[1], uiReset[0], uiReset[1],
    uiNode[0], uiNode[1], uiNode[2], uiNodeLbl[0], uiNodeLbl[1], uiNodeLbl[2],
    uiLink[0], uiLink[1], uiSyncDot, uiSyncTxt };
  for (lv_obj_t *o : objs) { if (hide) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN); else lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN); }
}
// section headers + row keys, indexed by build order, for the 3 languages
static const char *INFO_HDR[4][3] = {
  { "NETWORK", "网络", "Netzwerk" }, { "POWER", "电源", "Energie" },
  { "DATA", "数据", "Daten" },       { "SYSTEM", "系统", "System" },
};
static const char *INFO_KEY[16][3] = {
  { "SSID", "SSID", "SSID" },        { "IP", "IP", "IP" },
  { "Gateway", "网关", "Gateway" },  { "Signal", "信号", "Signal" },
  { "MAC", "MAC", "MAC" },           { "Battery", "电池", "Akku" },
  { "Voltage", "电压", "Spannung" }, { "Uptime", "运行", "Laufzeit" },
  { "Source", "来源", "Quelle" },    { "Refresh", "刷新", "Intervall" },
  { "Timezone", "时区", "Zeitzone" },{ "Updated", "更新", "Aktuell." },
  { "Cloud", "云端", "Cloud" },      { "Free RAM", "内存", "RAM" },
  { "Clock", "时钟", "Uhr" },        { "Firmware", "固件", "Firmware" },
};
// fill the page-3 diagnostics rows (value labels, same order as built in build_ui)
static void fill_info() {
  // i18n: translate headers/keys and swap fonts (Chinese needs the zh glyph font)
  const lv_font_t *f = (lang == 1) ? &lv_font_zh_14 : &lv_font_montserrat_14;
  for (int i = 0; i < 4; i++) {
    lv_obj_set_style_text_font(infoHdr[i], f, 0);
    lv_label_set_text(infoHdr[i], pickStr(INFO_HDR[i][0], INFO_HDR[i][1], INFO_HDR[i][2]));
  }
  for (int i = 0; i < 16; i++) {
    lv_obj_set_style_text_font(infoKey[i], f, 0);
    lv_label_set_text(infoKey[i], pickStr(INFO_KEY[i][0], INFO_KEY[i][1], INFO_KEY[i][2]));
    lv_obj_set_style_text_font(infoVal[i], f, 0);
  }
  char b[40];
  uint16_t mv = batMv;   // cached from read_power(); avoids a contended inline PMU read
  uint32_t up = millis() / 1000;
  char age[24]; age_str(age, sizeof(age));
  char clk[8]; clock_str(clk, sizeof(clk));
  // NETWORK
  lv_label_set_text(infoVal[0], g_wifi ? WiFi.SSID().c_str() : "--");
  lv_label_set_text(infoVal[1], g_wifi ? WiFi.localIP().toString().c_str() : "--");
  lv_label_set_text(infoVal[2], g_wifi ? WiFi.gatewayIP().toString().c_str() : "--");
  snprintf(b, sizeof(b), "%d dBm  ch %d", g_wifi ? WiFi.RSSI() : 0, g_wifi ? WiFi.channel() : 0);
  lv_label_set_text(infoVal[3], b);
  lv_label_set_text(infoVal[4], WiFi.macAddress().c_str());
  // POWER
  snprintf(b, sizeof(b), "%d%%  %s", batt_ui(),
           charging ? pickStr("(charging)", "充电中", "(laden)") : "");
  lv_label_set_text(infoVal[5], b);
  snprintf(b, sizeof(b), "%d.%02d V", mv / 1000, (mv % 1000) / 10);
  lv_label_set_text(infoVal[6], b);
  snprintf(b, sizeof(b), "%luh %02lum", (unsigned long)(up / 3600), (unsigned long)((up % 3600) / 60));
  lv_label_set_text(infoVal[7], b);
  // DATA
  lv_label_set_text(infoVal[8], "ccusage.peritrix.com");
  snprintf(b, sizeof(b), "%lus", (unsigned long)(FETCH_INTERVAL_MS / 1000));
  lv_label_set_text(infoVal[9], b);
  snprintf(b, sizeof(b), "UTC%+d", cur_utc_offset());
  lv_label_set_text(infoVal[10], b);
  lv_label_set_text(infoVal[11], age[0] ? age : "--");
  lv_label_set_text(infoVal[12], snap.ok ? "OK" : "--");
  // SYSTEM
  snprintf(b, sizeof(b), "%lu KB", (unsigned long)(ESP.getFreeHeap() / 1024));
  lv_label_set_text(infoVal[13], b);
  lv_label_set_text(infoVal[14], clk);
  lv_label_set_text(infoVal[15], __DATE__);
}
// fill the page-4 setup screen (status + button) with current connection + i18n
static void fill_setup() {
  const lv_font_t *f16 = (lang == 1) ? &lv_font_zh_16 : &lv_font_montserrat_16;
  lv_obj_set_style_text_font(uiSetTitle, f16, 0);
  lv_label_set_text(uiSetTitle, pickStr("WiFi SETUP", "网络配置", "WLAN Setup"));
  lv_obj_set_style_text_font(uiSetStat, (lang == 1) ? &lv_font_zh_16 : &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_font(uiSetSsid, f16, 0);
  lv_obj_set_style_text_font(uiSetBtnLbl, f16, 0);
  if (g_wifi) {        // connected: status + the button becomes "Disconnect"
    lv_obj_set_style_text_color(uiSetStat, lv_color_hex(0x3CCB7F), 0);
    lv_label_set_text(uiSetStat, pickStr("Connected", "已连接", "Verbunden"));
    lv_label_set_text(uiSetSsid, WiFi.SSID().c_str());
    lv_label_set_text(uiSetIp, WiFi.localIP().toString().c_str());
    lv_obj_set_style_bg_color(uiSetBtn, lv_color_hex(0x8A3A3A), 0);   // muted red
    lv_obj_set_style_text_color(uiSetBtnLbl, lv_color_hex(0xFFE6E6), 0);
    lv_label_set_text(uiSetBtnLbl, pickStr("Disconnect", "断开连接", "Trennen"));
  } else {             // offline: the button becomes "config WiFi"
    lv_obj_set_style_text_color(uiSetStat, lv_color_hex(0xFF5D5D), 0);
    lv_label_set_text(uiSetStat, pickStr("Offline", "离线", "Getrennt"));
    lv_label_set_text(uiSetSsid, pickStr("no network", "无网络", "kein Netz"));
    lv_label_set_text(uiSetIp, "");
    lv_obj_set_style_bg_color(uiSetBtn, lv_color_hex(0x35B8FF), 0);   // blue
    lv_obj_set_style_text_color(uiSetBtnLbl, lv_color_hex(0x05131C), 0);
    lv_label_set_text(uiSetBtnLbl, pickStr("Start Setup", "配置 WiFi", "Einrichten"));
  }
}
static void update_ui() {
  bool dash  = (page <= 1);
  bool info  = (page == 2);
  bool setup = (page == 3);
  set_core_hidden(!dash);
  if (info)  lv_obj_clear_flag(uiInfoBox, LV_OBJ_FLAG_HIDDEN);  else lv_obj_add_flag(uiInfoBox, LV_OBJ_FLAG_HIDDEN);
  if (setup) lv_obj_clear_flag(uiSetupBox, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(uiSetupBox, LV_OBJ_FLAG_HIDDEN);
  { int n = 4, gap = 7, dotW = 7, longW = 18, y = 438;            // page dots (4)
    int total = 0; for (int i = 0; i < n; i++) total += (i == page ? longW : dotW) + (i ? gap : 0);
    int x = (SIZE - total) / 2;
    lv_color_t dc = page == 0 ? COL_CLAUDE : page == 1 ? COL_CODEX
                  : page == 2 ? lv_color_hex(0x9098A0) : lv_color_hex(0x35B8FF);
    for (int i = 0; i < n; i++) { int wdt = (i == page ? longW : dotW); lv_obj_set_size(uiPageDot[i], wdt, 7);
      lv_obj_set_style_bg_color(uiPageDot[i], i == page ? dc : lv_color_hex(0x3A3C41), 0); lv_obj_set_pos(uiPageDot[i], x, y); x += wdt + gap; } }
  if (setup) { fill_setup(); return; }
  if (info)  { fill_info();  return; }

  bool isClaude = (page == 0);
  const AgentUsage &ag = isClaude ? snap.claude : snap.codex;
  lv_color_t color = isClaude ? COL_CLAUDE : COL_CODEX;

  // i18n: swap font + text for the translated labels (numbers/clock/battery/brand stay)
  const lv_font_t *fz12 = (lang == 1) ? &lv_font_zh_12 : &lv_font_montserrat_12;
  const lv_font_t *fz14 = (lang == 1) ? &lv_font_zh_14 : &lv_font_montserrat_14;
  lv_obj_set_style_text_font(uiTag[0], fz14, 0);
  lv_obj_set_style_text_font(uiTag[1], fz14, 0);
  lv_label_set_text(uiTag[0], pickStr("USAGE", "用量", "Nutzung"));
  lv_label_set_text(uiTag[1], pickStr("WEEKLY", "周用量", "Woche"));
  lv_obj_set_style_text_font(uiNodeLbl[1], fz12, 0);
  lv_obj_set_style_text_font(uiNodeLbl[2], fz12, 0);
  lv_label_set_text(uiNodeLbl[1], pickStr("SERVER", "服务器", "Server"));
  lv_label_set_text(uiNodeLbl[2], pickStr("TERMINAL", "终端", "Terminal"));
  lv_obj_set_style_text_font(uiSyncTxt, fz14, 0);
  lv_obj_set_style_text_font(uiReset[0], fz14, 0);
  lv_obj_set_style_text_font(uiReset[1], fz14, 0);

  lv_label_set_text(uiName, isClaude ? "CLAUDE CODE" : "CODEX");
  lv_label_set_text(uiNodeLbl[0], isClaude ? "CLAUDE" : "CODEX");
  lv_obj_set_style_bg_color(uiDot, color, 0);
  lv_obj_align_to(uiDot, uiName, LV_ALIGN_OUT_LEFT_MID, -8, 0);
  lv_img_set_src(uiLogo, isClaude ? &img_claude : &img_codex);

  int s = ag.h5.util, w = ag.d7.util;
  int arcPct = w < 0 ? 0 : w;   // edge arc always shows the 7-day (weekly) window
  lv_obj_set_style_arc_color(uiArc, color, LV_PART_INDICATOR);
  lv_arc_set_value(uiArc, arcPct);

  int uv[2] = { s, w };
  bool is7d[2] = { false, true };
  for (int i = 0; i < 2; i++) {
    int u = uv[i];
    bool hot = u >= 85;
    lv_obj_set_style_bg_color(uiBar[i], hot ? COL_DANGER : color, LV_PART_INDICATOR);
    lv_bar_set_value(uiBar[i], u < 0 ? 0 : u, LV_ANIM_OFF);
    char pc[8];
    if (u < 0) snprintf(pc, sizeof(pc), "--"); else snprintf(pc, sizeof(pc), "%d%%", u);
    lv_label_set_text(uiPct[i], pc);
    char r[24]; reset_str(r, sizeof(r), (i == 0 ? ag.h5 : ag.d7).reset_at, is7d[i]);
    lv_label_set_text(uiReset[i], r);
  }

  char clk[20]; clock_str(clk, sizeof(clk));
  lv_label_set_text(uiClock, clk);
  char bs[40];   // bolt: green=plugged, red=low (<=20%) on battery, grey=normal (# #=recolor)
  int bp = batt_ui();
  const char *bc = charging ? "#3CCB7F " : (bp <= 20 ? "#FF5D5D " : "#6A6D74 ");
  snprintf(bs, sizeof(bs), "%s%s#  %d%%", bc, LV_SYMBOL_CHARGE, bp);
  lv_label_set_text(uiBatt, bs);

  // sync-status card
  SyncState st = sync_state();
  lv_color_t green = lv_color_hex(0x3CCB7F), red = COL_DANGER,
             blue = lv_color_hex(0x35B8FF), amber = lv_color_hex(0xE0B020),
             dim = lv_color_hex(0x44474E);
  lv_color_t l1, l2, sd, n0, n1, n2;
  const char *word;
  switch (st) {
    case SYNC_SYNCING: l1 = blue;  l2 = blue;  sd = blue;  n0 = blue; n1 = blue;  n2 = blue;  word = pickStr("SYNCING", "同步中", "Sync"); break;
    case SYNC_STALE:   l1 = red;   l2 = green; sd = amber; n0 = red;  n1 = green; n2 = green; word = pickStr("STALE", "数据陈旧", "Veraltet");   break;
    case SYNC_OFFLINE: l1 = dim;   l2 = red;   sd = red;   n0 = dim;  n1 = dim;   n2 = red;   word = pickStr("OFFLINE", "离线", "Offline"); break;
    default:           l1 = green; l2 = green; sd = green; n0 = green; n1 = green; n2 = green; word = pickStr("SYNCED", "已同步", "Aktuell"); break;
  }
  lv_obj_set_style_bg_color(uiLink[0], l1, 0);
  lv_obj_set_style_bg_color(uiLink[1], l2, 0);
  lv_obj_set_style_bg_color(uiNode[0], n0, 0);
  lv_obj_set_style_bg_color(uiNode[1], n1, 0);
  lv_obj_set_style_bg_color(uiNode[2], n2, 0);
  lv_obj_set_style_bg_color(uiSyncDot, sd, 0);
  char age[24], line[44];
  age_str(age, sizeof(age));
  if ((st == SYNC_OK || st == SYNC_STALE) && age[0])
    snprintf(line, sizeof(line), "%s  %s", word, age);
  else
    snprintf(line, sizeof(line), "%s", word);
  lv_label_set_text(uiSyncTxt, line);
  lv_obj_align_to(uiSyncDot, uiSyncTxt, LV_ALIGN_OUT_LEFT_MID, -8, 0);
}

// boot banner on the sync-status line (connecting / setup) — keeps the dot glued to the text
static void sync_banner(const char *txt, lv_color_t c) {
  if (!uiSyncTxt) return;
  lv_label_set_text(uiSyncTxt, txt);
  lv_obj_set_style_bg_color(uiSyncDot, c, 0);
  lv_obj_align_to(uiSyncDot, uiSyncTxt, LV_ALIGN_OUT_LEFT_MID, -8, 0);
}

// ---- networking --------------------------------------------------------------
static void set_status(const char *s) {
  strncpy(g_status, s, sizeof(g_status) - 1);
  g_status[sizeof(g_status) - 1] = 0;
}
static void parse_win(JsonVariantConst o, Win &w) {
  if (o.isNull()) { w.util = -1; w.reset_at = 0; return; }
  w.util = o["util"] | -1;
  w.reset_at = o["reset_at"] | 0L;
}
static void fetch_usage() {
  if (WiFi.status() != WL_CONNECTED) { lastFetchOk = false; set_status("WiFi..."); return; }
  set_status("updating");
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(6);
  HTTPClient https;
  https.setConnectTimeout(6000);
  https.setTimeout(6000);
  String url = String(METER_URL) + "?key=" + METER_KEY;
  if (!https.begin(client, url)) { lastFetchOk = false; set_status("conn err"); return; }
  int code = https.GET();
  if (code == 200) {
    String payload = https.getString();
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (err) { lastFetchOk = false; set_status("json err"); }
    else {
      parse_win(doc["claude"]["five_hour"], snap.claude.h5);
      parse_win(doc["claude"]["seven_day"], snap.claude.d7);
      parse_win(doc["codex"]["five_hour"], snap.codex.h5);
      parse_win(doc["codex"]["seven_day"], snap.codex.d7);
      snap.updated = doc["updated"] | 0L;
      snap.ok = doc["ok"] | false;
      snap.valid = true;
      lastOkFetchMs = millis();
      lastFetchOk = true;
      set_status("");
    }
  } else {
    lastFetchOk = false;
    char b[24]; snprintf(b, sizeof(b), "http %d", code);
    set_status(b);
  }
  https.end();
}

// ---- WiFi --------------------------------------------------------------------
static void lvgl_pump(int ms) {
  uint32_t t0 = millis();
  do {
    uint32_t now = millis();
    lv_tick_inc(now - lastTick); lastTick = now;
    lv_timer_handler();
    delay(5);
  } while (millis() - t0 < (uint32_t)ms);
}
static WiFiManager wm;          // global so the non-blocking portal can be driven from loop()
static bool portalActive = false;
static uint32_t g_btnLock = 0;   // after any portal enter/exit, ignore button taps until this time
static uint32_t portalStart = 0;

// Hide/show every dashboard widget so the portal page is a clean black screen with just
// the notice (a full-screen opaque lv_obj overlay stalled the full_refresh render loop).
static void set_dashboard_hidden(bool hide) {   // portal: hide EVERYTHING (core + dots + info)
  set_core_hidden(hide);
  lv_obj_t *extra[] = { uiPageDot[0], uiPageDot[1], uiPageDot[2], uiPageDot[3], uiInfoBox, uiSetupBox };
  for (lv_obj_t *o : extra) {
    if (hide) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else      lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
  }
}

// (re)apply the portal notice + Cancel-button text for the current language. Called on enter
// and whenever BOOT cycles the language while the portal is up.
static void set_portal_text() {
  const lv_font_t *f = (lang == 1) ? &lv_font_zh_16 : &lv_font_montserrat_16;
  lv_obj_set_style_text_font(uiPortal, f, 0);
  lv_label_set_text(uiPortal, pickStr("WiFi SETUP\n\njoin hotspot:\nTokenGenie-Setup\n\nthen open\n192.168.4.1",
                                 "WiFi 配置\n\n手机连接热点:\nTokenGenie-Setup\n\n192.168.4.1",
                                 "WLAN Setup\n\nHotspot:\nTokenGenie-Setup\n\noeffne\n192.168.4.1"));
  lv_obj_align(uiPortal, LV_ALIGN_CENTER, 0, -40);
  lv_obj_set_style_text_font(uiCancelLbl, f, 0);
  lv_label_set_text(uiCancelLbl, pickStr("Cancel", "取消", "Abbrechen"));
}
static void enterPortal(bool wipe) {
  if (portalActive) return;
  Serial.println("[portal] enter");
  WiFi.setAutoReconnect(true);                       // undo any earlier manual disconnect
  WiFi.mode(WIFI_STA);                               // power the radio back on if Disconnect turned it off
  delay(50);
  set_dashboard_hidden(true);                        // clean black page — hide all widgets
  set_portal_text();
  lv_obj_clear_flag(uiPortal, LV_OBJ_FLAG_HIDDEN);   // show the setup notice
  lv_obj_clear_flag(uiCancelBtn, LV_OBJ_FLAG_HIDDEN);
  lv_timer_handler();                                // paint NOW — startAP() below blocks ~1-2s
  if (wipe) wm.resetSettings();
  // Drop the STA link first: WiFiManager only starts a clean AP-ONLY portal when STA is
  // disconnected. AP+STA coexist (single radio) often starts a softAP that won't broadcast.
  WiFi.disconnect(false, false);        // keep WiFi on, keep saved creds
  delay(60);
  wm.setConfigPortalBlocking(false);    // non-blocking — loop() drives wm.process()
  wm.setConfigPortalTimeout(0);
  Serial.println("[portal] startConfigPortal...");
  wm.startConfigPortal("TokenGenie-Setup");
  Serial.printf("[portal] up, AP IP=%s\n", WiFi.softAPIP().toString().c_str());
  portalActive = true;
  portalStart = millis();
  g_btnLock = millis() + 1200;   // don't let the same/next tap immediately Cancel
}
static void exitPortal() {
  if (!portalActive) return;
  portalActive = false;
  // 1) Update the SCREEN FIRST so Cancel feels instant. The WiFi teardown below blocks the
  //    main loop ~1s; doing it after the repaint means the dashboard is already on-screen.
  lv_obj_add_flag(uiPortal, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(uiCancelBtn, LV_OBJ_FLAG_HIDDEN);
  set_dashboard_hidden(false);
  g_btnLock = millis() + 1200;           // don't let the same/next tap immediately re-open the portal
  page = 0;                              // leave page 4 -> dashboard, so no tap can re-enter the portal
  update_ui();
  lv_timer_handler();                    // paint the dashboard NOW, before the blocking WiFi calls
  // 2) Blocking WiFi teardown (screen already shows the dashboard)
  wm.stopConfigPortal();
  if (WiFi.status() == WL_CONNECTED) {
    WiFi.softAPdisconnect(false);     // configured a new network: keep the STA link, drop only the AP
  } else {
    WiFi.disconnect(true, false);     // cancelled/timeout: power the radio off -> stay offline (no reconnect)
  }
  g_wifi = (WiFi.status() == WL_CONNECTED);
  Serial.println("[portal] exit done");
}
// page-4 button when connected: disconnect and stay offline (no auto-reconnect) until the
// user taps "config" again. Saved creds are kept, so a reboot still reconnects.
static void wifi_disconnect() {
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(true, false);          // wifioff=true: power down the STA radio -> truly stays
  delay(60);                             // offline (SDK can't auto-reconnect); eraseap=false keeps creds
  g_wifi = false;
  update_ui();                           // button flips back to "config WiFi"
}
static bool wait_wifi(uint32_t ms) {
  uint32_t t0 = millis();
  while (millis() - t0 < ms) {
    if (WiFi.status() == WL_CONNECTED) return true;
    lvgl_pump(20);
  }
  return WiFi.status() == WL_CONNECTED;
}
static void wifi_bringup(bool forcePortal) {
  WiFi.mode(WIFI_STA);
  sync_banner("connecting...", lv_color_hex(0x35B8FF));
  lvgl_pump(40);
  if (forcePortal) { enterPortal(true); return; }   // loop() drives the portal
  WiFi.begin();                                       // 1) saved creds
  if (!wait_wifi(9000)) {
    WiFi.begin(WIFI_SSID, WIFI_PASS);                 // 2) hardcoded seed
    if (!wait_wifi(9000)) { enterPortal(false); return; }   // 3) portal (non-blocking)
  }
  g_wifi = (WiFi.status() == WL_CONNECTED);
}

// ---- setup / loop ------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);   // never block on USB-CDC writes when no host is reading (was causing UI jank)
  pinMode(0, INPUT_PULLUP);   // BOOT button (active-low)

  gfx->begin();
  gfx->setBrightness(200);
  gfx->fillScreen(0x0000);

  lv_init();
  // partial render: two small PSRAM buffers, redraw only dirty areas. (full_refresh on a
  // 466x466 panel repainted the whole screen every frame — that was the stutter.)
  size_t bufpx = SIZE * 48;
  static lv_color_t *buf1, *buf2;
  buf1 = (lv_color_t *)heap_caps_malloc(bufpx * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  buf2 = (lv_color_t *)heap_caps_malloc(bufpx * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  if (!buf1 || !buf2) Serial.println("draw buffer alloc FAILED");
  g_fb = (uint16_t *)heap_caps_malloc(SIZE * SIZE * 2, MALLOC_CAP_SPIRAM);   // screenshot mirror
  lv_disp_draw_buf_init(&draw_buf, buf1, buf2, bufpx);
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = SIZE;
  disp_drv.ver_res = SIZE;
  disp_drv.flush_cb = disp_flush;
  disp_drv.rounder_cb = disp_rounder;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  build_ui();
  lastTick = millis();
  update_ui();
  lvgl_pump(20);

  // Shared I2C bus: touch (0x5A), AXP2101 PMU (0x34), PCF85063 RTC (0x51).
  Wire.begin(IIC_SDA, IIC_SCL);
  pmuOK = pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (pmuOK) {
    pmu.enableBattDetection();
    pmu.setPowerKeyPressOffTime(XPOWERS_POWEROFF_10S);   // long-press PWR ~10s -> hardware power off
    pmu.clearIrqStatus();
    pmu.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ | XPOWERS_AXP2101_PKEY_POSITIVE_IRQ | XPOWERS_AXP2101_PKEY_NEGATIVE_IRQ);   // short=page, press/release edges = power-off timing
    Serial.println("pmu ok");
  } else Serial.println("pmu NOT found");
  rtcOK = rtc.begin(Wire, IIC_SDA, IIC_SCL);
  Serial.println(rtcOK ? "rtc ok" : "rtc NOT found");

  pinMode(TP_RESET, OUTPUT);
  digitalWrite(TP_RESET, LOW);  delay(30);
  digitalWrite(TP_RESET, HIGH); delay(50);
  touch.setPins(TP_RESET, TP_INT);
  touchOK = touch.begin(Wire, 0x5A, IIC_SDA, IIC_SCL);
  if (touchOK) {
    touch.setMaxCoordinates(466, 466);
    touch.setMirrorXY(true, true);
    // NB: no touch interrupt — we poll touch_pressed(). A GPIO ISR firing during the WiFi
    // portal's NVS/flash writes (cache disabled) was hard-freezing the board on Cancel/enter.
    Serial.println("touch ok");
  } else Serial.println("touch NOT found");

  bool forcePortal = false;
  if (touchOK && touch_pressed()) {
    uint32_t t0 = millis();
    forcePortal = true;
    while (millis() - t0 < 1200) {
      if (!touch_pressed()) { forcePortal = false; break; }
      delay(50);
    }
  }

  wifi_bringup(forcePortal);
  time_sync();
  read_power();
  if (!portalActive) update_ui();
  Serial.println("setup done");
}

static void go_page(int p) {                                      // land on page p, reset info scroll
  page = p;
  if (page == 2) lv_obj_scroll_to_y(uiInfoBox, 0, LV_ANIM_OFF);
  update_ui();
}
static void on_tap() { go_page((page + 1) % 4); }                 // next page (PWR / serial 'N')
static void on_swipe(int dir) { go_page((page + dir + 4) % 4); }  // +1 left, -1 right

void loop() {
  uint32_t now = millis();
  lv_tick_inc(now - lastTick); lastTick = now;
  lv_timer_handler();
  static bool swallowTap = false;   // true after a portal Cancel: ignore the still-down finger

  // serial debug: 'N' -> next page
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'N') on_tap();
    else if (c == 'D' && g_fb) {   // dump full-screen mirror as a screenshot
      Serial.setTxTimeoutMs(2000);   // let the 434 KB dump block through (TX is otherwise non-blocking)
      Serial.write((const uint8_t *)"FBSTART", 7);
      Serial.write((const uint8_t *)g_fb, SIZE * SIZE * 2);
      Serial.write((const uint8_t *)"FBEND", 5);
      Serial.flush();
      Serial.setTxTimeoutMs(0);
    }
  }

  // BOOT key short-press -> cycle language EN/中文/Deutsch (works on every page AND in the portal).
  static uint32_t bootDownAt = 0;
  if (digitalRead(0) == LOW) {
    if (bootDownAt == 0) bootDownAt = now;
  } else {
    if (bootDownAt && now - bootDownAt < 800) {     // short press -> cycle language + toast
      lang = (lang + 1) % 3;
      if (portalActive) set_portal_text();          // portal up: re-render its notice/Cancel text
      else              update_ui();                // normal pages: re-render the page
      lv_obj_set_style_text_font(uiToast, &lv_font_montserrat_28, 0);
      lv_label_set_text(uiToast, pickStr("English", "Chinese", "Deutsch"));
      lv_obj_center(uiToast); lv_obj_clear_flag(uiToast, LV_OBJ_FLAG_HIDDEN);
      toastUntil = now + 1500;
    }
    bootDownAt = 0;
  }

  if (toastUntil && now > toastUntil) { lv_obj_add_flag(uiToast, LV_OBJ_FLAG_HIDDEN); toastUntil = 0; }  // hide toast (also during portal)

  // non-blocking config portal: drive it; auto-exit on connect or 2-min timeout. Skip the rest.
  if (portalActive) {
    wm.process();
    // Cancel = tap ANYWHERE on the portal screen (robust; no fragile button hit-test). The
    // 800ms lock after entering stops the entering tap from instantly cancelling.
    static bool cTouch = false;
    if (touch_pressed()) cTouch = true;
    else if (cTouch) {
      cTouch = false;
      if (now >= g_btnLock) { swallowTap = true; exitPortal(); delay(3); return; }
    }
    if (WiFi.status() == WL_CONNECTED) { exitPortal(); time_sync(); }       // configured a new net
    else if (millis() - portalStart > 120000) { exitPortal(); }             // 2-min timeout
    delay(3);
    return;
  }

  // Touch gestures: horizontal swipe changes page; vertical drag on the info page
  // scrolls its list live (gated by dominant axis so the two never fight). Fire the
  // page change only on a genuine release (no touch for >120ms).
  static bool touching = false;
  static int16_t startX = 0, startY = 0, lastX = 0, lastY = 0;
  static uint32_t releaseAt = 0;
  if (swallowTap) {                 // a portal Cancel just fired — ignore that finger until it lifts,
    if (touch_pressed()) { delay(3); return; }   // else it re-taps "Start Setup" and re-enters the portal
    swallowTap = false; touching = false;
  }
  if (touch_pressed()) {
    if (!touching) { touching = true; startX = lastX = tx[0]; startY = lastY = ty[0]; }
    if (page == 2 && abs(ty[0] - startY) > abs(tx[0] - startX)) { // vertical-dominant -> live scroll
      int dy = ty[0] - lastY;                                     // clamp to content bounds: no overscroll
      int top = lv_obj_get_scroll_top(uiInfoBox);                 // hidden above (dy>0 reveals it)
      int bot = lv_obj_get_scroll_bottom(uiInfoBox);              // hidden below (dy<0 reveals it)
      if (dy > top) dy = top;
      if (-dy > bot) dy = -bot;
      if (dy) lv_obj_scroll_by(uiInfoBox, 0, dy, LV_ANIM_OFF);
    }
    lastX = tx[0]; lastY = ty[0];
    releaseAt = 0;
  } else if (touching) {
    if (releaseAt == 0) releaseAt = now;
    else if (now - releaseAt > 120) {                     // genuinely released
      int dx = lastX - startX, dy = lastY - startY;
      if (abs(dx) > abs(dy) && abs(dx) >= 55) {
        on_swipe(dx < 0 ? +1 : -1);                       // horizontal swipe -> page
      } else if (page == 3 && abs(dx) < 24 && abs(dy) < 24) {   // tap on setup page -> hit-test button
        lv_area_t a; lv_obj_get_coords(uiSetBtn, &a);
        const int M = 28;   // generous margin (touch panel is mirrored; tolerate small offset)
        bool inBtn = (lastX >= a.x1 - M && lastX <= a.x2 + M && lastY >= a.y1 - M && lastY <= a.y2 + M);
        if (inBtn && now >= g_btnLock) {
          if (g_wifi) wifi_disconnect();   // connected -> disconnect; offline -> open portal
          else        enterPortal(false);
        }
      }
      touching = false;
    }
  }

  // PWR key (AXP2101 PWRKEY) short-press -> cycle page. BOOT key left unassigned.
  // (long-press PWR is still a hardware power-off handled by the AXP2101.)
  static uint32_t lastIrqPoll = 0;
  static uint32_t pwrClickAt = 0;
  static bool pwrPending = false;
  if (pmuOK && now - lastIrqPoll > 40) {
    lastIrqPoll = now;
    pmu.getIrqStatus();
    if (pmu.isPekeyPositiveIrq()) { pwrDownAt = now; pwrHeld = true; }   // PWR pressed
    if (pmu.isPekeyNegativeIrq()) pwrHeld = false;                       // PWR released
    if (pmu.isPekeyShortPressIrq()) {
      pwrHeld = false;   // a completed short-press means PWR was already released — cancel power-off timing
      if (pwrPending && now - pwrClickAt < 700) {   // 2nd click -> timezone (page unchanged)
        tzIndex = (tzIndex + 1) % NTZ; apply_tz(); update_ui();
        char ts[12]; snprintf(ts, sizeof(ts), "UTC%+d", cur_utc_offset());
        lv_obj_set_style_text_font(uiToast, &lv_font_montserrat_28, 0);
        lv_label_set_text(uiToast, ts); lv_obj_center(uiToast);
        lv_obj_clear_flag(uiToast, LV_OBJ_FLAG_HIDDEN);
        toastUntil = now + 2000;
        pwrPending = false;
      } else {
        pwrPending = true; pwrClickAt = now;        // 1st click -> hold ~0.7s for a possible 2nd
      }
    }
    pmu.clearIrqStatus();
  }
  if (pwrPending && now - pwrClickAt > 700) { pwrPending = false; on_tap(); }   // lone click -> switch page
  if (pwrHeld && now - pwrDownAt > 8000) pmu.shutdown();   // PWR held ~8s -> power off

  static bool firstDone = false;
  g_wifi = (WiFi.status() == WL_CONNECTED);
  if (g_wifi && (!firstDone || now - lastFetch > FETCH_INTERVAL_MS)) {
    lastFetch = now; firstDone = true;
    fetch_usage();
    update_ui();
  }

  static uint32_t lastSec = 0;
  if (now - lastSec > 1000) {     // refresh clock / countdown / battery once a second
    lastSec = now;
    read_power();
    update_ui();
  }

  static uint32_t lastHb = 0;
  if (now - lastHb > 5000) {
    lastHb = now;
    Serial.printf("[hb] wifi=%d heap=%u valid=%d bat=%d chg=%d mv=%u t=%ld\n",
                  WiFi.status(), ESP.getFreeHeap(), snap.valid, batPct,
                  charging, batMv, (long)time(nullptr));
  }
  delay(3);
}
