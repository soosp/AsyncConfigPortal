/**
 * @file AsyncConfigPortalHosted.cpp
 * @brief The ESP-Hosted co-processor section of the Other page: what it shows,
 *        and its update from an uploaded image or one the device downloads.
 *
 * Compiled only with CONFIG_PORTAL_HOSTED_OTA (detail/hosted_config.h): an
 * ESP32 build with ESP-Hosted whose board variant declares a co-processor.
 *
 * No request handler talks to the co-processor. The first design wrote each
 * block from the upload handler, on measurements taken over Ethernet with
 * current co-processor firmware (~17 ms a block). Updating an old C6 over
 * Wi-Fi — the case the feature exists for — tripped the AsyncTCP task's
 * watchdog: the upload was arriving through the very co-processor being
 * written, and old firmware erases as it writes, so a single RPC could take
 * seconds. Hence:
 *
 *   upload    POST /hostedota: the handler only copies into a buffer in PSRAM
 *             (an image is ~1.2 MB) and checks the image head on the way, with
 *             no RPC. The answer goes out once the file is in.
 *   write     a short-lived task: starts ESP-Hosted if the application has not,
 *             hostedBeginUpdate(), the image in 2 KB blocks, hostedEndUpdate()
 *             (which verifies it). Blocking is harmless there, and by then the
 *             network no longer competes with the writes.
 *   activate  the same task: onBeforeRestart(Coprocessor), then
 *             hostedActivateUpdate(). The co-processor restarts, and
 *             ESP-Hosted resets the host by itself within ~100 ms; the
 *             portal's own restart is only a fallback.
 *
 *   download  POST /hostedfetch (CONFIG_PORTAL_HOSTED_FETCH) starts the same
 *             task, which first downloads the image from Espressif's mirror
 *             into the same buffer, over verified TLS, then goes on as above.
 *             The whole file is in before the first write, so on Wi-Fi the
 *             download and the writes never share the co-processor's link.
 *
 * The page polls /firmware throughout ("writing 45%", then "activating").
 * An upload that stops arriving (the browser closed) is dropped after 30 s and
 * its buffer freed by a timer, without waiting for a request.
 *
 * Whatever fails before the activation, the co-processor keeps its firmware:
 * the writes go to its inactive partition, and an image that was not ended is
 * never activated. Its next Begin starts afresh (measured).
 */
#include "AsyncConfigPortal.h"

#if CONFIG_PORTAL_HOSTED_OTA

#include <Update.h>
#include <esp_heap_caps.h>
#include <esp_app_format.h>   // esp_image_header_t, esp_chip_id_t
#include <esp_app_desc.h>     // esp_app_desc_t
#if CONFIG_PORTAL_HOSTED_FETCH
#  include <NetworkClientSecure.h>
#  include <HTTPClient.h>
#endif

namespace {

/// The co-processor's chip name as ESP-Hosted gives it, to the chip ID in an
/// image header. Only targets that can serve as an ESP-Hosted co-processor.
struct ChipName { const char* name; uint16_t id; };
const ChipName CHIPS[] = {
    { "esp32",    ESP_CHIP_ID_ESP32   },
    { "esp32s2",  ESP_CHIP_ID_ESP32S2 },
    { "esp32s3",  ESP_CHIP_ID_ESP32S3 },
    { "esp32c2",  ESP_CHIP_ID_ESP32C2 },
    { "esp32c3",  ESP_CHIP_ID_ESP32C3 },
    { "esp32c5",  ESP_CHIP_ID_ESP32C5 },
    { "esp32c6",  ESP_CHIP_ID_ESP32C6 },
    { "esp32c61", ESP_CHIP_ID_ESP32C61 },
};

/// The firmware an ESP-Hosted co-processor runs names itself so.
const char HOSTED_PROJECT[] = "network_adapter";

const char* stateName(uint8_t s) {
    static const char* const N[] = { "idle", "receiving", "fetching", "writing", "activating",
                                     "failed" };
    return s < sizeof(N) / sizeof(N[0]) ? N[s] : "idle";
}

/// The host's ESP-Hosted version as text, "2.12.13".
void hostVersion(char* out, size_t len) {
    uint32_t a = 0, b = 0, c = 0;
    hostedGetHostVersion(&a, &b, &c);
    snprintf(out, len, "%lu.%lu.%lu", (unsigned long)a, (unsigned long)b, (unsigned long)c);
}

}  // namespace

