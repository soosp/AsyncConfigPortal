/**
 * @file AsyncConfigPortal.cpp
 * @brief Implementation of the AsyncConfigPortal base class.
 */

#include <atomic>

#include "AsyncConfigPortal.h"
#include "ConfigWebPages.h"    // PROGMEM HTML/CSS for the built-in pages
#include "WebFormUtils.h"      // String-free POST helpers
#include "FirmwareMarker.h"    // OTA firmware identity validation

#if defined(ARDUINO_ARCH_ESP32)
#  include <Update.h>
#  include <esp_system.h>
#  define ACP_UPDATE_ERR Update.errorString()             // const char*
#elif defined(ARDUINO_ARCH_ESP8266)
#  include <Updater.h>       // the core's OTA updater (Update global)
#  define ACP_UPDATE_ERR Update.getErrorString().c_str()  // used inline only:
                                                          // the temporary String
                                                          // lives through the call
#endif
#include <stdarg.h>

// -----------------------------------------------------------------------------
// Construction
// -----------------------------------------------------------------------------

AsyncConfigPortal::AsyncConfigPortal(uint16_t port)
    : _server(port) {
}

// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------

void AsyncConfigPortal::begin(HttpDigestAuth& auth) {
    _auth = &auth;

    // 1. Common system endpoints (/css, /favicon, /menu, /project, /chkauth,
    //    /logout) and the default Status data endpoint.
    _registerSystemRoutes();

    // 2. Built-in pages every project gets: Status (first) and Other (last).
    //    Status uses the overridable statusPageHtml(); Other is fully built-in.
    addPage("/", "Status", statusPageHtml(), AuthLevel::None, MENU_STATUS);
    _registerOtherPage();

    // 3. Subclass adds its project-specific pages (and the Network page).
    registerRoutes();

    // 4. Fire up the server.
    _server.begin();
}

// -----------------------------------------------------------------------------
// Composition API
// -----------------------------------------------------------------------------

bool AsyncConfigPortal::addPage(const char* path, const char* label,
                              const char* html, AuthLevel auth, int8_t order) {
    // Serve the HTML body (optionally auth-gated).
    _server.on(path, HTTP_GET, [this, html, auth](AsyncWebServerRequest* req) {
        // Authentication first, always: a 304 is still an answer, and letting a
        // conditional request short-circuit the check would hand the page's
        // existence — and its cacheability — to anyone who guessed the URL.
        if (auth == AuthLevel::Required && !requireAuth(req)) return;
        if (cacheHit(req)) return;
        sendProgmem(req, 200, "text/html", html);
    });

    // Record a menu entry unless hidden; report registry-full to the caller.
    return _addMenuEntry(path, label, order);
}

namespace {

// Scratch buffer shared by every JSON endpoint.
//
// Why a single static buffer rather than the usual stack array or a malloc:
//
//  * Stack. The buffer has to cover the largest registered document, and
//    NetConfigComponent's /netdata scales with the configured NTP and DNS server
//    counts — kilobytes are realistic. That does not belong on the AsyncTCP
//    task stack, which every connection depends on.
//
//  * malloc. This library avoids dynamic allocation: a long-running device that
//    allocates and frees per request fragments the heap over months of uptime.
//
//  A file-scope buffer is neither: it lives in .bss, is sized at compile time,
//  costs one allocation of zero, and cannot fragment anything. Where PSRAM is
//  available and the build permits BSS in external RAM, define
//  CONFIG_PORTAL_JSON_BUF_IN_PSRAM to move it off internal RAM.
//
// Sharing one buffer is safe because ESPAsyncWebServer dispatches every handler
// from the single AsyncTCP task: handlers run one after another, never
// concurrently, and send() copies the body before the handler returns. Several
// browsers polling at once therefore interleave requests, they do not overlap
// handlers. The flag below guards that invariant rather than trusting it.
//
// Consequence for providers: the buffer is scratch space. Do not retain a
// pointer to it beyond the provider call.
#ifdef CONFIG_PORTAL_JSON_BUF_IN_PSRAM
EXT_RAM_BSS_ATTR
#endif
char g_jsonBuf[AsyncConfigPortal::JSON_ENDPOINT_SIZE];

std::atomic_flag g_jsonBusy = ATOMIC_FLAG_INIT;

}  // namespace

