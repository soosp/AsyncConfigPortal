// CustomPage.ino
//
// The canonical "add your own page" example. Minimal shows the bare server and
// NetConfig shows the ready-made network component; neither shows how an
// application composes a page of its own. This one does, on purpose with no
// hardware attached, so it runs on any ESP32.
//
// Networking is kept deliberately plain: it joins one Wi-Fi network with the
// fixed credentials near the top of the file, just enough to reach the page.
// Wi-Fi is on virtually every ESP32 (a few P4 variants aside). Making the
// network itself configurable from the browser — addressing, hostname,
// fallback, Wi-Fi power — is a separate and more elegant job, and exactly what
// the optional NetConfigComponent does (see the NetConfig example), at the cost
// of the NetworkProfile / NetworkManager dependencies. That is not the point
// here; composing your own page is.
//
// It drives a "widget": a virtual output that toggles on an interval. All of
// its settings live on a /widget page built from the shared field builders,
// and every composition hook is exercised once:
//
//   addPage()          the page shell (HEADER + body + FOOTER, closed by hand)
//   addJsonEndpoint()  /widgetdata for the form, /widgetlive polled for status
//   addPostHandler()   /widgetsave, with server-side validation + NVS persist
//   addBackupSection() a Widget section on the built-in Backup page ...
//   addUploadHandler() ... backed by /widgetbackup (download) + /widgetrestore
//   addResetHandler()  clears the widget's NVS namespace on factory reset
//
// The GPIO is chosen from a dropdown whose options come from ONE array in this
// sketch (GPIO_CHOICES). That single source feeds the <select> (via /widgetdata)
// and the save handler re-validates against it — the dropdown constrains an
// honest browser, but the server is the authority, because a client can POST
// any value it likes. Change the pin from the web page; no reflash needed.
//
// Things worth knowing, made explicit because they trip people up:
//   * CONFIG_PORTAL_FOOTER does NOT close <body>/</html>; the page does that
//     itself in its last fragment.
//   * Buffer sizes follow the _LEN / _SIZE = _LEN + 1 convention.
//   * No Arduino String in application code; Preferences is read through its
//     char-buffer overload.
//   * A factory reset with no reset handler still clears credentials and
//     reboots — a handler only adds this component's own state to the wipe.
//
// build_opt.h next to this sketch carries the firmware-marker identity
// (FIRMWARE_PROJECT / FIRMWARE_BOARD / FIRMWARE_VERSION). The Arduino IDE
// compiles library sources as separate translation units, so a #define in the
// sketch never reaches them; build_opt.h is passed to all of them. Its contents
// go straight onto the compiler command line, so it holds only flags and may
// NOT contain comments. (PlatformIO: put the same values in build_flags.)
//
// On ESP8266 that file is ignored — the core reads CustomPage.ino.globals.h
// instead, with the flags inside a `/* @create-file:build.opt@ ... */` block. It
// is not shipped here because the IDE hides an example folder containing one.
// The two cores reading different files is also what lets a single sketch carry
// different flags per platform.
//
// ESP8266 additionally needs a flash layout with a filesystem: the credentials
// and this widget's config go through Preferences, which is backed by LittleFS
// there, so a layout without an FS saves nothing.
//
// Default credentials on first boot: admin / admin.

#include <Arduino.h>
#ifdef ARDUINO_ARCH_ESP32
#include <WiFi.h>
#elif defined(ARDUINO_ARCH_ESP8266)
#include <ESP8266WiFi.h>
#else
#error "Unsupported architecture".
#endif
#include <Preferences.h>
#include <atomic>

// Library headers use <> here: an example shows how a sketch outside the
// library includes it, and a sketch reaches its dependencies through the
// library search path, not by relative path.
#include <AsyncConfigPortal.h>
#include <ConfigWebPages.h>   // CONFIG_PORTAL_HEADER / CONFIG_PORTAL_FOOTER
#include <HttpDigestAuth.h>
#include <WebFormUtils.h>     // postVal / postHas
#include <JsonReadUtils.h>    // jsonRoot / jsonValidate / jsonVal / jsonNum

// =============================================================================
// Wi-Fi credentials  —  SET THESE FOR YOUR NETWORK
// =============================================================================
static const char* WIFI_SSID = "your-ssid";
static const char* WIFI_PASS = "your-password";