// -----------------------------------------------------------------------------
// What the page shows
// -----------------------------------------------------------------------------
//
// Never brings ESP-Hosted up: initialising it configures SDIO and toggles the
// co-processor's reset pin (GPIO54 on the ESP32-P4), which on a board without
// one is somebody else's pin. Until the application has started Wi-Fi — or the
// user asks for an update — the section says the co-processor is not running.
//
// No RPC from a handler either. The versions are what the core noted when it
// brought ESP-Hosted up (hostedInit() asks the co-processor once).

/// The co-processor's chip name. Asking costs an RPC, so only the writer task
/// may (@p mayAsk): handlers get the cached name, or until there is one the
/// target the build was made for. Asked only of a co-processor new enough to
/// say (2.12.2, the core's own threshold); older ones are taken to be the build
/// target, as the core does.
const char* AsyncConfigPortal::_hostedTarget(bool mayAsk) {
    static char target[HOSTED_TARGET_LEN + 1] = {};
    if (!target[0] && mayAsk && hostedIsInitialized()) {
        uint32_t a = 0, b = 0, c = 0;
        hostedGetSlaveVersion(&a, &b, &c);
        if (((a << 16) | (b << 8) | c) >= ((2u << 16) | (12u << 8) | 2u))
            snprintf(target, sizeof(target), "%s", hostedGetSlaveTargetName());
    }
    return target[0] ? target : CONFIG_IDF_SLAVE_TARGET;
}

bool AsyncConfigPortal::_hostedJson(char* out, size_t len) {
    _hostedExpire();

    uint32_t hMaj = 0, hMin = 0, hPat = 0, sMaj = 0, sMin = 0, sPat = 0;
    hostedGetHostVersion(&hMaj, &hMin, &hPat);
    const bool started = hostedIsInitialized();
    if (started) hostedGetSlaveVersion(&sMaj, &sMin, &sPat);
    const bool slaveKnown = started && (sMaj | sMin | sPat) != 0;
    const char* tgt = _hostedTarget(false);

    char slave[HOSTED_VER_LEN + 1] = "";
    if (slaveKnown) snprintf(slave, sizeof(slave), "%lu.%lu.%lu",
                             (unsigned long)sMaj, (unsigned long)sMin, (unsigned long)sPat);
    const bool update = started &&
        (!slaveKnown || sMaj != hMaj || sMin != hMin || sPat != hPat);
    const uint8_t st = _hState.load();

    // The URL as hostedGetUpdateURL() builds it — built here instead, because
    // that function asks the co-processor for its name on every call.
    const int w = snprintf(out, len, HOSTED_JSON_FMT,
        started ? "true" : "false",
        (unsigned long)hMaj, (unsigned long)hMin, (unsigned long)hPat,
        slave, tgt, update ? "true" : "false",
        stateName(st), st == (uint8_t)HState::Failed ? _hErr : "",
        (unsigned long)_hDone.load(), (unsigned long)_hTotal,
        tgt, (unsigned long)hMaj, (unsigned long)hMin, (unsigned long)hPat);
    return w > 0 && (size_t)w < len;
}

// -----------------------------------------------------------------------------
// The update
// -----------------------------------------------------------------------------

void AsyncConfigPortal::_hostedRelease() {
    if (_hImg) heap_caps_free(_hImg);
    _hImg = nullptr;
    _hCap = _hLen = 0;
}

void AsyncConfigPortal::_hostedFail(const char* code, const char* msg) {
    _hErr = code;
    snprintf(_hMsg, sizeof(_hMsg), "%s", msg);
    _hState.store((uint8_t)HState::Failed);
    logf(LogLevel::Warn, "hosted", "%s: %s", code, msg);
}

/// Drops an upload that stopped arriving, and frees its buffer. A periodic
/// timer checks while an upload is received (_hostedWatch()), so the memory
/// comes back even if nobody opens the page again; /firmware and the next
/// upload check on their way through as well.
void AsyncConfigPortal::_hostedExpire() {
    std::lock_guard<std::mutex> g(_hLock);
    _hostedExpireLocked();
}