bool AsyncConfigPortal::addJsonEndpoint(const char* path, JsonSource source,
                                      AuthLevel auth) {
    _server.on(path, HTTP_GET,
        [this, source, auth](AsyncWebServerRequest* req) {
            if (auth == AuthLevel::Required && !requireAuth(req)) return;

            // The source owns its storage, so the shared buffer is not involved
            // and no claim is needed here.
            const char* json = source ? source() : nullptr;
            if (json) {
                req->send(200, "application/json", json);
            } else {
                req->send(500, "application/json", "{}");
            }
        });
    return true;
}

bool AsyncConfigPortal::addJsonEndpoint(const char* path, JsonSource source,
                                        JsonRelease release, AuthLevel auth) {
    _server.on(path, HTTP_GET,
        [this, source, release, auth](AsyncWebServerRequest* req) {
            if (auth == AuthLevel::Required && !requireAuth(req)) return;

            // The source keeps its buffer claimed and returns it; we stream it
            // (no String copy) and release the claim when the send completes.
            const char* json = source ? source() : nullptr;
            if (json) {
                req->send(beginStreamed(req, json, "application/json", release));
            } else {
                req->send(500, "application/json", "{}");   // source handled its own unlock
            }
        });
    return true;
}

bool AsyncConfigPortal::addJsonEndpoint(const char* path, JsonProvider provider,
                                      AuthLevel auth) {
    _server.on(path, HTTP_GET,
        [this, provider, auth](AsyncWebServerRequest* req) {
            if (auth == AuthLevel::Required && !requireAuth(req)) return;

            // Claim the shared buffer. Under the single-task invariant this can
            // never fail; if the invariant is ever broken, answering 503 turns a
            // silent data race into a visible, harmless error. test_and_set does
            // not block, so the async task is never stalled.
            if (g_jsonBusy.test_and_set(std::memory_order_acquire)) {
                req->send(503, "application/json", "{}");
                return;
            }

            // A JSON exceeding the buffer is reported as HTTP 500 rather than
            // silently truncated.
            bool ok = provider && provider(g_jsonBuf, sizeof(g_jsonBuf));
            if (ok) {
                // send() copies the body into the response object, so the buffer
                // is free again as soon as this returns.
                req->send(200, "application/json", g_jsonBuf);
            } else {
                req->send(500, "application/json", "{}");
            }

            g_jsonBusy.clear(std::memory_order_release);
        });
    return true;
}

bool AsyncConfigPortal::addPostHandler(const char* path, PostHandler handler,
                                     AuthLevel auth) {
    _server.on(path, HTTP_POST,
        [this, handler, auth](AsyncWebServerRequest* req) {
            if (auth == AuthLevel::Required && !requireAuth(req)) return;
            if (handler) handler(req);
        });
    return true;
}

bool AsyncConfigPortal::addBackupSection(const char* title,
                                            const char* backupPath,
                                            const char* restorePath,
                                            FieldProvider extraFields,
                                            int8_t order) {
    if (!title || !backupPath || !restorePath) {
        logf(LogLevel::Error, "backup",
              PSTR("section not registered: title, backup path and restore path are "
              "all required"));
        return false;
    }

    if (_backupCount >= MAX_BACKUP_SECTIONS) {
        logf(LogLevel::Error, "backup",
              PSTR("section registry full (%u); \"%s\" not registered"),
              (unsigned)MAX_BACKUP_SECTIONS, title);
        return false;
    }

    // The first section brings the page into being. Registering the route and
    // the menu entry here rather than in begin() keeps the "no sections, no
    // page" rule in one place instead of spread across two.
    if (_backupCount == 0) {
        _server.on("/backup", HTTP_GET, [this](AsyncWebServerRequest* req) {
            if (!requireAuth(req)) return;
            sendProgmem(req, 200, "text/html", CONFIG_PORTAL_BACKUP_HTML);
        });
        _server.on("/backupdata", HTTP_GET, [this](AsyncWebServerRequest* req) {
            if (!requireAuth(req)) return;
            _handleBackupData(req);
        });
        _addMenuEntry("/backup", "Backup", MENU_OTHER - 1);
    }

    BackupSection& s = _backupSections[_backupCount++];
    s.title       = title;
    s.backupPath  = backupPath;
    s.restorePath = restorePath;
    s.fields      = extraFields;
    s.order       = order;
    return true;
}