// -----------------------------------------------------------------------------
// Objects
// -----------------------------------------------------------------------------

AsyncConfigPortal web(80);
HttpDigestAuth    auth;
Preferences       prefs;

static const char* NVS_NS = "widget";

// The pins the dropdown offers. This is the single source of truth: /widgetdata
// serializes it for the <select>, and the save/restore handlers reject anything
// not in it. Edit here to change the choices.
static constexpr uint8_t GPIO_CHOICES[] = { 2, 4, 7, 11 };

static bool gpioAllowed(uint8_t g) {
    for (uint8_t c : GPIO_CHOICES) if (c == g) return true;
    return false;
}

// -----------------------------------------------------------------------------
// Configuration
// -----------------------------------------------------------------------------

static constexpr size_t LABEL_LEN  = 31;
static constexpr size_t LABEL_SIZE = LABEL_LEN + 1;

static constexpr uint16_t BLINK_MIN = 50;
static constexpr uint16_t BLINK_MAX = 10000;

struct WidgetConfig {
    char     label[LABEL_SIZE] = "widget";
    uint16_t blinkMs           = 500;
    bool     enabled           = false;
    uint8_t  gpio              = GPIO_CHOICES[0];
};

// Written from the async server task (save/restore), read from loop(). A small
// POD read torn across tasks is the one rough edge kept for brevity; a real
// application would apply changes from loop() behind a flag, as NetConfig does
// for its restart. The live counters below are atomic, which is the part that
// actually races.
static WidgetConfig g_cfg;

// The counters are shared between the async server task and loop(), so on ESP32
// they are atomic. ESP8266 has no preemption — the async callbacks run in the SYS
// context, which only gets the CPU once the loop() task yields — so a plain
// variable is already consistent there. That is not just a simplification: the
// Xtensa LX106 has no atomic read-modify-write instruction, so an atomic counter
// compiles to a libatomic call (__atomic_fetch_add_4) that the ESP8266 core does
// not link, and the sketch fails at link time.
#if defined(ARDUINO_ARCH_ESP32)
template <typename T> using Shared = std::atomic<T>;
#else
template <typename T>
class Shared {                      // same surface, no atomics needed
public:
    Shared(T v = T{}) : _v(v) {}
    T    load()  const   { return _v; }
    void store(T v)      { _v = v; }
    T    fetch_add(T d)  { T old = _v; _v = old + d; return old; }
private:
    T _v;
};
#endif

static Shared<uint32_t> g_toggles{0};
static Shared<bool>     g_output{false};