void AsyncConfigPortal::_hostedExpireLocked() {
    if ((HState)_hState.load() == HState::Receiving && millis() - _hSeen > HOSTED_IDLE_MS) {
        _hOwner = nullptr;
        _hostedRelease();
        _hostedFail("timeout", "The upload stopped. Nothing was changed.");
    }
}

void AsyncConfigPortal::_hostedWatch() {
    if (!_hTimer) {
        esp_timer_create_args_t a = {};
        a.callback = &AsyncConfigPortal::_hostedWatchFired;
        a.arg      = this;
        a.name     = "hostedWatch";
        if (esp_timer_create(&a, &_hTimer) != ESP_OK) { _hTimer = nullptr; return; }
    }
    esp_timer_stop(_hTimer);   // harmless when it was not running
    esp_timer_start_periodic(_hTimer, (uint64_t)HOSTED_WATCH_MS * 1000u);
}

/// In the esp_timer task: expires a stalled upload, and stops itself once
/// nothing is being received.
void AsyncConfigPortal::_hostedWatchFired(void* arg) {
    auto* self = static_cast<AsyncConfigPortal*>(arg);
    self->_hostedExpire();
    if ((HState)self->_hState.load() != HState::Receiving) esp_timer_stop(self->_hTimer);
}

/// The checks before anything goes to the co-processor, all within the first
/// 256 bytes (the official esp32c6-v2.12.13.bin as the reference): image magic
/// 0xE9; the chip ID at offset 12 against the co-processor's chip; the app
/// descriptor's magic at 32; its project name, "network_adapter"; and its
/// version, which has to be the host's exactly — ESP-Hosted 3.0 changed the
/// RPC and the wire protocol, and nothing states that unequal versions talk, so
/// a mismatch is refused before it can cost the device its Wi-Fi.
bool AsyncConfigPortal::_hostedCheckHead(const uint8_t* p, size_t n, const char* target) {
    const size_t descAt = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
    if (n < descAt + sizeof(esp_app_desc_t)) {
        _hostedFail("bad_file", "Not a firmware image. Nothing was changed.");
        return false;
    }
    esp_image_header_t hdr;
    memcpy(&hdr, p, sizeof(hdr));
    esp_app_desc_t desc;
    memcpy(&desc, p + descAt, sizeof(desc));
    if (hdr.magic != ESP_IMAGE_HEADER_MAGIC || desc.magic_word != ESP_APP_DESC_MAGIC_WORD) {
        _hostedFail("bad_file", "Not a firmware image. Nothing was changed.");
        return false;
    }

    for (const ChipName& c : CHIPS) {
        if (strcmp(c.name, target) != 0) continue;
        if (hdr.chip_id != c.id) {
            _hostedFail("wrong_chip", "This file is for another chip. Nothing was changed.");
            return false;
        }
        break;
    }

    char project[sizeof(desc.project_name) + 1] = {};
    memcpy(project, desc.project_name, sizeof(desc.project_name));
    if (strcmp(project, HOSTED_PROJECT) != 0) {
        _hostedFail("not_hosted", "Not an ESP-Hosted co-processor image. Nothing was changed.");
        return false;
    }

    char version[sizeof(desc.version) + 1] = {};
    memcpy(version, desc.version, sizeof(desc.version));
    char host[HOSTED_VER_LEN + 1];
    hostVersion(host, sizeof(host));
    if (strcmp(version, host) != 0) {
        char m[sizeof(_hMsg)];
        snprintf(m, sizeof(m), "This image is ESP-Hosted %s; this device's firmware works with %s. "
                 "Nothing was changed.", version, host);
        _hostedFail("version_mismatch", m);
        return false;
    }
    return true;
}

bool AsyncConfigPortal::_hostedBusyLocked() {
    const HState s = (HState)_hState.load();
    return (s != HState::Idle && s != HState::Failed) || Update.isRunning() ||
           _restartScheduled.load();
}