bool AsyncConfigPortal::addResetHandler(const char* what, ResetFn fn) {
    if (!what || !fn) {
        logf(LogLevel::Error, "reset",
             PSTR("handler not registered: both a name and a function are required"));
        return false;
    }
    if (_resetCount >= MAX_RESET_HANDLERS) {
        logf(LogLevel::Error, "reset",
             PSTR("handler registry full (%u): \"%s\" will NOT be erased by a "
             "factory reset"), (unsigned)MAX_RESET_HANDLERS, what);
        return false;
    }
    _resetTargets[_resetCount].what = what;
    _resetTargets[_resetCount].fn   = std::move(fn);
    _resetCount++;
    return true;
}

void AsyncConfigPortal::_handleBackupData(AsyncWebServerRequest* req) const {
    // Sorted by weight, stable for equal weights — the same insertion sort as
    // the menu, and for the same reason: N is at most MAX_BACKUP_SECTIONS.
    uint8_t idx[MAX_BACKUP_SECTIONS];
    for (uint8_t i = 0; i < _backupCount; i++) idx[i] = i;
    for (uint8_t i = 1; i < _backupCount; i++) {
        uint8_t key = idx[i];
        int8_t  kw  = _backupSections[key].order;
        int j = i - 1;
        while (j >= 0 && _backupSections[idx[j]].order > kw) {
            idx[j + 1] = idx[j];
            j--;
        }
        idx[j + 1] = key;
    }

    char json[BACKUPDATA_SIZE];
    size_t n = 0;
    n += snprintf(json + n, sizeof(json) - n, "[");

    for (uint8_t i = 0; i < _backupCount && n < sizeof(json); i++) {
        const BackupSection& s = _backupSections[idx[i]];

        // A provider that fails or does not fit yields an empty list rather
        // than a broken document — but not silently. An empty list is not a
        // cosmetic loss: if the field was there to collect a secret, the
        // restore cannot succeed without it, and the user is left with no way
        // to supply it and no idea why.
        char fields[SECTION_FIELDS_SIZE];
        fields[0] = '\0';
        if (s.fields && !s.fields(fields, sizeof(fields))) {
            fields[0] = '\0';
            logf(LogLevel::Error, "backup",
                  PSTR("section \"%s\": extra fields did not fit in %u bytes; "
                  "restore may be impossible without them"),
                  s.title, (unsigned)sizeof(fields));
        }

        n += snprintf_P(json + n, sizeof(json) - n,
                      PSTR("%s{\"title\":\"%s\",\"backup\":\"%s\",\"restore\":\"%s\","
                      "\"fields\":%s}"),
                      (i == 0) ? "" : ",",
                      s.title, s.backupPath, s.restorePath,
                      fields[0] ? fields : "[]");
    }

    if (n < sizeof(json)) n += snprintf(json + n, sizeof(json) - n, "]");
    req->send(200, "application/json", json);
}

bool AsyncConfigPortal::addUploadHandler(const char* path,
                                            PostHandler onComplete,
                                            UploadHandler onChunk,
                                            AuthLevel auth) {
    _server.on(path, HTTP_POST,
        [this, onComplete, auth](AsyncWebServerRequest* req) {
            if (auth == AuthLevel::Required && !requireAuth(req)) return;
            if (onComplete) onComplete(req);
        },
        [this, onChunk, auth](AsyncWebServerRequest* req, const String& filename,
                              size_t index, uint8_t* data, size_t len, bool final) {
            (void)filename;  // dictated by the callback signature; unused here
            if (auth == AuthLevel::Required && !requireAuth(req)) return;
            if (onChunk) onChunk(req, index, data, len, final);
        });
    return true;
}

// -----------------------------------------------------------------------------
// Project metadata
// -----------------------------------------------------------------------------

void AsyncConfigPortal::setProject(const char* name, const char* version,
                                 const char* desc, const char* year, const char* author) {
    if (name)    _projName   = name;
    if (version) _projVer    = version;
    if (desc)    _projDesc   = desc;
    if (year)    _projYear   = year;
    if (author)  _projAuthor = author;
}

// -----------------------------------------------------------------------------
// Auth helper
// -----------------------------------------------------------------------------

bool AsyncConfigPortal::requireAuth(AsyncWebServerRequest* req) const {
    if (!_auth) {
        logf(LogLevel::Error, "auth",
              PSTR("request refused: no auth store — begin() has not run"));
        return false;
    }

    HttpDigestAuth::Credentials c;
    _auth->getCredentials(c);
    if (!req->authenticate(c.user, c.md5, c.realm, true)) {
        req->requestAuthentication(c.realm, true);
        return false;
    }
    return true;
}

// -----------------------------------------------------------------------------
// Menu registry
// -----------------------------------------------------------------------------

