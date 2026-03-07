/*
 * WyTerminal v3 — USB Composite: HID Keyboard + NCM Ethernet
 *
 * Board appears as:
 *   - USB HID Keyboard  (/key, /run, /type)
 *   - USB NCM Ethernet  (relay comms — no WiFi needed for shell)
 *
 * USB network: board=192.168.7.2, target=192.168.7.1
 * On plug-in: if relay not found → auto-deploys via HID
 * WiFi: Telegram polling only
 *
 * Board: LilyGO T-Display S3 AMOLED | FQBN: lilygo_t_display_s3
 * Libraries: Arduino_GFX, ArduinoJson, TJpg_Decoder
 */

// ── NOTE: NCM+HID composite requires custom tusb_config ──────────
// Until Arduino core exposes USBNCM class directly, we use a
// workaround: CDC-ECM/RNDIS via the existing TinyUSB integration.
// For now this build targets WiFi relay only (v2 parity) and adds
// the USB-net autodeploy flow. Full NCM composite is a platform-level
// change that needs an IDF component or custom core build.
// See: https://github.com/espressif/arduino-esp32/issues/7501
// ─────────────────────────────────────────────────────────────────

#include "USB.h"
#include "USBHIDKeyboard.h"
#include "usb_ncm.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Arduino_GFX_Library.h>
#include <TJpg_Decoder.h>
#include "secrets.h"

// USB net target (populated once NCM is available; for now unused)
#define USB_NET_TARGET_IP  "192.168.7.1"
#define RELAY_USB_URL      "http://192.168.7.1:7799"

// ── Display pins ──────────────────────────────────────────────────
#define LCD_CS   6
#define LCD_SCK  47
#define LCD_D0   18
#define LCD_D1   7
#define LCD_D2   48
#define LCD_D3   5
#define LCD_RST  17
#define LCD_PWR  38

// ── Screen layout ─────────────────────────────────────────────────
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

#define C_BG      0x0000
#define C_GREEN   0x07E0
#define C_RED     0xF800
#define C_YELLOW  0xFFE0
#define C_CYAN    0x07FF
#define C_GREY    0x4208
#define C_DKGREEN 0x0200
#define C_DKBLUE  0x000A
#define C_ORANGE  0xFD20

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCK, LCD_D0, LCD_D1, LCD_D2, LCD_D3);
Arduino_GFX *gfx = new Arduino_RM67162(bus, LCD_RST, 0);

USBHIDKeyboard Keyboard;
WiFiClientSecure tls_client;

static long     s_offset    = 0;
static uint32_t s_start_ms  = 0;
static bool     s_wifi_ok   = false;
static bool     s_relay_ok  = false;
static bool     s_usb_relay = false;   // relay found via USB net
static char     s_target[32] = "local";
static char     s_last_text[512] = "";

// active relay URL — USB net first, WiFi fallback
static const char *active_relay() {
    if (usb_ncm_connected() && s_usb_relay) return RELAY_USB_URL;
    return RELAY_URL;
}

// ── Terminal ──────────────────────────────────────────────────────
struct Line { char text[TERM_COLS + 1]; uint16_t col; };
static Line s_buf[TERM_LINES];
static int  s_count = 0;
static int  s_jpg_draw_y = 0;

bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bm) {
    if (y >= SCREEN_H) return false;
    gfx->draw16bitRGBBitmap(x, s_jpg_draw_y + y, bm, w, h);
    return true;
}

void set_brightness(uint8_t v) {
    bus->beginWrite(); bus->writeC8D8(0x51, v); bus->endWrite();
}

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
        s_count++;
    } else {
        memmove(s_buf, s_buf + 1, sizeof(Line) * (TERM_LINES - 1));
        strncpy(s_buf[TERM_LINES-1].text, text, TERM_COLS);
        s_buf[TERM_LINES-1].col = col;
    }
    term_redraw();
}

