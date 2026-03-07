/*
 * WyTerminal — LilyGO T-Display S3 AMOLED (1.91" RM67162)
 *
 * Full Linux terminal over Telegram:
 *  - USB HID keyboard injection
 *  - Telegram bot: /shell /screenshot /key /run /type /upload /sysinfo /ps
 *  - AMOLED scrolling terminal log
 *  - Screenshot preview: daemon sends scaled JPEG → rendered inline on AMOLED
 *
 * Board: LilyGo T-Display-S3 | USB Mode: USB-OTG (TinyUSB) | CDC On Boot: Enabled
 * Libraries: Arduino_GFX (github.com/moononournation/Arduino_GFX), ArduinoJson, TJpg_Decoder
 *
 * Serial protocol (115200):
 *   ESP32→daemon: CMD:<command>\n
 *   daemon→ESP32: OUT:<text>\n | ERR:<text>\n | IMG:<len>\n<jpeg bytes>
 */

#include "USB.h"
#include "USBHIDKeyboard.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Arduino_GFX_Library.h>
#include <TJpg_Decoder.h>

// ── Config ────────────────────────────────────────────────────────────
#define WIFI_SSID        "D-Link the router"
#define WIFI_PASSWORD    "Ajeip853jw5590!"
#define BOT_TOKEN        "8688942400:AAFZKipOJnzroUWAea-zZuhZbLbRTiAluLM"
#define ALLOWED_CHAT_ID  1790655432LL

// ── Display pins ──────────────────────────────────────────────────────
#define LCD_CS   6
#define LCD_SCK  47
#define LCD_D0   18
#define LCD_D1   7
#define LCD_D2   48
#define LCD_D3   5
#define LCD_RST  17
#define LCD_PWR  38

// ── Screen layout ─────────────────────────────────────────────────────
#define SCREEN_W  240
#define SCREEN_H  536
#define HEADER_H   26
#define FOOTER_H   20
#define TERM_Y    (HEADER_H + 2)
#define TERM_H    (SCREEN_H - HEADER_H - FOOTER_H - 4)
#define FONT_H     12
#define FONT_W      6
#define TERM_LINES (TERM_H / FONT_H)
#define TERM_COLS  (SCREEN_W / FONT_W)

// ── Colours ───────────────────────────────────────────────────────────
#define C_BG      0x0000
#define C_GREEN   0x07E0
#define C_RED     0xF800
#define C_YELLOW  0xFFE0
#define C_CYAN    0x07FF
#define C_GREY    0x4208
#define C_DKGREEN 0x0200
#define C_DKBLUE  0x000A

// ── Display object ────────────────────────────────────────────────────
Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCK, LCD_D0, LCD_D1, LCD_D2, LCD_D3);
Arduino_GFX *gfx = new Arduino_RM67162(bus, LCD_RST, 0);

USBHIDKeyboard Keyboard;
WiFiClientSecure tls_client;

static long     s_offset   = 0;
static uint32_t s_start_ms = 0;
static char     s_last_text[512] = "";
static bool     s_wifi_ok  = false;

// ── Terminal ring buffer ──────────────────────────────────────────────
struct Line { char text[TERM_COLS + 1]; uint16_t col; bool is_img; int img_h; };
static Line s_buf[TERM_LINES];
static int  s_count = 0;

// ── JPEG → AMOLED via TJpg_Decoder ───────────────────────────────────
static int  s_jpg_draw_y = 0;  // top-left Y for current decode

bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
    if (y >= SCREEN_H) return false;
    gfx->draw16bitRGBBitmap(x, s_jpg_draw_y + y, bitmap, w, h);
    return true;
}

// ── Brightness ────────────────────────────────────────────────────────
void set_brightness(uint8_t v) {
    bus->beginWrite(); bus->writeC8D8(0x51, v); bus->endWrite();
}

// ── Terminal ──────────────────────────────────────────────────────────
void term_redraw() {
    gfx->fillRect(0, TERM_Y, SCREEN_W, TERM_H, C_BG);
    int start = (s_count > TERM_LINES) ? s_count - TERM_LINES : 0;
    for (int i = start; i < s_count; i++) {
        int y = TERM_Y + (i - start) * FONT_H;
        gfx->setTextColor(s_buf[i].col);
        gfx->setCursor(2, y);
        gfx->print(s_buf[i].text);
    }
}

void term_push(const char *text, uint16_t col) {
    if (s_count < TERM_LINES) {
        strncpy(s_buf[s_count].text, text, TERM_COLS);
        s_buf[s_count].col = col;
        s_buf[s_count].is_img = false;
        s_count++;
    } else {
        memmove(s_buf, s_buf + 1, sizeof(Line) * (TERM_LINES - 1));
        strncpy(s_buf[TERM_LINES-1].text, text, TERM_COLS);
        s_buf[TERM_LINES-1].col = col;
        s_buf[TERM_LINES-1].is_img = false;
    }
    term_redraw();
}