// Reject anything that would need escaping when echoed into JSON, so the
// endpoints can emit the label verbatim. Non-empty, printable, no quotes.
static bool labelClean(const char* s) {
    if (!s || !s[0]) return false;
    for (const char* p = s; *p; ++p) {
        char c = *p;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

static void configLoad() {
    prefs.begin(NVS_NS, /*readOnly=*/true);
    if (prefs.isKey("label"))
        prefs.getString("label", g_cfg.label, sizeof(g_cfg.label));
    g_cfg.blinkMs = prefs.getUShort("blink", g_cfg.blinkMs);
    g_cfg.enabled = prefs.getBool  ("en",    g_cfg.enabled);
    g_cfg.gpio    = prefs.getUChar ("gpio",  g_cfg.gpio);
    prefs.end();
    if (!gpioAllowed(g_cfg.gpio)) g_cfg.gpio = GPIO_CHOICES[0];
}

static void configSave() {
    prefs.begin(NVS_NS, /*readOnly=*/false);
    prefs.putString("label", g_cfg.label);
    prefs.putUShort("blink", g_cfg.blinkMs);
    prefs.putBool  ("en",    g_cfg.enabled);
    prefs.putUChar ("gpio",  g_cfg.gpio);
    prefs.end();
}

// -----------------------------------------------------------------------------
// The /widget page
// -----------------------------------------------------------------------------
//
// A full page assembled from the header/footer macros: the JS is defined first
// (so its functions exist before HEADER's menu wiring runs), then HEADER, then
// the body, then FOOTER, then the page closes <body>/</html> itself.
//
// build() renders the form from /widgetdata using the shared builders now in
// common.js: ifRow (text), numRow (number) and selectRow (dropdown). The
// checkbox is inline because a checkbox is a single tag. attachValidation()
// wires the built-in checks and also disables Save while the device is
// unreachable — the watchdog fed by getJSON() drives that for free.

static const char WIDGET_HTML[] PROGMEM =
R"HTML(<!DOCTYPE html><html lang="en"><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1.0">
<link rel="stylesheet" href="/css"><script src="/common.js"></script><script src="/fields.js"></script>
</head><body onload="init()">
<script>
function build(c){
 var h=ifRow('','Label','label',c.label,'my device',{title:'Shown on the device',req:true})
  +numRow('','Blink interval (ms)','blink',c.blink,50,10000,10,'How fast the output toggles')
  +selectRow('','Output GPIO','gpio',c.gpio,c.choices,{title:'Pin driven when enabled'})
  +'<div class="row"><span class="label">Enabled:</span><span class="value">'
  +'<input type="checkbox" id="enabled" name="enabled" value="1"'+(c.enabled?' checked':'')+'></span></div>';
 ID('fields').innerHTML=h;
 markRequired('fields');
 attachValidation('fields','saveBtn');
}
function live(o){
 ID('l_up').innerHTML=o.uptime_s;
 ID('l_heap').innerHTML=o.heap;
 ID('l_count').innerHTML=o.count;
 ID('l_on').innerHTML=o.on?'on':'off';
}
function poll(){getJSON('/widgetlive',live);}
function init(){initCommon();getJSON('/widgetdata',build);poll();setInterval(poll,2000);}
</script>)HTML"
CONFIG_PORTAL_HEADER
R"HTML(
<div class="content">
 <div class="group"><div class="group-title">Live</div>
  <div class="row"><span class="label">Uptime (s):</span><span class="value" id="l_up">-</span></div>
  <div class="row"><span class="label">Free heap:</span><span class="value" id="l_heap">-</span></div>
  <div class="row"><span class="label">Toggles:</span><span class="value" id="l_count">-</span></div>
  <div class="row"><span class="label">Output:</span><span class="value" id="l_on">-</span></div>
 </div>
 <form method="post" action="/widgetsave">
  <div class="group"><div class="group-title">Settings</div>
   <div id="fields"></div>
  </div>
  <div class="row"><span class="label">&nbsp;</span><span class="value">
   <button type="submit" id="saveBtn">Save</button></span></div>
 </form>
</div>)HTML"
CONFIG_PORTAL_FOOTER
R"HTML(
</body></html>
)HTML";

// The "saved" reply reuses the same centered alert box the built-in Reboot
// page uses — its .alert-* classes come from the shared /css — instead of a
// bare line of text. The built-in CONFIG_PORTAL_SAVED_HTML lives in the network
// module, so a standalone page supplies its own.
static const char SAVED_HTML[] PROGMEM =
R"HTML(<!DOCTYPE html><html lang="en"><head><meta charset="UTF-8">
<meta http-equiv="refresh" content="2; url='/widget'"><link rel="stylesheet" href="/css">
<title>Saved</title></head><body>
<div class="alert-container"><div class="alert-backdrop"></div>
<div class="alert-content">Saved. Returning to the widget page...</div></div></body></html>
)HTML";

// -----------------------------------------------------------------------------
// Restore upload accumulation
// -----------------------------------------------------------------------------
//
// The document is tiny, so a fixed buffer is enough; overflow is rejected
// rather than truncated. This single-buffer form assumes one restore at a time
// (fine for a single admin); NetConfig shows the concurrency-hardened version.

static char   g_upBuf[256];
static size_t g_upLen = 0;
static bool   g_upOver = false;

// -----------------------------------------------------------------------------
// Route registration
// -----------------------------------------------------------------------------