void term_show_jpeg(uint8_t *jpg, size_t len) {
    int img_h = 135;
    for (size_t i = 4; i < len - 9; i++) {
        if (jpg[i] == 0xFF && (jpg[i+1] == 0xC0 || jpg[i+1] == 0xC2)) {
            img_h = (jpg[i+5] << 8) | jpg[i+6]; break;
        }
    }
    img_h = min(img_h, TERM_H - FONT_H);
    int lines_needed = (img_h + FONT_H - 1) / FONT_H;
    char label[TERM_COLS+1];
    snprintf(label, sizeof(label), "[screenshot %dx%d]", SCREEN_W, img_h);
    term_push(label, C_GREY);
    for (int i = 1; i < lines_needed; i++) term_push("", C_BG);
    int start = (s_count > TERM_LINES) ? s_count - TERM_LINES : 0;
    int img_line = (s_count - lines_needed) - start;
    s_jpg_draw_y = TERM_Y + img_line * FONT_H;
    if (s_jpg_draw_y >= TERM_Y) {
        gfx->fillRect(0, s_jpg_draw_y, SCREEN_W, img_h, C_BG);
        TJpgDec.setCallback(tft_output);
        TJpgDec.setJpgScale(1);
        TJpgDec.drawJpg(0, 0, jpg, len);
    }
}

void term_cmd(const char *s)  { char b[42]; snprintf(b,42,"> %.39s",s); term_push(b, C_CYAN);   }
void term_ok(const char *s)   { char b[42]; snprintf(b,42,"  %.39s",s); term_push(b, C_GREEN);  }
void term_err(const char *s)  { char b[42]; snprintf(b,42,"  %.39s",s); term_push(b, C_RED);    }
void term_info(const char *s) { char b[42]; snprintf(b,42,"  %.39s",s); term_push(b, C_GREY);   }
void term_head(const char *s) { char b[42]; snprintf(b,42,"%.41s",s);   term_push(b, C_YELLOW); }

void draw_header() {
    gfx->fillRect(0, 0, SCREEN_W, HEADER_H, C_DKGREEN);
    gfx->setTextColor(C_GREEN); gfx->setTextSize(1);
    gfx->setCursor(4, 8);
    char h[32]; snprintf(h, sizeof(h), "WyTerminal [%s]", s_target);
    gfx->print(h);
    gfx->setCursor(SCREEN_W - 36, 8);
    if (s_relay_ok) {
        gfx->setTextColor(s_usb_relay ? C_ORANGE : C_GREEN);
        gfx->print(s_usb_relay ? " USB" : "WIFI");
    } else {
        gfx->setTextColor(C_RED);
        gfx->print("WAIT");
    }
}

void draw_footer() {
    int y = SCREEN_H - FOOTER_H;
    gfx->fillRect(0, y, SCREEN_W, FOOTER_H, C_DKBLUE);
    gfx->setTextColor(C_GREY); gfx->setTextSize(1);
    gfx->setCursor(2, y + 4);
    uint32_t up = (millis() - s_start_ms) / 1000;
    char buf[42];
    snprintf(buf, sizeof(buf), s_wifi_ok ?
        "%s %lus" : "WiFi... %lus",
        s_wifi_ok ? WiFi.localIP().toString().c_str() : "",
        (unsigned long)up);
    gfx->print(buf);
}

// ── Relay ─────────────────────────────────────────────────────────
bool check_relay(const char *base_url) {
    HTTPClient h;
    h.begin(String(base_url) + "/health");
    h.setTimeout(2000);
    int code = h.GET(); h.end();
    return code == 200;
}

void try_deploy_relay() {
    term_info("typing install cmd...");
    // Focus terminal first
    Keyboard.press(KEY_LEFT_CTRL); Keyboard.press(KEY_LEFT_ALT);
    Keyboard.press('t'); delay(150); Keyboard.releaseAll(); delay(1500);
    // Type install command
    const char *cmd = "curl -fsSL https://wyltekindustries.com/wyterminal/install.sh | bash";
    Keyboard.print(cmd);
    delay(50); Keyboard.press(KEY_RETURN); delay(50); Keyboard.releaseAll();
    term_info("waiting 20s for install...");
    delay(20000);
}

String relay_post_url(const char *base, const char *path, const String &body, int tms) {
    HTTPClient h;
    h.begin(String(base) + path);
    h.addHeader("Content-Type", "application/json");
    h.setTimeout(tms);
    String b2 = body;
    int code = h.POST(b2);
    String r = (code > 0) ? h.getString() : "{\"error\":\"relay unreachable\"}";
    h.end();
    return r;
}