void term_cmd(const char *s)  { char b[42]; snprintf(b,42,"> %.39s",s); term_push(b, C_CYAN);   }
void term_ok(const char *s)   { char b[42]; snprintf(b,42,"  %.39s",s); term_push(b, C_GREEN);  }
void term_err(const char *s)  { char b[42]; snprintf(b,42,"  %.39s",s); term_push(b, C_RED);    }
void term_info(const char *s) { char b[42]; snprintf(b,42,"  %.39s",s); term_push(b, C_GREY);   }
void term_head(const char *s) { char b[42]; snprintf(b,42,"%.41s",s);   term_push(b, C_YELLOW); }

// Draw JPEG inline — push enough placeholder lines to scroll terminal up,
// then decode JPEG into that cleared space
void term_show_jpeg(uint8_t *jpg, size_t len) {
    // Peek image height from JPEG SOF marker
    int img_h = 135;
    for (size_t i = 4; i < len - 9; i++) {
        if (jpg[i] == 0xFF && (jpg[i+1] == 0xC0 || jpg[i+1] == 0xC2)) {
            img_h = (jpg[i+5] << 8) | jpg[i+6];
            break;
        }
    }
    img_h = min(img_h, TERM_H - FONT_H);
    int lines_needed = (img_h + FONT_H - 1) / FONT_H;

    // Add placeholder lines to scroll buffer
    char label[TERM_COLS+1];
    snprintf(label, sizeof(label), "[img %dx%d]", SCREEN_W, img_h);
    term_push(label, C_GREY);
    for (int i = 1; i < lines_needed; i++) term_push("", C_BG);

    // Find Y coordinate where those lines land on screen
    int start = (s_count > TERM_LINES) ? s_count - TERM_LINES : 0;
    int img_line = (s_count - lines_needed) - start;
    s_jpg_draw_y = TERM_Y + img_line * FONT_H;

    if (s_jpg_draw_y >= TERM_Y) {
        // Clear the image area first
        gfx->fillRect(0, s_jpg_draw_y, SCREEN_W, img_h, C_BG);
        // Decode and draw
        TJpgDec.setCallback(tft_output);
        TJpgDec.setJpgScale(1);
        TJpgDec.drawJpg(0, 0, jpg, len);  // xy offset applied via s_jpg_draw_y
    }
}

// ── Header / footer ───────────────────────────────────────────────────
void draw_header() {
    gfx->fillRect(0, 0, SCREEN_W, HEADER_H, C_DKGREEN);
    gfx->setTextColor(C_GREEN); gfx->setTextSize(1);
    gfx->setCursor(4, 8); gfx->print("WyTerminal");
    gfx->setCursor(SCREEN_W - 30, 8);
    gfx->setTextColor(s_wifi_ok ? C_GREEN : C_RED);
    gfx->print(s_wifi_ok ? " LIVE" : " WAIT");
}

void draw_footer() {
    int y = SCREEN_H - FOOTER_H;
    gfx->fillRect(0, y, SCREEN_W, FOOTER_H, C_DKBLUE);
    gfx->setTextColor(C_GREY); gfx->setTextSize(1);
    gfx->setCursor(2, y + 4);
    uint32_t up = (millis() - s_start_ms) / 1000;
    char buf[42];
    if (s_wifi_ok)
        snprintf(buf, sizeof(buf), "%s %ddBm %lus",
            WiFi.localIP().toString().c_str(), WiFi.RSSI(), (unsigned long)up);
    else
        snprintf(buf, sizeof(buf), "Connecting... %lus", (unsigned long)up);
    gfx->print(buf);
}

// ── Telegram ──────────────────────────────────────────────────────────
String tg_post(const char *method, const String &body) {
    if (!tls_client.connect("api.telegram.org", 443)) return "";
    String path = String("/bot") + BOT_TOKEN + "/" + method;
    tls_client.println("POST " + path + " HTTP/1.1");
    tls_client.println("Host: api.telegram.org");
    tls_client.println("Content-Type: application/json");
    tls_client.println("Content-Length: " + String(body.length()));
    tls_client.println("Connection: close");
    tls_client.println();
    tls_client.print(body);
    String resp; uint32_t t = millis();
    while (tls_client.connected() && millis() - t < 6000)
        while (tls_client.available()) resp += (char)tls_client.read();
    tls_client.stop();
    int idx = resp.indexOf("\r\n\r\n");
    return (idx >= 0) ? resp.substring(idx + 4) : "";
}