bool AsyncConfigPortal::_addMenuEntry(const char* path, const char* label,
                                    int8_t order) {
    if (order == MENU_HIDDEN) return true;        // not a menu item — still ok
        if (_pageCount >= MAX_PAGES) {
        // Otherwise the page simply is not in the menu, with nothing anywhere
        // to say why — the same silent loss as a dropped profile.
        logf(LogLevel::Error, "web",
              PSTR("menu full (%u entries): \"%s\" will not appear; raise MAX_PAGES"),
              (unsigned)MAX_PAGES, label ? label : path);
        return false;
    }


    _pages[_pageCount].path  = path;
    _pages[_pageCount].label = label;
    _pages[_pageCount].order = order;
    _pageCount++;
    return true;
}

// -----------------------------------------------------------------------------
// Logging
// -----------------------------------------------------------------------------
//
// Routes through the optional LogFn set via setLogger(). If none is set, the
// server is silent — it never assumes a configured Serial, so it is safe to
// reuse in projects that own the serial port differently (or not at all).
// This is the one implementation behind logf(), vlogf() and logf().
void AsyncConfigPortal::vlogf(LogLevel level, const char* tag,
                                 const char* fmt, va_list ap) const {
    if (!_log) return;
    char line[LOG_SIZE];
    vsnprintf_P(line, sizeof(line), fmt, ap);
    _log(level, tag, line);
}

// The varargs form must route through vlogf(): C++ gives no way to pass "..."
// on to another variadic function, so va_list is the only channel. A shim that
// simply calls another variadic function drops every argument and leaves
// vsnprintf reading operands nobody ever pushed.
void AsyncConfigPortal::logf(LogLevel level, const char* tag,
                                const char* fmt, ...) const {
    va_list ap;
    va_start(ap, fmt);
    vlogf(level, tag, fmt, ap);
    va_end(ap);
}

// -----------------------------------------------------------------------------
// System routes
// -----------------------------------------------------------------------------

void AsyncConfigPortal::_registerSystemRoutes() {
    // Shared CSS.
    _server.on("/css", HTTP_GET, [this](AsyncWebServerRequest* req) {
        if (cacheHit(req)) return;
        // Both come from members, not macros: this file is a separate
        // translation unit, so a stylesheet named in a sketch is not visible
        // here. The supplement, if any, follows the sheet — later rules win, so
        // overriding a variable needs no copy of the original.
        sendProgmem(req, 200, "text/css",
                    _css ? _css : CONFIG_PORTAL_CSS, _cssExtra);
    });

    // Shared JavaScript (menu, project, auth-state helpers). Browser-cached,
    // so pages include it via <script src="/common.js"> instead of inlining.
    _server.on("/common.js", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (cacheHit(req)) return;
        sendProgmem(req, 200, "application/javascript", CONFIG_PORTAL_COMMON_JS);
    });

    // Field toolkit (validation engine + row builders). Only form pages need it,
    // so it is a separate script rather than part of /common.js. A standalone
    // portal can reuse this same asset without the runtime.
    _server.on("/fields.js", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (cacheHit(req)) return;
        sendProgmem(req, 200, "application/javascript", CONFIG_PORTAL_FIELDS_JS);
    });

    // Favicon: deliberate placeholder. Returns 204 No Content so browsers get
    // a clean answer (no 404 noise) without embedding a binary icon. Projects
    // that want a real favicon can register their own /favicon.ico route.
    _server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(204);
    });

    // Dynamic menu (ordered by weight).
    _server.on("/menu", HTTP_GET, [this](AsyncWebServerRequest* req) {
        _handleMenu(req);
    });

    // Project metadata.
    _server.on("/project", HTTP_GET, [this](AsyncWebServerRequest* req) {
        _handleProject(req);
    });

    // Default Status data (system info; subclass may override statusJson()).
    _server.on("/statusdata", HTTP_GET, [this](AsyncWebServerRequest* req) {
        _handleStatusData(req);
    });

    // Auth probe — used by pages to show/hide the Logout link.
    _server.on("/chkauth", HTTP_GET, [this](AsyncWebServerRequest* req) {
        HttpDigestAuth::Credentials c;
        if (_auth) _auth->getCredentials(c);
        if (_auth && !req->authenticate(c.user, c.md5, c.realm, true)) {
            req->send(403, "text/plain", "");
        } else {
            req->send(200, "text/plain", "");
        }
    });

    // Logout — forces re-authentication then redirects (page handles the JS).
    _server.on("/logout", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->requestAuthentication();
        sendProgmem(req, 200, "text/html", CONFIG_PORTAL_LOGOUT_HTML);
    });

    // Unknown paths.
    _server.onNotFound([](AsyncWebServerRequest* req) {
        sendProgmem(req, 404, "text/html", CONFIG_PORTAL_NOTFOUND_HTML);
    });
}