String relay_shell(const char *cmd, int tms = 10000) {
    StaticJsonDocument<256> req;
    req["cmd"] = cmd; req["target"] = s_target;
    String body; serializeJson(req, body);
    // Try USB relay first
    if (s_usb_relay) {
        String r = relay_post_url(RELAY_USB_URL, "/shell", body, tms);
        if (r.indexOf("relay unreachable") < 0) return r;
        s_usb_relay = false; // lost USB relay
    }
    return relay_post_url(RELAY_URL, "/shell", body, tms);
}

// ── Telegram ──────────────────────────────────────────────────────
String tg_post_req(const char *method, const String &body) {
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
    while (tls_client.connected() && millis() - t < 8000)
        while (tls_client.available()) resp += (char)tls_client.read();
    tls_client.stop();
    int idx = resp.indexOf("\r\n\r\n");
    return (idx >= 0) ? resp.substring(idx + 4) : "";
}

void tg_send(long long chat_id, const char *text) {
    StaticJsonDocument<512> doc;
    doc["chat_id"] = chat_id; doc["text"] = text;
    String body; serializeJson(doc, body);
    tg_post_req("sendMessage", body);
}

// ── HID ───────────────────────────────────────────────────────────
void hid_type(const char *text, bool enter) {
    Keyboard.print(text);
    if (enter) { delay(50); Keyboard.press(KEY_RETURN); delay(50); Keyboard.releaseAll(); }
}