static void registerRoutes() {
    using Auth = AsyncConfigPortal::AuthLevel;

    // Form data: current values plus the GPIO choices, straight from the one
    // array. The label is validated on the way in, so it is safe to emit raw.
    web.addJsonEndpoint("/widgetdata", [](char* b, size_t n) {
        int p = snprintf(b, n,
            "{\"label\":\"%s\",\"blink\":%u,\"enabled\":%s,\"gpio\":%u,\"choices\":[",
            g_cfg.label, g_cfg.blinkMs, g_cfg.enabled ? "true" : "false", g_cfg.gpio);
        for (size_t i = 0; i < (sizeof(GPIO_CHOICES) / sizeof(GPIO_CHOICES[0])); ++i) {
            if (p < 0 || (size_t)p >= n) return false;
            p += snprintf(b + p, n - p, "%s%u", i ? "," : "", GPIO_CHOICES[i]);
        }
        if (p < 0 || (size_t)p >= n) return false;
        p += snprintf(b + p, n - p, "]}");
        return p > 0 && (size_t)p < n;
    }, Auth::Required);

    // Live status, polled by the page. No auth: it is only uptime and heap, and
    // the poll doubles as the reachability heartbeat for the watchdog.
    web.addJsonEndpoint("/widgetlive", [](char* b, size_t n) {
        int p = snprintf(b, n,
            "{\"uptime_s\":%lu,\"heap\":%u,\"count\":%lu,\"on\":%s}",
            (unsigned long)(millis() / 1000), (unsigned)ESP.getFreeHeap(),
            (unsigned long)g_toggles.load(), g_output.load() ? "true" : "false");
        return p > 0 && (size_t)p < n;
    }, Auth::None);

    // Save. Validate everything into a copy, then commit — read-before-write, so
    // a rejected field leaves the live config untouched.
    web.addPostHandler("/widgetsave", [](AsyncWebServerRequest* req) {
        WidgetConfig c = g_cfg;
        bool ok = true;

        char label[LABEL_SIZE];
        int ln = postVal(req, "label", label, sizeof(label));
        if (ln <= 0 || (size_t)ln > LABEL_LEN || !labelClean(label)) ok = false;

        char nb[8];
        int bn = postVal(req, "blink", nb, sizeof(nb));
        long blink = (bn > 0) ? strtol(nb, nullptr, 10) : -1;
        if (blink < BLINK_MIN || blink > BLINK_MAX) ok = false;

        char gb[8];
        int gn = postVal(req, "gpio", gb, sizeof(gb));
        long gpio = (gn > 0) ? strtol(gb, nullptr, 10) : -1;
        if (gpio < 0 || !gpioAllowed((uint8_t)gpio)) ok = false;   // authority, not the dropdown

        bool enabled = postHas(req, "enabled");   // checkbox: present only when ticked

        if (!ok) {
            req->send(400, "text/plain", "Invalid settings. Nothing was changed.");
            return;
        }
        snprintf(c.label, sizeof(c.label), "%s", label);
        c.blinkMs = (uint16_t)blink;
        c.gpio    = (uint8_t)gpio;
        c.enabled = enabled;
        g_cfg = c;
        configSave();
        req->send(200, "text/html", SAVED_HTML);
    }, Auth::Required);

    // Backup download: the config as a JSON attachment. The Download button on
    // the Backup page POSTs here.
    web.addPostHandler("/widgetbackup", [](AsyncWebServerRequest* req) {
        char buf[192];
        snprintf(buf, sizeof(buf),
            "{\"label\":\"%s\",\"blink\":%u,\"enabled\":%s,\"gpio\":%u}",
            g_cfg.label, g_cfg.blinkMs, g_cfg.enabled ? "true" : "false", g_cfg.gpio);
        AsyncWebServerResponse* r = req->beginResponse(200, "application/json", buf);
        r->addHeader("Content-Disposition", "attachment; filename=\"widget-backup.json\"");
        req->send(r);
    }, Auth::Required);

    // Restore upload: accumulate, validate the whole document, then apply. A
    // mangled file changes nothing rather than applying the part it could read.
    web.addUploadHandler("/widgetrestore",
        [](AsyncWebServerRequest* req) {                       // onComplete
            if (g_upOver) {
                req->send(400, "text/plain", "File too large. Nothing was changed.");
                g_upLen = 0; g_upOver = false; return;
            }
            if (g_upLen == 0) {
                req->send(400, "text/plain", "No file was uploaded.");
                return;
            }
            g_upBuf[g_upLen] = '\0';
            JsonSpan doc = jsonRoot(g_upBuf, g_upLen);
            g_upLen = 0;

            if (!jsonValidate(doc)) {
                req->send(400, "text/plain", "Not a valid backup. Nothing was changed.");
                return;
            }
            WidgetConfig c = g_cfg;
            bool ok = true;

            char label[LABEL_SIZE];
            int ln = jsonVal(doc, "label", label, sizeof(label));
            if (ln <= 0 || (size_t)ln > LABEL_LEN || !labelClean(label)) ok = false;

            uint16_t blink = 0;
            if (jsonNum(doc, "blink", blink) != 0 || blink < BLINK_MIN || blink > BLINK_MAX) ok = false;

            uint8_t gpio = 0;
            if (jsonNum(doc, "gpio", gpio) != 0 || !gpioAllowed(gpio)) ok = false;

            bool enabled = false;
            jsonNum(doc, "enabled", enabled);   // optional; absent means off

            if (!ok) {
                req->send(400, "text/plain", "Backup values are invalid. Nothing was changed.");
                return;
            }
            snprintf(c.label, sizeof(c.label), "%s", label);
            c.blinkMs = blink;
            c.gpio    = gpio;
            c.enabled = enabled;
            g_cfg = c;
            configSave();
            req->send(200, "text/html", SAVED_HTML);
        },
        [](AsyncWebServerRequest* req, size_t index, uint8_t* data,
           size_t len, bool final) {                            // onChunk
            (void)req; (void)final;
            if (index == 0) { g_upLen = 0; g_upOver = false; }
            if (g_upLen + len < sizeof(g_upBuf)) {
                memcpy(g_upBuf + g_upLen, data, len);
                g_upLen += len;
            } else {
                g_upOver = true;
            }
        },
        Auth::Required);

    // Add the Widget section to the built-in Backup page (Download + Restore).
    // Both paths in one call, so a download-without-restore trap is unreachable.
    web.addBackupSection("Widget", "/widgetbackup", "/widgetrestore");

    // Factory reset erases this component's NVS namespace. Without this handler
    // the reset would still clear credentials and reboot, but leave the widget
    // settings behind.
    web.addResetHandler("widget", []() {
        prefs.begin(NVS_NS, /*readOnly=*/false);
        bool ok = prefs.clear();
        prefs.end();
        return ok;
    });

    // The page itself. Required, because it changes device state.
    web.addPage("/widget", "Widget", WIDGET_HTML,
                Auth::Required, AsyncConfigPortal::MENU_NORMAL);
}