void AsyncConfigPortal::_hostedChunk(AsyncWebServerRequest* req, size_t index,
                                     uint8_t* data, size_t len, bool final) {
    std::lock_guard<std::mutex> g(_hLock);
    if (index == 0) {
        _hostedExpireLocked();
        _hBusy = _hostedBusyLocked();
        if (_hBusy) return;

        // The whole request is the upper bound of the file: the multipart
        // framing around it only adds.
        const size_t cap = req->contentLength();
        _hErr = "";
        _hMsg[0] = '\0';
        _hDone = 0;
        _hTotal = 0;
        _hChecked = false;
        _hOwner = req;
        if (cap == 0 || cap > HOSTED_MAX_UPLOAD) {
            _hostedFail("too_large", "The file is larger than a co-processor image can be. "
                        "Nothing was changed.");
            return;
        }
        _hImg = static_cast<uint8_t*>(heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!_hImg) {
            _hostedFail("no_memory", "Not enough memory to take the image (it is kept in PSRAM). "
                        "Nothing was changed.");
            return;
        }
        _hCap = cap;
        _hLen = 0;
        _hSeen = millis();
        _hState.store((uint8_t)HState::Receiving);
        _hostedWatch();
    }
    if (_hOwner != req || (HState)_hState.load() != HState::Receiving) return;
    _hSeen = millis();

    if (_hLen + len > _hCap) {   // cannot happen with an honest Content-Length
        _hostedRelease();
        _hostedFail("too_large", "The upload was larger than it announced. Nothing was changed.");
        return;
    }
    memcpy(_hImg + _hLen, data, len);
    _hLen += len;

    // The head as soon as it is in: a wrong file is refused at the end of the
    // upload without the co-processor having been touched. Against the cached
    // chip name (no RPC here); the writer task checks the chip again once it
    // has asked the co-processor.
    if (!_hChecked && (_hLen >= HOSTED_HEAD || final)) {
        if (!_hostedCheckHead(_hImg, _hLen, _hostedTarget(false))) {
            _hostedRelease();
            return;
        }
        _hChecked = true;
    }
}

void AsyncConfigPortal::_hostedComplete(AsyncWebServerRequest* req) {
    if (!requireAuth(req)) return;
    std::lock_guard<std::mutex> g(_hLock);
    if (_hOwner != req) {
        if (_hBusy) {
            _hBusy = false;
            sendResult_P(req, 409, "busy", -1, PSTR("Another update is running. Nothing was changed."));
        } else {
            sendResult_P(req, 400, "no_file", -1, PSTR("No file arrived. Nothing was changed."));
        }
        return;
    }
    _hOwner = nullptr;
    const HState s = (HState)_hState.load();
    if (s == HState::Failed) {
        sendResult(req, 400, _hErr, -1, _hMsg);
        return;
    }
    if (s != HState::Receiving || !_hChecked) {
        _hostedRelease();
        _hostedFail("bad_file", "The upload was incomplete. Nothing was changed.");
        sendResult(req, 400, _hErr, -1, _hMsg);
        return;
    }

    _hTotal = _hLen;
    _hState.store((uint8_t)HState::Writing);
    logf(LogLevel::Info, "hosted", "image received, %lu bytes; writing it to the co-processor",
         (unsigned long)_hLen);
    if (!_hostedStartWriter(false)) {
        sendResult(req, 500, _hErr, -1, _hMsg);
        return;
    }
    sendResult_P(req, 200, "ok", -1, PSTR("Received: writing it to the co-processor"));
}

bool AsyncConfigPortal::_hostedStartWriter(bool fetch) {
    _hFetch = fetch;
    uint32_t stack = fetch ? HOSTED_FETCH_STACK : HOSTED_WRITE_STACK;
    if (stack < CONFIG_PORTAL_RESTART_STACK) stack = CONFIG_PORTAL_RESTART_STACK;
    if (xTaskCreatePinnedToCore(_hostedWriterTask, "hostedWrite", stack, this, 1, nullptr,
                                xTaskGetCoreID(nullptr)) == pdPASS)
        return true;
    _hostedRelease();
    _hostedFail("busy", "The device could not start the update. Nothing was changed.");
    return false;
}

#if CONFIG_PORTAL_HOSTED_FETCH
void AsyncConfigPortal::_hostedFetchRequest(AsyncWebServerRequest* req) {
    if (!requireAuth(req)) return;
    std::lock_guard<std::mutex> g(_hLock);
    _hostedExpireLocked();
    if (_hostedBusyLocked()) {
        sendResult_P(req, 409, "busy", -1, PSTR("Another update is running. Nothing was changed."));
        return;
    }
    _hErr = "";
    _hMsg[0] = '\0';
    _hDone = 0;
    _hTotal = 0;
    _hState.store((uint8_t)HState::Fetching);
    if (!_hostedStartWriter(true)) {
        sendResult(req, 500, _hErr, -1, _hMsg);
        return;
    }
    sendResult_P(req, 200, "ok", -1, PSTR("Downloading the image"));
}