// ── Command handler ───────────────────────────────────────────────
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

    if (t.startsWith("/shell ")) {
        String cmd = t.substring(7);
        tg_send(chat_id, "⏳ running...");
        term_info("→ relay...");
        String resp = relay_shell(cmd.c_str());
        DynamicJsonDocument doc(4096);
        if (!deserializeJson(doc, resp)) {
            const char *out = doc["output"] | "";
            int code = doc["exit_code"] | -1;
            const char *err = doc["error"] | "";
            if (strlen(err)) { tg_send(chat_id, (String("❌ ")+err).c_str()); term_err(err); }
            else {
                String reply = (code==0?"✅ ":"❌ ") + cmd + "\n" + String(out).substring(0,3800);
                tg_send(chat_id, reply.c_str());
                String o(out); int nl=0, pos=0;
                while(nl<3 && pos<(int)o.length()) {
                    int end=o.indexOf('\n',pos); if(end<0) end=o.length();
                    String line=o.substring(pos,end); if(line.length()) term_ok(line.c_str());
                    pos=end+1; nl++;
                }
            }
        } else { tg_send(chat_id,"❌ relay error"); term_err("relay error"); }
        return;
    }

    if (t == "/screenshot") {
        tg_send(chat_id, "📸 capturing...");
        term_info("→ screenshot...");
        String resp = relay_shell("screenshot", 30000);
        DynamicJsonDocument doc(65536);
        if (deserializeJson(doc, resp)) { term_err("parse error"); return; }
        const char *err = doc["error"] | "";
        if (strlen(err)) { tg_send(chat_id,(String("❌ ")+err).c_str()); term_err(err); return; }
        term_ok("TG sent");
        const char *b64 = doc["jpeg_b64"] | "";
        if (strlen(b64) > 10) {
            size_t b64_len = strlen(b64);
            uint8_t *jpg = (uint8_t*)malloc((b64_len/4)*3+4);
            if (jpg) {
                static const uint8_t dt[256]={
                    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                    0,0,0,0,0,0,0,0,0,0,0,62,0,0,0,63,52,53,54,55,56,57,58,59,60,61,
                    0,0,0,0,0,0,0,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,
                    20,21,22,23,24,25,0,0,0,0,0,0,26,27,28,29,30,31,32,33,34,35,36,37,
                    38,39,40,41,42,43,44,45,46,47,48,49,50,51};
                size_t i=0,ol=0;
                while(i+3<b64_len){
                    uint8_t a=dt[(uint8_t)b64[i]],b2=dt[(uint8_t)b64[i+1]],
                            c=dt[(uint8_t)b64[i+2]],d=dt[(uint8_t)b64[i+3]];
                    jpg[ol++]=(a<<2)|(b2>>4);
                    if(b64[i+2]!='=') jpg[ol++]=(b2<<4)|(c>>2);
                    if(b64[i+3]!='=') jpg[ol++]=(c<<6)|d;
                    i+=4;
                }
                if(ol>100) term_show_jpeg(jpg,ol); else term_err("jpeg too small");
                free(jpg);
            } else term_err("no mem");
        }
        return;
    }

    if (t.startsWith("/target ")) {
        String tgt = t.substring(8); tgt.trim();
        if (tgt.indexOf("@") >= 0) {
            int sp = tgt.indexOf(" ");
            String alias,host;
            if(sp>0){alias=tgt.substring(0,sp);host=tgt.substring(sp+1);}
            else{alias=tgt;host=tgt;}
            HTTPClient hreg;
            hreg.begin(String(active_relay())+"/target/add");
            hreg.addHeader("Content-Type","application/json");
            StaticJsonDocument<128> reg; reg["alias"]=alias; reg["host"]=host;
            String rb; serializeJson(reg,rb); hreg.POST(rb); hreg.end();
            tgt=alias;
        }
        strncpy(s_target, tgt.c_str(), sizeof(s_target)-1);
        draw_header();
        tg_send(chat_id,(String("🎯 Target: ")+s_target).c_str());
        term_ok(("target: "+tgt).c_str());
        return;
    }

    if (t.startsWith("/pass ")) {
        // Store SSH password for current target — relay verifies immediately
        String pw = t.substring(6); pw.trim();
        StaticJsonDocument<128> pdoc;
        pdoc["target"] = s_target; pdoc["password"] = pw;
        String pbody; serializeJson(pdoc, pbody);
        HTTPClient hpw;
        hpw.begin(String(active_relay())+"/target/password");
        hpw.addHeader("Content-Type","application/json");
        hpw.setTimeout(15000);
        String pb2 = pbody;
        int code = hpw.POST(pb2);
        String resp = (code > 0) ? hpw.getString() : "";
        hpw.end();
        DynamicJsonDocument rdoc(512);
        if (!deserializeJson(rdoc, resp)) {
            bool ok = rdoc["verified"] | false;
            const char *err = rdoc["error"] | "";
            const char *user = rdoc["user"] | "";
            if (ok) {
                tg_send(chat_id, (String("✅ SSH auth OK — logged in as: ") + user).c_str());
                term_ok(("auth ok: "+String(user)).c_str());
            } else {
                tg_send(chat_id, (String("❌ SSH auth failed: ") + err).c_str());
                term_err("auth failed");
            }
        } else {
            tg_send(chat_id, "❌ relay error");
            term_err("relay error");
        }
        return;
    }

    if (t.startsWith("/input ")) {
        HTTPClient hinp;
        hinp.begin(String(active_relay())+"/shell/input");
        hinp.addHeader("Content-Type","application/json");
        StaticJsonDocument<128> inp; inp["value"]=t.substring(7);
        String ib; serializeJson(inp,ib);
        int code=hinp.POST(ib); hinp.end();
        tg_send(chat_id,code==200?"🔑 sent":"❌ no pending input");
        return;
    }

    if (t == "/targets") {
        HTTPClient hlist;
        hlist.begin(String(active_relay())+"/targets");
        int code=hlist.GET();
        String resp=(code>0)?hlist.getString():"{}"; hlist.end();
        DynamicJsonDocument doc(1024);
        String out="📋 Targets:\n";
        if(!deserializeJson(doc,resp))
            for(JsonPair kv:doc.as<JsonObject>())
                out+=String("  ")+kv.key().c_str()+" → "+
                     (kv.value().isNull()?"local":kv.value().as<String>())+"\n";
        out+="\nCurrent: "+String(s_target);
        tg_send(chat_id,out.c_str()); term_info("targets listed");
        return;
    }

    if (t.startsWith("/run ")) {
        hid_type(t.substring(5).c_str(),true);
        tg_send(chat_id,("▶️ "+t.substring(5)).c_str()); term_ok("sent+Enter");
    } else if (t.startsWith("/type ")) {
        String s=t.substring(6); strncpy(s_last_text,s.c_str(),sizeof(s_last_text)-1);
        hid_type(s.c_str(),false); tg_send(chat_id,"⌨️ typed"); term_ok("typed");
    } else if (t=="/enter") {
        Keyboard.press(KEY_RETURN); delay(50); Keyboard.releaseAll();
        tg_send(chat_id,"↵"); term_ok("Enter");
    } else if (t=="/paste") {
        if(!s_last_text[0]){tg_send(chat_id,"❌ nothing");return;}
        hid_type(s_last_text,false); tg_send(chat_id,"📋 pasted"); term_ok("pasted");
    } else if (t.startsWith("/key ")) {
        String combo=t.substring(5); combo.toLowerCase();
        bool ctrl=combo.indexOf("ctrl")>=0,alt=combo.indexOf("alt")>=0;
        bool shift=combo.indexOf("shift")>=0,sup=combo.indexOf("super")>=0||combo.indexOf("win")>=0;
        int lp=combo.lastIndexOf("+");
        String ks=(lp>=0)?combo.substring(lp+1):combo; ks.trim();
        uint8_t key=0;
        if(ks=="t")key='t'; else if(ks=="c")key='c'; else if(ks=="v")key='v';
        else if(ks=="z")key='z'; else if(ks=="a")key='a'; else if(ks=="x")key='x';
        else if(ks=="f4")key=KEY_F4; else if(ks=="f5")key=KEY_F5;
        else if(ks=="tab")key=KEY_TAB; else if(ks=="esc")key=KEY_ESC;
        else if(ks=="space")key=' '; else if(ks=="up")key=KEY_UP_ARROW;
        else if(ks=="down")key=KEY_DOWN_ARROW; else if(ks=="left")key=KEY_LEFT_ARROW;
        else if(ks=="right")key=KEY_RIGHT_ARROW;
        else if(ks.length()==1)key=(uint8_t)ks[0];
        if(key){
            if(ctrl)Keyboard.press(KEY_LEFT_CTRL); if(alt)Keyboard.press(KEY_LEFT_ALT);
            if(shift)Keyboard.press(KEY_LEFT_SHIFT); if(sup)Keyboard.press(KEY_LEFT_GUI);
            Keyboard.press(key); delay(100); Keyboard.releaseAll();
            tg_send(chat_id,("⌨️ "+combo).c_str()); term_ok(combo.c_str());
        } else { tg_send(chat_id,"❓ unknown key"); term_err("unknown key"); }
    } else if (t=="/deploy") {
        tg_send(chat_id,"🚀 deploying relay on target...");
        try_deploy_relay();
        // Check if it came up
        if(check_relay(RELAY_USB_URL)){
            s_usb_relay=true; s_relay_ok=true; draw_header();
            tg_send(chat_id,"✅ USB relay online!");
        } else if(check_relay(RELAY_URL)){
            s_relay_ok=true;
            tg_send(chat_id,"✅ WiFi relay reachable");
        } else {
            tg_send(chat_id,"⏳ still deploying — try /status in 30s");
        }
    } else if (t=="/clear") {
        s_count=0; gfx->fillRect(0,TERM_Y,SCREEN_W,TERM_H,C_BG);
        tg_send(chat_id,"🧹 cleared");
    } else if (t=="/status") {
        uint32_t up=(millis()-s_start_ms)/1000;
        char buf[200];
        snprintf(buf,sizeof(buf),
            "WyTerminal v3\nRelay: %s (%s)\nUSB: %s\nWiFi: %s\nTarget: %s\nUptime: %lus",
            s_relay_ok?"✅":"❌",
            s_usb_relay?"USB":"WiFi",
            s_usb_relay?"up":"down",
            s_wifi_ok?WiFi.localIP().toString().c_str():"down",
            s_target,(unsigned long)up);
        tg_send(chat_id,buf); term_info("status sent");
    } else if (t=="/help") {
        tg_send(chat_id,
            "WyTerminal v3\n\n"
            "Shell (USB relay or WiFi fallback):\n"
            "/shell <cmd>\n"
            "/screenshot\n"
            "/input <text> — sudo password\n\n"
            "Targets:\n"
            "/target user@host\n"
            "/target alias — switch saved\n"
            "/targets — list all\n\n"
            "Keyboard (HID):\n"
            "/run <cmd>+Enter\n"
            "/key ctrl+alt+t\n"
            "/type <text>\n"
            "/enter /paste\n\n"
            "/deploy — install relay on target\n"
            "/clear /status /help");
        term_info("help sent");
    } else {
        tg_send(chat_id,"❓ /help"); term_err("unknown");
    }
}