void tg_send(long long chat_id, const char *text) {
    StaticJsonDocument<512> doc;
    doc["chat_id"] = chat_id; doc["text"] = text;
    String body; serializeJson(doc, body);
    tg_post("sendMessage", body);
}

// ── HID ───────────────────────────────────────────────────────────────
void hid_type(const char *text, bool enter) {
    Keyboard.print(text);
    if (enter) { delay(50); Keyboard.press(KEY_RETURN); delay(50); Keyboard.releaseAll(); }
}

// ── Serial → daemon pipeline ─────────────────────────────────────────
static uint8_t s_jpg_buf[65536];
static size_t  s_jpg_expected = 0;
static size_t  s_jpg_received = 0;
static bool    s_jpg_mode = false;

void check_serial() {
    while (Serial0.available()) {
        if (s_jpg_mode) {
            int b = Serial0.read();
            if (s_jpg_received < sizeof(s_jpg_buf))
                s_jpg_buf[s_jpg_received++] = (uint8_t)b;
            if (s_jpg_received >= s_jpg_expected) {
                term_show_jpeg(s_jpg_buf, s_jpg_received);
                s_jpg_mode = false; s_jpg_expected = 0; s_jpg_received = 0;
            }
        } else {
            String line = Serial0.readStringUntil('\n'); line.trim();
            if (!line.length()) continue;
            if      (line.startsWith("OUT:")) term_ok(line.substring(4).c_str());
            else if (line.startsWith("ERR:")) term_err(line.substring(4).c_str());
            else if (line.startsWith("IMG:")) {
                s_jpg_expected = line.substring(4).toInt();
                s_jpg_received = 0; s_jpg_mode = true;
            }
        }
    }
}

// ── Command handler ───────────────────────────────────────────────────
void handle_update(JsonObject &upd) {
    if (!upd.containsKey("message")) return;
    JsonObject msg = upd["message"];
    long long chat_id = msg["chat"]["id"];
    if (chat_id != ALLOWED_CHAT_ID) return;
    const char *text = msg["text"] | "";
    if (!text[0]) return;

    String t(text);
    String disp = t.length() > 30 ? t.substring(0, 29) + ">" : t;
    term_cmd(disp.c_str());
    draw_footer();

    // Daemon-handled commands — forward over serial
    if (t.startsWith("/shell ") || t == "/screenshot" ||
        t.startsWith("/upload ") || t == "/clipboard" ||
        t == "/sysinfo" || t == "/ps") {
        Serial0.println("CMD:" + t);
        tg_send(chat_id, "⏳ running...");
        return;
    }

    // Local HID commands
    if (t.startsWith("/run ")) {
        String cmd = t.substring(5);
        hid_type(cmd.c_str(), true);
        tg_send(chat_id, ("▶️ " + cmd).c_str());
        term_ok("sent+Enter");

    } else if (t.startsWith("/type ")) {
        String s = t.substring(6);
        strncpy(s_last_text, s.c_str(), sizeof(s_last_text)-1);
        hid_type(s.c_str(), false); tg_send(chat_id, "⌨️ typed"); term_ok("typed");

    } else if (t == "/enter") {
        Keyboard.press(KEY_RETURN); delay(50); Keyboard.releaseAll();
        tg_send(chat_id, "↵"); term_ok("Enter");

    } else if (t == "/paste") {
        if (!s_last_text[0]) { tg_send(chat_id, "❌ nothing"); term_err("nothing"); return; }
        hid_type(s_last_text, false); tg_send(chat_id, "📋 pasted"); term_ok("pasted");

    } else if (t.startsWith("/key ")) {
        String combo = t.substring(5); combo.toLowerCase();
        bool ctrl  = combo.indexOf("ctrl")  >= 0;
        bool alt   = combo.indexOf("alt")   >= 0;
        bool shift = combo.indexOf("shift") >= 0;
        bool sup   = combo.indexOf("super") >= 0 || combo.indexOf("win") >= 0;
        int lp = combo.lastIndexOf("+");
        String ks = (lp >= 0) ? combo.substring(lp+1) : combo; ks.trim();
        uint8_t key = 0;
        if      (ks=="t")    key='t'; else if (ks=="c")  key='c';
        else if (ks=="v")    key='v'; else if (ks=="z")  key='z';
        else if (ks=="a")    key='a'; else if (ks=="x")  key='x';
        else if (ks=="f4")   key=KEY_F4; else if (ks=="f5") key=KEY_F5;
        else if (ks=="tab")  key=KEY_TAB; else if (ks=="esc") key=KEY_ESC;
        else if (ks=="space")key=' ';
        else if (ks=="up")   key=KEY_UP_ARROW;
        else if (ks=="down") key=KEY_DOWN_ARROW;
        else if (ks=="left") key=KEY_LEFT_ARROW;
        else if (ks=="right")key=KEY_RIGHT_ARROW;
        else if (ks.length()==1) key=(uint8_t)ks[0];
        if (key) {
            if (ctrl)  Keyboard.press(KEY_LEFT_CTRL);
            if (alt)   Keyboard.press(KEY_LEFT_ALT);
            if (shift) Keyboard.press(KEY_LEFT_SHIFT);
            if (sup)   Keyboard.press(KEY_LEFT_GUI);
            Keyboard.press(key); delay(100); Keyboard.releaseAll();
            tg_send(chat_id, ("⌨️ " + combo).c_str());
            term_ok(combo.c_str());
        } else { tg_send(chat_id, "❓ unknown key"); term_err("unknown key"); }

    } else if (t == "/clear") {
        s_count = 0; gfx->fillRect(0, TERM_Y, SCREEN_W, TERM_H, C_BG);
        tg_send(chat_id, "🧹 cleared");

    } else if (t == "/status") {
        uint32_t up = (millis() - s_start_ms) / 1000;
        char buf[128];
        snprintf(buf, sizeof(buf), "✅ WyTerminal\nIP: %s\nRSSI: %ddBm\nUptime: %lus",
            WiFi.localIP().toString().c_str(), WiFi.RSSI(), (unsigned long)up);
        tg_send(chat_id, buf); term_info("status sent");

    } else if (t == "/help") {
        tg_send(chat_id,
            "WyTerminal — Linux on Telegram\n\n"
            "Shell (needs daemon):\n"
            "/shell <cmd> — run + output\n"
            "/screenshot — screen→TG+AMOLED\n"
            "/upload <path> — file→TG\n"
            "/clipboard — read clipboard\n"
            "/sysinfo — CPU/RAM/disk/IP\n"
            "/ps — top processes\n\n"
            "Keyboard (HID, no daemon):\n"
            "/run <cmd> — type+Enter\n"
            "/type <text> — type only\n"
            "/enter — Enter key\n"
            "/paste — retype last\n"
            "/key ctrl+alt+t etc\n\n"
            "/clear /status /help");
        term_info("help sent");
    } else {
        tg_send(chat_id, "❓ /help for commands");
        term_err("unknown cmd");
    }
}