void AsyncConfigPortal::_handleMenu(AsyncWebServerRequest* req) const {
    // Build an index array sorted by weight (stable: equal weights keep
    // registration order). Small N (<= MAX_PAGES), so insertion sort is fine.
    uint8_t idx[MAX_PAGES];
    for (uint8_t i = 0; i < _pageCount; i++) idx[i] = i;

    for (uint8_t i = 1; i < _pageCount; i++) {
        uint8_t key = idx[i];
        int8_t  kw  = _pages[key].order;
        int j = i - 1;
        while (j >= 0 && _pages[idx[j]].order > kw) {
            idx[j + 1] = idx[j];
            j--;
        }
        idx[j + 1] = key;
    }

    // Emit a JSON array: [{"path":"/","label":"Status"}, ...]
    char json[MENU_SIZE];
    size_t n = 0;
    n += snprintf(json + n, sizeof(json) - n, "[");
    for (uint8_t i = 0; i < _pageCount && n < sizeof(json); i++) {
        const PageEntry& p = _pages[idx[i]];
        n += snprintf_P(json + n, sizeof(json) - n,
                      PSTR("%s{\"path\":\"%s\",\"label\":\"%s\"}"),
                      (i == 0) ? "" : ",",
                      p.path ? p.path : "",
                      p.label ? p.label : "");
    }
    if (n < sizeof(json)) n += snprintf(json + n, sizeof(json) - n, "]");

    req->send(200, "application/json", json);
}

void AsyncConfigPortal::_handleProject(AsyncWebServerRequest* req) const {
    char json[PROJECT_SIZE];
    snprintf_P(json, sizeof(json),
        PSTR("{\"project_name\":\"%s\",\"project_ver\":\"%s\","
        "\"project_desc\":\"%s\",\"project_year\":\"%s\",\"author\":\"%s\"}"),
        _projName, _projVer, _projDesc, _projYear, _projAuthor);
    req->send(200, "application/json", json);
}

void AsyncConfigPortal::_handleStatusData(AsyncWebServerRequest* req) const {
    char json[STATUS_SIZE];
    if (statusJson(json, sizeof(json))) {
        req->send(200, "application/json", json);
    } else {
        req->send(500, "application/json", "{}");
    }
}

// -----------------------------------------------------------------------------
// Deferred restart
// -----------------------------------------------------------------------------
//
// Reboots after a short delay rather than inside the async request handler.
// Calling ESP.restart() directly in a handler tears down the TCP connection
// before the HTTP response is flushed, which the browser sees as a timeout.
// The delay lets the reply go out first. ESP32 uses a short-lived task on the
// caller's core; ESP8266 (no FreeRTOS) uses a one-shot Ticker.

void AsyncConfigPortal::_scheduleRestart() {
#if defined(ARDUINO_ARCH_ESP32)
    // ESP32: a short-lived task on the caller's core, so the flush delay does not
    // block the async handler.
    BaseType_t core = xTaskGetCoreID(nullptr);
    xTaskCreatePinnedToCore(
        [](void*) {
            vTaskDelay(pdMS_TO_TICKS(CONFIG_PORTAL_RESTART_DELAY_MS)); // let the HTTP response flush
            ESP.restart();
        },
        "cfgRestart", 2048, nullptr, 1, nullptr, core);
#elif defined(ARDUINO_ARCH_ESP8266)
    // ESP8266 has no FreeRTOS: a one-shot Ticker fires the restart off the async
    // handler after the same flush delay.
    //
    // The Ticker runs in the SYS context, and ESP.restart() called from there
    // does not stop the cont task: the SDK begins tearing the network stack down
    // while loop() carries on using it. An application that polls its network
    // layer every pass — as NetworkManager does — then reaches a netif the
    // restart has already freed, and the reboot ends in a LoadProhibited
    // exception instead of a clean one:
    //
    //     netif_remove <- eagle_lwip_if_free <- wifi_station_disconnect
    //                  <- WiFi.disconnect() <- adapter stop <- loop()
    //
    // schedule_function() queues the call for the cont context, so it runs
    // between two loop() passes with nothing else in flight. The delay still
    // belongs to the Ticker: the response has to flush first.
    _restartTicker.once_ms(CONFIG_PORTAL_RESTART_DELAY_MS, +[]() {
        schedule_function([]() { ESP.restart(); });
    });
#endif
}