/// Downloads the image for this host's ESP-Hosted version and the
/// co-processor's chip into a PSRAM buffer (_hImg, _hTotal); the progress in
/// _hDone. The TLS is verified against the core's CA bundle: nothing else
/// vouches for an image that is about to run on the co-processor, so an
/// unverified connection (as the core's own example makes) would let anyone
/// in the path supply it. On failure the state and message are set, and
/// nothing is left allocated.
bool AsyncConfigPortal::_hostedFetch() {
    uint32_t a = 0, b = 0, c = 0;
    hostedGetHostVersion(&a, &b, &c);
    char url[sizeof("https://espressif.github.io/arduino-esp32/hosted/-v...bin")
             + HOSTED_TARGET_LEN + HOSTED_VER_LEN];
    snprintf(url, sizeof(url), "https://espressif.github.io/arduino-esp32/hosted/%s-v%lu.%lu.%lu.bin",
             _hostedTarget(true), (unsigned long)a, (unsigned long)b, (unsigned long)c);
    logf(LogLevel::Info, "hosted", "downloading %s", url);

    const uint32_t t0 = millis();
    NetworkClientSecure tls;
    tls.useBuiltinCACertBundle();
    HTTPClient http;
    http.setConnectTimeout(HOSTED_FETCH_TIMEOUT_MS);
    http.setTimeout(HOSTED_FETCH_TIMEOUT_MS);
    char m[sizeof(_hMsg)];
    auto failed = [&](const char* why) {
        http.end();
        _hostedRelease();
        logf(LogLevel::Warn, "hosted", "download failed: %s", why);
        snprintf(m, sizeof(m), "The download failed (%s). Nothing was changed. "
                 "Download the file from the link above and upload it.", why);
        _hostedFail("fetch_failed", m);
        return false;
    };
    if (!http.begin(tls, url)) return failed("bad address");

    const int code = http.GET();
    if (code == HTTP_CODE_NOT_FOUND) {
        http.end();
        snprintf(m, sizeof(m), "Espressif publishes no image for ESP-Hosted %lu.%lu.%lu "
                 "on this chip. Nothing was changed.",
                 (unsigned long)a, (unsigned long)b, (unsigned long)c);
        _hostedFail("no_image", m);
        return false;
    }
    char why[48];
    if (code != HTTP_CODE_OK) {
        // A negative code is HTTPClient's own: no connection, TLS refused, ...
        if (code < 0) snprintf(why, sizeof(why), "%s", HTTPClient::errorToString(code).c_str());
        else          snprintf(why, sizeof(why), "HTTP %d", code);
        return failed(why);
    }
    const int size = http.getSize();
    if (size <= 0) return failed("no length");
    if ((size_t)size > HOSTED_MAX_UPLOAD) return failed("too large");

    _hImg = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!_hImg) return failed("no memory for the image");
    _hCap = size;
    _hTotal = size;

    NetworkClient* in = http.getStreamPtr();
    size_t got = 0;
    uint32_t seen = millis();
    while (got < (size_t)size) {
        const int avail = in ? in->available() : 0;
        if (avail > 0) {
            const size_t want = (size_t)avail < size - got ? (size_t)avail : size - got;
            const int n = in->read(_hImg + got, want);
            if (n > 0) { got += n; _hDone = got; seen = millis(); continue; }
        }
        if (!in || !in->connected()) break;
        if (millis() - seen > HOSTED_IDLE_MS) break;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (got != (size_t)size) {
        snprintf(why, sizeof(why), "stopped at %u of %d bytes", (unsigned)got, size);
        return failed(why);
    }
    http.end();
    _hLen = got;
    logf(LogLevel::Info, "hosted", "downloaded %d bytes in %lu ms (stack left %u)",
         size, (unsigned long)(millis() - t0), (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    return true;
}
#endif  // CONFIG_PORTAL_HOSTED_FETCH

void AsyncConfigPortal::_hostedWriterTask(void* arg) {
    auto* self = static_cast<AsyncConfigPortal*>(arg);
    auto fail = [self](const char* code, const char* msg) {
        self->_hostedRelease();
        self->_hostedFail(code, msg);
        vTaskDelete(nullptr);
    };

    // The user asked for this, so here — and only here — the portal may bring
    // ESP-Hosted up itself: the Wi-Fi may not have started (Ethernet serving).
    if (!hostedIsInitialized() && !hostedInitWiFi())
        return fail("no_coprocessor", "No co-processor answered. This board may not have one. "
                    "Nothing was changed.");

#if CONFIG_PORTAL_HOSTED_FETCH
    if (self->_hFetch) {
        if (!self->_hostedFetch()) { vTaskDelete(nullptr); return; }
        self->_hState.store((uint8_t)HState::Writing);
    }
#endif

    // The chip again, now from the co-processor itself (for a download, the
    // first check). A refusal has set the state and the message already.
    if (!self->_hostedCheckHead(self->_hImg, self->_hTotal, self->_hostedTarget(true))) {
        self->_hostedRelease();
        vTaskDelete(nullptr);
        return;
    }

    uint32_t t0 = millis();
    if (!hostedBeginUpdate())
        return fail("begin_failed", "The co-processor could not start an update. Nothing was changed.");
    const uint32_t beginMs = millis() - t0;

    t0 = millis();
    self->_hDone = 0;   // a download counted it up already
    for (size_t at = 0; at < self->_hTotal; at += HOSTED_BLOCK) {
        const size_t n = (self->_hTotal - at < HOSTED_BLOCK) ? self->_hTotal - at : HOSTED_BLOCK;
        if (!hostedWriteUpdate(self->_hImg + at, n)) {
            self->logf(LogLevel::Error, "hosted", "write failed at %lu of %lu bytes",
                       (unsigned long)at, (unsigned long)self->_hTotal);
            return fail("write_failed", "Writing to the co-processor failed. "
                        "It still runs its previous firmware.");
        }
        self->_hDone = at + n;
    }
    const uint32_t writeMs = millis() - t0;
    if (!hostedEndUpdate())
        return fail("bad_image", "The co-processor refused the image. "
                    "It still runs its previous firmware.");
    self->_hostedRelease();
    self->logf(LogLevel::Info, "hosted", "written and verified: begin %lu ms, writes %lu ms",
               (unsigned long)beginMs, (unsigned long)writeMs);

    // Activation. The page's next poll has to see "activating" before it, so
    // it covers itself: after the activation nothing answers for a while.
    self->_restartScheduled.store(true);
    self->_hState.store((uint8_t)HState::Activating);
    vTaskDelay(pdMS_TO_TICKS(HOSTED_ACTIVATE_DELAY_MS));
    self->logf(LogLevel::Info, "hosted", "activating; the device restarts");
    // Everything that has to happen before the restart happens now: after the
    // activation there may not be time for another line.
    if (self->_onBeforeRestart) self->_onBeforeRestart(RestartReason::Coprocessor);
    if (!hostedActivateUpdate()) {
        self->_restartScheduled.store(false);
        return fail("activate_failed", "The co-processor could not switch to the new image. "
                    "It still runs its previous firmware.");
    }
    vTaskDelay(pdMS_TO_TICKS(HOSTED_RESTART_FALLBACK_MS));
    ESP.restart();   // only if ESP-Hosted did not
}

void AsyncConfigPortal::_registerHostedRoutes() {
    _server.on("/hostedota", HTTP_POST,
        [this](AsyncWebServerRequest* req) { _hostedComplete(req); },
        [this](AsyncWebServerRequest* req, const String& filename, size_t index,
               uint8_t* data, size_t len, bool final) {
            (void)filename;
            if (!originAllowed(req, false)) return;
            if (!requireAuth(req)) return;
            _hostedChunk(req, index, data, len, final);
        });
#if CONFIG_PORTAL_HOSTED_FETCH
    _server.on("/hostedfetch", HTTP_POST,
               [this](AsyncWebServerRequest* req) { _hostedFetchRequest(req); });
#endif
}

#endif  // CONFIG_PORTAL_HOSTED_OTA