void poll_telegram() {
    StaticJsonDocument<128> req;
    req["offset"]=s_offset+1; req["timeout"]=0; req["limit"]=5;
    String body; serializeJson(req,body);
    String resp=tg_post_req("getUpdates",body);
    if(!resp.length()) return;
    DynamicJsonDocument doc(8192);
    if(deserializeJson(doc,resp)) return;
    if(!doc["ok"]) return;
    for(JsonObject upd:doc["result"].as<JsonArray>()) {
        long uid=upd["update_id"];
        if(uid>s_offset) s_offset=uid;
        handle_update(upd);
    }
}

// ── Relay discovery on boot ───────────────────────────────────────
void discover_relay() {
    term_info("checking USB relay...");
    if(check_relay(RELAY_USB_URL)) {
        s_usb_relay=true; s_relay_ok=true;
        term_ok("USB relay found!"); draw_header();
        return;
    }
    term_info("checking WiFi relay...");
    if(check_relay(RELAY_URL)) {
        s_relay_ok=true;
        term_ok("WiFi relay found"); draw_header();
        return;
    }
    // No relay — auto deploy via HID if USB link is up
    // (USB NCM not yet available — skip auto-deploy for now)
    term_err("no relay found");
    term_info("/deploy to install");
    draw_header();
}