// -----------------------------------------------------------------------------
// Built-in Other page: OTA / restart / factory reset / password / backup
// -----------------------------------------------------------------------------

void AsyncConfigPortal::_registerOtherPage() {
    // The page itself.
    addPage("/misc", "Other", CONFIG_PORTAL_OTHER_HTML,
            AuthLevel::Required, MENU_OTHER);

    // Auth data (username only) for the page to display.
    _server.on("/authdata", HTTP_GET, [this](AsyncWebServerRequest* req) {
        if (!requireAuth(req)) return;
        char json[HttpDigestAuth::JSON_LEN + 1];
        if (_auth && _auth->toJson(json, sizeof(json))) {
            req->send(200, "application/json", json);
        } else {
            req->send(500, "application/json", "{}");
        }
    });

    // Password change.
    _server.on("/misc", HTTP_POST, [this](AsyncWebServerRequest* req) {
        if (!requireAuth(req)) return;
        if (!_auth) { req->send(500); return; }

        HttpDigestAuth::Credentials c;
        _auth->getCredentials(c);
        // String-free reads straight into the credential buffers.
        postVal(req, "user", c.user, HttpDigestAuth::MAX_USER_SIZE);
        postVal(req, "pw",   c.pw,   HttpDigestAuth::MAX_PASSWORD_SIZE);
        _auth->setCredentials(c);
        _auth->save();
        sendProgmem(req, 200, "text/html", CONFIG_PORTAL_PWCHANGE_HTML);
    });

    // Restart.
    _server.on("/restart", HTTP_POST, [this](AsyncWebServerRequest* req) {
        if (!requireAuth(req)) return;
        sendProgmem(req, 200, "text/html", CONFIG_PORTAL_REBOOT_HTML);
        // Reboot from a short-lived task so the response flushes first (see the
        // OTA handler for the rationale).
        _scheduleRestart();
    });

    // Factory reset: erase every registered target, then the credentials, then
    // reboot.
    //
    // Credentials go last and are cleared rather than rewritten: the base does
    // not know what "default" means for this project — the application does,
    // and the boot pattern the examples use,
    //
    //     if (!auth.restore()) { auth.setCredentials(...); auth.save(); }
    //
    // puts the documented defaults back on the next start. Clearing them is
    // deliberate: a forgotten password is one of the main reasons anyone
    // reaches for a factory reset, and a reset that preserves it does half its
    // job. The route is behind auth, so this is not an escalation — but it does
    // mean an authenticated user can return the device to default credentials,
    // which belongs in the documentation.
    //
    // A failing target does not stop the sequence. Every handler runs and the
    // device restarts either way: half-erased but still running, with no signal
    // that anything went wrong, is the worse of the two states.
    _server.on("/factoryreset", HTTP_POST, [this](AsyncWebServerRequest* req) {
        if (!requireAuth(req)) return;

        uint8_t failed = 0;
        for (uint8_t i = 0; i < _resetCount; i++) {
            if (_resetTargets[i].fn && _resetTargets[i].fn()) continue;
            failed++;
            logf(LogLevel::Error, "reset", PSTR("\"%s\" could not be erased"),
                 _resetTargets[i].what);
        }

        if (_auth && !_auth->clear()) {
            failed++;
            logf(LogLevel::Error, "reset", PSTR("credentials could not be erased"));
        }

        logf(failed ? LogLevel::Warn : LogLevel::Info, "reset",
             "factory reset done, %u of %u target(s) failed; restarting",
             (unsigned)failed, (unsigned)(_resetCount + 1));

        sendProgmem(req, 200, "text/html", CONFIG_PORTAL_FACTORYRESET_HTML);
        _scheduleRestart();
    });

    // OTA firmware update: upload handler + completion handler.
    _server.on("/update", HTTP_POST,
        // onRequest: fires after the upload completes.
        [this](AsyncWebServerRequest* req) {
            if (!requireAuth(req)) return;
            // A marker mismatch is reported distinctly from a flash error.
            if (_otaReject) {
                sendProgmem(req, 200, "text/html", CONFIG_PORTAL_FWMISMATCH_HTML);
                _otaReject = false;
                return;
            }
            bool ok = !Update.hasError();
            sendProgmem(req, 200, "text/html",
                        ok ? CONFIG_PORTAL_REBOOT_HTML : CONFIG_PORTAL_FILEERROR_HTML);
            // Restart from a short-lived task, not here: calling ESP.restart()
            // inside the async handler cuts the TCP connection before the reply
            // is fully flushed, which the browser sees as a timeout. The task
            // waits long enough for the response to be sent, then reboots.
            if (ok) _scheduleRestart();
        },
        // onUpload: fires for each chunk.
        [this](AsyncWebServerRequest* req, const String& filename, size_t index,
               uint8_t* data, size_t len, bool final) {
            (void)filename;  // dictated by the callback signature; unused here
            if (!requireAuth(req)) return;

            if (index == 0) {
                // The identity marker lives in the image's .rodata, which can be
                // well past the first chunk, so we can't validate up-front. We
                // write to the (inactive) OTA partition while scanning every
                // chunk for the marker, and only finalize (making the image
                // bootable) if a matching marker was seen. A wrong/missing
                // marker → the update is aborted, leaving the running firmware
                // intact (ESP32: Update.abort(); ESP8266: Update.end(false)).
                _otaMarkerFound = false;
                _otaMarkerMatch = false;
                _otaTailLen     = 0;
#if defined(ARDUINO_ARCH_ESP8266)
                Update.runAsync(true);   // required for the async server on ESP8266
#endif
                if (!Update.begin((ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000)) {
                    logf(LogLevel::Error, "ota", PSTR("begin failed: %s"), ACP_UPDATE_ERR);
                }
            }

            // Scan this chunk for the marker (until found). To catch a marker
            // straddling a chunk boundary, prepend the tail we kept from the
            // previous chunk and scan the joined region too.
            if (!_otaMarkerFound) {
                FirmwareMarker fm;
                if (fwMarkerFind(data, len, fm)) {
                    _otaMarkerFound = true;
                    FwMatch m = fwMarkerCheck(fm);
                    _otaMarkerMatch = (m == FwMatch::Ok);
                    logf(LogLevel::Info, "ota", PSTR("marker: %s (project=%s board=%s)"),
                          fwMatchStr(m), fm.project, fm.board);
                } else if (_otaTailLen > 0) {
                    // Check the boundary: [kept tail][start of this chunk].
                    uint8_t join[OTA_TAIL_KEEP + sizeof(FirmwareMarker)];
                    size_t head = (len < sizeof(FirmwareMarker))
                                ? len : sizeof(FirmwareMarker);
                    memcpy(join, _otaTail, _otaTailLen);
                    memcpy(join + _otaTailLen, data, head);
                    if (fwMarkerFind(join, _otaTailLen + head, fm)) {
                        _otaMarkerFound = true;
                        FwMatch m = fwMarkerCheck(fm);
                        _otaMarkerMatch = (m == FwMatch::Ok);
                        logf(LogLevel::Info, "ota", PSTR("marker (boundary): %s"), fwMatchStr(m));
                    }
                }
                // Keep the tail of this chunk for the next boundary check.
                if (!_otaMarkerFound) {
                    size_t keep = (len < OTA_TAIL_KEEP) ? len : OTA_TAIL_KEEP;
                    memcpy(_otaTail, data + (len - keep), keep);
                    _otaTailLen = keep;
                }
            }

            if (!Update.hasError()) {
                if (Update.write(data, len) != len)
                    logf(LogLevel::Error, "ota", PSTR("write failed: %s"), ACP_UPDATE_ERR);
            }

            if (final) {
                // Finalize only if a matching marker was found in the stream.
                if (!_otaMarkerFound || !_otaMarkerMatch) {
                    logf(LogLevel::Warn,  "ota", PSTR("rejected: no matching marker — aborting"));
#if defined(ARDUINO_ARCH_ESP32)
                    Update.abort();          // inactive partition never activated
#elif defined(ARDUINO_ARCH_ESP8266)
                    // ESP8266 has no abort(). begin() was sized to the whole free
                    // sketch space, so the image is never "finished"; end(false)
                    // therefore resets it without writing the eboot command, and
                    // the running firmware stays.
                    Update.end(false);
#endif
                    _otaReject = true;
                    return;
                }
                if (Update.end(true)) {
                    logf(LogLevel::Info,  "ota", PSTR("success, %u bytes"), (unsigned)(index + len));
                } else {
                    logf(LogLevel::Error, "ota", PSTR("end failed: %s"), ACP_UPDATE_ERR);
                }
            }
        });
}

// -----------------------------------------------------------------------------
// Default (overridable) Status page
// -----------------------------------------------------------------------------

const char* AsyncConfigPortal::statusPageHtml() const {
    // Generic system-info page. Subclasses override for project-specific status.
    return CONFIG_PORTAL_STATUS_HTML;
}

bool AsyncConfigPortal::statusJson(char* buf, size_t len) const {
    // Interface-independent system info only — no network assumptions.
    uint32_t uptime_s = millis() / 1000UL;
    const char* reset = "unknown";
#if defined(ARDUINO_ARCH_ESP32)
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:  reset = "poweron";  break;
        case ESP_RST_SW:       reset = "software"; break;
        case ESP_RST_PANIC:    reset = "panic";    break;
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:      reset = "watchdog"; break;
        case ESP_RST_BROWNOUT: reset = "brownout"; break;
        case ESP_RST_DEEPSLEEP:reset = "deepsleep";break;
        default: break;
    }
#elif defined(ARDUINO_ARCH_ESP8266)
    switch (ESP.getResetInfoPtr()->reason) {
        case REASON_DEFAULT_RST:
        case REASON_EXT_SYS_RST:      reset = "poweron";   break;
        case REASON_SOFT_RESTART:     reset = "software";  break;
        case REASON_EXCEPTION_RST:    reset = "panic";     break;
        case REASON_WDT_RST:
        case REASON_SOFT_WDT_RST:     reset = "watchdog";  break;
        case REASON_DEEP_SLEEP_AWAKE: reset = "deepsleep"; break;
        default: break;
    }
#endif

    // Heap health, including a fragmentation indicator for long-running /
    // industrial deployments: even when free heap is ample, the largest
    // contiguous block (max_alloc) can shrink as the async web stack allocates
    // and frees. frag_pct = 100 * (1 - maxAlloc/free) — rising over time, at
    // comparable load, is the signal to watch.
    //
    // Only at comparable load: the divisor is free heap, so releasing memory
    // raises the percentage without the largest block having moved. A reading
    // taken at rest can look worse than one taken just after a page load while
    // being the healthier state. max_alloc is the figure that decides whether a
    // document can be built; the page says so in a tooltip.
    //
    // Compute in fixed-point to avoid float in the hot path.
    uint32_t heap_free = ESP.getFreeHeap();
#if defined(ARDUINO_ARCH_ESP32)
    uint32_t    heap_min  = ESP.getMinFreeHeap();
    uint32_t    max_alloc = ESP.getMaxAllocHeap();
    const char* chip      = ESP.getChipModel();
    unsigned    cores     = ESP.getChipCores();
    float       temp      = temperatureRead();
#elif defined(ARDUINO_ARCH_ESP8266)
    uint32_t    max_alloc = ESP.getMaxFreeBlockSize();
    const char* chip      = "ESP8266";
    unsigned    cores     = 1;
    // No minimum-heap tracking and no die-temperature sensor here. The two keys
    // are left out of the document rather than sent as zeros: a reading that does
    // not exist is not the same fact as a reading of zero, and the page hides a
    // row whose key is absent.
#endif
    uint32_t frag_pct  = (heap_free > 0)
        ? (uint32_t)(100UL - (100ULL * max_alloc / heap_free)) : 0;

    int n = snprintf_P(buf, len,
        PSTR("{\"uptime_s\":%lu,\"heap_free\":%lu,"
#if defined(ARDUINO_ARCH_ESP32)
        "\"heap_min\":%lu,"
#endif
        "\"heap_max_alloc\":%lu,\"heap_frag_pct\":%lu,"
        "\"chip\":\"%s\",\"cores\":%u,\"cpu_mhz\":%lu,"
#if defined(ARDUINO_ARCH_ESP32)
        "\"temp\":%.0f,"
#endif
        "\"flash_size\":%lu,\"reset_reason\":\"%s\"}"),
        (unsigned long)uptime_s,
        (unsigned long)heap_free,
#if defined(ARDUINO_ARCH_ESP32)
        (unsigned long)heap_min,
#endif
        (unsigned long)max_alloc,
        (unsigned long)frag_pct,
        chip,
        (unsigned)cores,
        (unsigned long)ESP.getCpuFreqMHz(),
#if defined(ARDUINO_ARCH_ESP32)
        temp,
#endif
        (unsigned long)ESP.getFlashChipSize(),
        reset);

    return (n > 0 && (size_t)n < len);
}