void poll_telegram() {
    StaticJsonDocument<128> req;
    req["offset"] = s_offset + 1; req["timeout"] = 0; req["limit"] = 5;
    String body; serializeJson(req, body);
    String resp = tg_post("getUpdates", body);
    if (!resp.length()) return;
    DynamicJsonDocument doc(8192);
    if (deserializeJson(doc, resp)) return;
    if (!doc["ok"]) return;
    for (JsonObject upd : doc["result"].as<JsonArray>()) {
        long uid = upd["update_id"];
        if (uid > s_offset) s_offset = uid;
        handle_update(upd);
    }
}

// ── Setup ─────────────────────────────────────────────────────────────
void setup() {
    Serial0.begin(115200); // UART (ACM0 on host)
    Serial.begin(115200);   // USB CDC (debug)
    pinMode(LCD_PWR, OUTPUT); digitalWrite(LCD_PWR, HIGH); delay(50);

    gfx->begin();
    set_brightness(200);
    gfx->fillScreen(C_BG);
    gfx->setTextSize(1); gfx->setTextWrap(false);

    TJpgDec.setCallback(tft_output);
    TJpgDec.setJpgScale(1);

    draw_header(); draw_footer();
    term_head("WyTerminal v1.0");
    term_info("by Wyltek Industries");
    term_info("─────────────────────");

    USB.begin(); Keyboard.begin(); delay(200);
    term_ok("USB HID ready");

    tls_client.setInsecure();
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    term_info("WiFi connecting...");

    uint32_t t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 30000) delay(500);
    s_start_ms = millis();

    if (WiFi.status() == WL_CONNECTED) {
        s_wifi_ok = true; draw_header();
        char buf[40]; snprintf(buf, sizeof(buf), "%s", WiFi.localIP().toString().c_str());
        term_ok(buf);
        term_ok("Ready — /help for commands");
    } else {
        term_err("WiFi failed!");
    }
    draw_footer();
}

// ── Loop ──────────────────────────────────────────────────────────────
static uint32_t s_last_footer = 0;
void loop() {
    check_serial();
    if (s_wifi_ok) {
        if (WiFi.status() != WL_CONNECTED) { s_wifi_ok = false; draw_header(); WiFi.reconnect(); }
        poll_telegram();
    } else if (WiFi.status() == WL_CONNECTED) { s_wifi_ok = true; draw_header(); }
    if (millis() - s_last_footer > 5000) { draw_footer(); s_last_footer = millis(); }
    delay(1000);
}