void setup() {
    Serial.begin(115200);
    pinMode(LCD_PWR, OUTPUT); digitalWrite(LCD_PWR, HIGH); delay(50);
    gfx->begin(); set_brightness(200);
    gfx->fillScreen(C_BG); gfx->setTextSize(1); gfx->setTextWrap(false);
    TJpgDec.setCallback(tft_output); TJpgDec.setJpgScale(1);
    draw_header(); draw_footer();
    term_head("WyTerminal v3");
    term_info("USB Composite Edition");
    term_info("──────────────────────");
    USB.begin(); Keyboard.begin(); delay(200);
    usb_ncm_init();
    term_ok("HID + NCM ready");
    tls_client.setInsecure();
    WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    term_info("WiFi connecting...");
    uint32_t t=millis();
    while(WiFi.status()!=WL_CONNECTED && millis()-t<30000) delay(500);
    s_start_ms=millis();
    if(WiFi.status()==WL_CONNECTED) {
        s_wifi_ok=true;
        char buf[40]; snprintf(buf,sizeof(buf),"%s",WiFi.localIP().toString().c_str());
        term_ok(buf);
        discover_relay();
    } else {
        term_err("WiFi failed!");
    }
    draw_footer();
}

static uint32_t s_last_footer=0;
static uint32_t s_last_relay_check=0;

void loop() {
    if(s_wifi_ok) {
        if(WiFi.status()!=WL_CONNECTED){s_wifi_ok=false;draw_header();WiFi.reconnect();}
        poll_telegram();
        // Periodically re-check relay (USB may come up after boot)
        if(millis()-s_last_relay_check > 15000) {
            s_last_relay_check=millis();
            if(!s_relay_ok || !s_usb_relay) {
                if(check_relay(RELAY_USB_URL)){
                    s_usb_relay=true; s_relay_ok=true; draw_header();
                } else if(!s_relay_ok && check_relay(RELAY_URL)){
                    s_relay_ok=true; draw_header();
                }
            }
        }
    } else if(WiFi.status()==WL_CONNECTED){s_wifi_ok=true;draw_header();discover_relay();}
    if(millis()-s_last_footer>5000){draw_footer();s_last_footer=millis();}
    usb_ncm_poll();
    delay(1000);
}