// -----------------------------------------------------------------------------
// Setup / loop
// -----------------------------------------------------------------------------

void setup() {
    Serial.begin(115200);

    // Wi-Fi. Credentials are at the top of the file. The config server needs a
    // network before it starts, so connect first and report the address.
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print(F("wifi: connecting"));
    for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; ++i) {
        delay(250);
        Serial.print('.');
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
        Serial.print(F("wifi: connected, IP "));
        Serial.println(WiFi.localIP());
    } else {
        Serial.println(F("wifi: not connected - check the credentials at the top"));
    }

    if (!auth.restore()) {
        auth.setCredentials("admin", "esp32", "admin");
        auth.save();
    }

    configLoad();
    registerRoutes();

    web.setProject("CustomPage", "0.1.0",
                   "Composing your own config page", "2026", "you");

    web.setLogger([](AsyncConfigPortal::LogLevel lvl, const char* tag, const char* msg) {
        Serial.printf("[%s][%d] %s\n", tag, static_cast<int>(lvl), msg);
    });

    web.begin(auth);
}

void loop() {
    static uint32_t last = 0;

    if (g_cfg.enabled) {
        uint32_t now = millis();
        if (now - last >= g_cfg.blinkMs) {
            last = now;
            bool on = !g_output.load();
            g_output.store(on);
            g_toggles.fetch_add(1);

            // --- Drive a real LED on the chosen pin: uncomment this block. The
            //     pin comes from the Widget page, so it can change at runtime
            //     without a reflash. ---
            // static uint8_t wired = 255;
            // if (wired != g_cfg.gpio) { pinMode(g_cfg.gpio, OUTPUT); wired = g_cfg.gpio; }
            // digitalWrite(g_cfg.gpio, on ? HIGH : LOW);
        }
    } else {
        g_output.store(false);
    }

    delay(5);
}
