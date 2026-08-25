#pragma once

/**
 * @file MqttConfigComponent.h
 * @brief Optional, opt-in MQTT broker configuration page for AsyncConfigPortal.
 *
 * Edits an `MqttProfile`: the broker address, port, TLS and credentials. It owns
 * no MQTT client and starts no connection — the application does that, and this
 * only changes what the application reads. Including this header is what pulls
 * in the NetworkProfile family; nothing else in the library does.
 *
 * Usage:
 * @code
 * #include <AsyncConfigPortal.h>
 * #include <MqttConfigComponent.h>
 *
 * AsyncConfigPortal   web;
 * MqttProfile         mqtt;
 * MqttConfigComponent mqttPage;
 *
 * void setup() {
 *     mqtt.loadCfg("mqtt");
 *     web.begin(auth);
 *     mqttPage.setProfile(mqtt, "mqtt");
 *     mqttPage.onSaved([](const MqttConfigComponent::Changed& c) {
 *         if (c.connection) ESP.restart();   // see the note below
 *     });
 *     mqttPage.attach(web);
 * }
 * @endcode
 *
 * @warning **TLS on ESP8266 rarely fits.** BearSSL reserves a ~6 KB secondary
 *          stack the moment a secure client is instantiated, and its connection
 *          buffers are ~22 KB by default — reducible to a few KB only if the
 *          broker agrees to MFLN. A device also running this portal usually has
 *          nowhere near that free, and the failure mode is the bad kind: the
 *          allocation takes the device down, it reboots, reads the same setting,
 *          and goes down again before the portal can answer, so the setting
 *          cannot be undone. An application offering TLS on ESP8266 should
 *          therefore measure the largest free block once the connection stands
 *          and back out if too little is left — staying up without MQTT keeps
 *          the setting reachable. Checking beforehand does not work: the free
 *          heap at startup says nothing about the portal's peak, which comes
 *          when a page is served. The NetConfig example shows the check, and the
 *          three build settings that make TLS fit at all (MFLN, a lowered
 *          HOST_FQDN_LEN, and -D BEARSSL_SSL_BASIC).
 *
 * **Applying a change.** The component saves and reports; what to do about it is
 * the application's call, because the client belongs to the application. A
 * restart is the honest answer for anything the connection is built from — the
 * TLS flag in particular, since a plain and a secured client are different
 * objects, usually created once where the task or loop starts. Reconnecting in
 * place is possible for an address or credential change if the application is
 * structured for it; `Changed` says which kind of change it was.
 */

#include <atomic>

#include <AsyncConfigPortal.h>
#include <MqttProfile.h>
#include "JsonReadUtils.h"
#include "detail/upload_claim.h"

#include "MqttConfigPages.h"

class MqttConfigComponent {
public:
    /** @brief What a save changed, so the application can decide what to do. */
    struct Changed {
        /// The broker, port, TLS or credentials — the connection was built from
        /// these, so it has to be rebuilt to use them.
        bool connection = false;
        /// MQTT was switched on or off.
        bool enabled    = false;

        /// @return true if anything changed at all.
        bool any() const { return connection || enabled; }
    };

    /** @brief Called after a successful save. */
    using SavedFn = std::function<void(const Changed&)>;

    /**
     * @brief Names the profile to edit and where it is stored.
     *
     * @param profile Profile the page reads and writes. Must outlive the portal.
     * @param ns      Preferences namespace saved to; the application loads from
     *                the same one at boot.
     */
    void setProfile(MqttProfile& profile, const char* ns) {
        _profile = &profile;
        _ns      = ns;
    }

    /** @brief Registers a callback invoked after a successful save. */
    void onSaved(SavedFn fn) { _onSaved = fn; }

    /**
     * @brief Registers the page, its data endpoint and its POST handler.
     *
     * @param srv        The portal to attach to.
     * @param order      Menu position; defaults just before the network page.
     * @param label      Menu label, and the heading of the Backup section:
     *                   renaming the page renames both, so they cannot drift.
     * @return false if a profile was not set, or the page registry is full.
     * @param withBackup Register a Backup-page section of its own. Off by
     *                   default, as in NetConfigComponent: the Backup page comes
     *                   into existence with the first section, and a device
     *                   whose settings are not worth carrying between units
     *                   should not acquire one. Separate from
     *                   the network one because the two are separate concerns: a
     *                   broker moves without the addressing changing, and a file
     *                   restored to another device should be able to carry one
     *                   without the other.
     */
    bool attach(AsyncConfigPortal& srv,
                int8_t      order      = AsyncConfigPortal::MENU_NET - 1,
                const char* label      = "MQTT",
                bool        withBackup = false) {
        _srv = &srv;
        if (!_profile) {
            // Registering a page that cannot read or write anything would look
            // like a working feature. Saying so is the smaller surprise.
            srv.logf(AsyncConfigPortal::LogLevel::Error, "mqtt",
                     PSTR("no profile set — call setProfile() before attach()"));
            return false;
        }

        bool ok = srv.addPage("/mqtt", label, CONFIG_PORTAL_MQTT_HTML,
                              AsyncConfigPortal::AuthLevel::Required, order);

        ok = srv.addJsonEndpoint("/mqttdata",
            [this](char* buf, size_t len) -> bool { return _data(buf, len); },
            AsyncConfigPortal::AuthLevel::Required) && ok;

        ok = srv.addPostHandler("/mqtt",
            [this](AsyncWebServerRequest* req) { _save(req); },
            AsyncConfigPortal::AuthLevel::Required) && ok;

        if (withBackup) {
            // POST, not GET: the Backup page submits a form so the download
            // carries the session, and a plain link would not.
            ok = srv.addPostHandler("/mqttbackup",
                [this](AsyncWebServerRequest* req) { _backup(req); },
                AsyncConfigPortal::AuthLevel::Required) && ok;

            ok = srv.addUploadHandler("/mqttrestore",
                [this](AsyncWebServerRequest* req) { _restoreDone(req); },
                [this](AsyncWebServerRequest* req, size_t index, uint8_t* data,
                       size_t len, bool final) {
                    _restoreChunk(req, index, data, len, final);
                },
                AsyncConfigPortal::AuthLevel::Required) && ok;

            // The backup carries no password, so the restore page asks for one.
            // Left empty it keeps whatever is stored, which is what a restore
            // onto the same device wants; moving a file to another device is
            // when it has to be typed.
            ok = srv.addBackupSection(label, "/mqttbackup", "/mqttrestore",
                [this](char* buf, size_t len) -> bool {
                    // Two controls, same scheme as the Wi-Fi restore: the file
                    // carries the username but never the password, and an empty
                    // field alone cannot say which of three things is meant.
                    MqttProfile::MqttConfig c;
                    if (!_profile->getConfig(c)) return false;
                    return snprintf_P(buf, len,
                        PSTR("[{\"name\":\"mqttreq\","
                             "\"label\":\"Broker requires a password\","
                             "\"type\":\"checkbox\",\"checked\":%u},"
                             "{\"name\":\"mqttpass\",\"label\":\"Broker password\","
                             "\"type\":\"password\"}]"),
                        c.password[0] ? 1u : 0u) < (int)len;
                },
                order) && ok;
        }
        return ok;
    }

private:
    /// Holds the backup document, in either direction. Sized from what actually
    /// goes into it — the two variable-length fields plus the fixed keys — since
    /// the same buffer now serves the download, where coming up short would mean
    /// a 500 rather than a truncated file.
    static constexpr size_t RESTORE_LEN =
          Host::MAX_FQDN_SIZE            // host
        + MqttProfile::MAX_USER_SIZE     // user
        + 128;                           // keys, punctuation, port, flags
    /// How long a stalled upload keeps the buffer before another may take it.
    static constexpr uint32_t RESTORE_IDLE_MS = 10000;

    MqttProfile*       _profile = nullptr;
    AsyncConfigPortal* _srv     = nullptr;
    const char*        _ns      = nullptr;
    SavedFn            _onSaved;

    /// The form's data. The password is never sent — only whether one is stored,
    /// so the field can say "(unchanged)" instead of pretending to be empty.
    bool _data(char* buf, size_t len) {
        MqttProfile::MqttConfig c;
        if (!_profile->getConfig(c)) return false;

        // json_cat() appends, so the buffer has to start empty: it is reused
        // between requests, and appending to what was left there produced two
        // documents in a row — valid-looking on the first request and malformed
        // on every one after it.
        buf[0] = '\0';

        bool ok = json_cat_P(buf, PSTR("{\"enabled\":"), len);
        ok = ok && json_cat_P(buf, c.enabled ? PSTR("true") : PSTR("false"), len);
        ok = ok && json_cat_P(buf, PSTR(",\"host\":\""), len);
        ok = ok && json_cat_esc(buf, c.host, len);   // escapes straight into buf
        ok = ok && json_cat_P(buf, PSTR("\","), len);

        char n[32];
        ok = ok && json_fitted(snprintf_P(n, sizeof(n), PSTR("\"port\":%u,\"tls\":"),
                                          (unsigned)c.port), sizeof(n));
        ok = ok && json_cat(buf, n, len);
        ok = ok && json_cat_P(buf, c.tls ? PSTR("true") : PSTR("false"), len);

        ok = ok && json_cat_P(buf, PSTR(",\"user\":\""), len);
        ok = ok && json_cat_esc(buf, c.user, len);
        ok = ok && json_cat_P(buf, PSTR("\",\"hasPassword\":"), len);
        ok = ok && json_cat_P(buf, c.password[0] ? PSTR("true") : PSTR("false"), len);
        ok = ok && json_cat_P(buf, PSTR("}"), len);
        return ok;
    }

    /// Serves the backup file. Reuses the restore buffer — the two never run at
    /// the same time, and one buffer for the page is enough.
    void _backup(AsyncWebServerRequest* req) {
        if (!_claim().claim(req)) {
            AsyncConfigPortal::sendProgmemLine(req, 503, "text/plain",
                PSTR("Busy: another operation is using the buffer. Try again."));
            return;
        }
        char* buf = _buffer();
        if (!_profile->toJson(buf, RESTORE_LEN)) {
            _claim().release(req);
            AsyncConfigPortal::sendProgmemLine(req, 500, "text/plain",
                PSTR("Could not read the configuration."));
            return;
        }
        AsyncWebServerResponse* resp = AsyncConfigPortal::beginStreamed(
            req, buf, "application/json",
            [this, req]() { _claim().release(req); });
        resp->addHeader("Content-Disposition", "attachment; filename=mqtt.json");
        req->send(resp);
    }

    void _save(AsyncWebServerRequest* req) {
        MqttProfile::MqttConfig c;
        if (!_profile->getConfig(c)) {
            AsyncConfigPortal::sendProgmem(req, 500, "text/html",
                                           CONFIG_PORTAL_SAVE_FAILED_HTML);
            return;
        }
        const MqttProfile::MqttConfig before = c;

        // A checkbox absent from the body means unticked — that is the only way
        // a browser reports one, so its absence carries meaning here.
        c.enabled = req->hasParam("enabled", true);
        c.tls     = req->hasParam("tls", true);

        _str(req, "host", c.host, sizeof(c.host));

        // The "requires authentication" box absent means an anonymous
        // connection: both credentials are cleared. A hidden field still
        // submits, so the checkbox — not the field's emptiness — is what says
        // the user meant to drop them.
        if (!req->hasParam("auth", true)) {
            c.user[0]     = '\0';
            c.password[0] = '\0';
        } else {
            _str(req, "user", c.user, sizeof(c.user));
        }
        if (req->hasParam("port", true))
            c.port = (uint16_t)req->getParam("port", true)->value().toInt();

        // An empty password field keeps the stored one: the page never receives
        // it, so an empty field means "not retyped", not "no password". Clearing
        // one is done by clearing the username, which makes the connection
        // anonymous.
        if (req->hasParam("pass", true)) {
            const String& v = req->getParam("pass", true)->value();
            if (v.length()) snprintf(c.password, sizeof(c.password), "%s", v.c_str());
        }
        const MqttProfile::ConfigCheck cc = _profile->checkConfig(c);
        if (cc != MqttProfile::ConfigCheck::Ok) {
            char why[CHECK_STR_SIZE];
            _checkStr(cc, why, sizeof(why));
            _srv->logf(AsyncConfigPortal::LogLevel::Warn, "mqtt",
                       PSTR("rejected: %s"), why);
            AsyncConfigPortal::sendProgmem(req, 400, "text/html",
                                           CONFIG_PORTAL_SAVE_FAILED_HTML);
            return;
        }

        if (!_profile->setConfig(c) || !_profile->saveCfg(_ns)) {
            _srv->logf(AsyncConfigPortal::LogLevel::Error, "mqtt",
                       PSTR("could not be saved"));
            AsyncConfigPortal::sendProgmem(req, 500, "text/html",
                                           CONFIG_PORTAL_SAVE_FAILED_HTML);
            return;
        }

        Changed ch;
        ch.enabled    = (c.enabled != before.enabled);
        ch.connection = strcmp(c.host, before.host) != 0
                     || c.port != before.port
                     || c.tls  != before.tls
                     || strcmp(c.user, before.user) != 0
                     || strcmp(c.password, before.password) != 0;
        _srv->logf(AsyncConfigPortal::LogLevel::Info, "mqtt",
                   PSTR("saved%s%s"),
                   ch.enabled    ? ", enabled state changed" : "",
                   ch.connection ? ", connection settings changed" : "");

        AsyncConfigPortal::sendProgmem(req, 200, "text/html",
                                       CONFIG_PORTAL_SAVED_HTML);
        if (_onSaved && ch.any()) _onSaved(ch);
    }

    // ---------------------------------------------------------------- restore ---

    /**
     * @brief Collects the uploaded file.
     *
     * One upload at a time: the buffer is claimed for the duration. A stalled
     * upload would otherwise hold it for good, so an idle claim can be taken
     * over — the buffer never becomes free, it only changes hands.
     */
    void _restoreChunk(AsyncWebServerRequest* req, size_t index,
                       uint8_t* data, size_t len, bool final) {
        (void)final;
        _claim().collect(req, index, data, len);   // not ours: swallowed
    }

    /// Validates the whole document, then applies it — or applies nothing.
    void _restoreDone(AsyncWebServerRequest* req) {
        if (!_claim().owns(req)) {
            AsyncConfigPortal::sendProgmemLine(req, 409, "text/plain",
                PSTR("This upload was abandoned while another one started. Nothing was "
                     "changed; please try again."));
            return;
        }
        if (_claim().overflowed()) {
            _claim().release(req);
            AsyncConfigPortal::sendProgmemLine(req, 413, "text/plain",
                PSTR("The uploaded file is larger than this firmware can hold. Nothing "
                     "was changed."));
            return;
        }
        if (_claim().size() == 0) {
            _claim().release(req);
            AsyncConfigPortal::sendProgmemLine(req, 400, "text/plain",
                PSTR("No file was uploaded."));
            return;
        }

        JsonSpan doc = jsonRoot(_claim().data(), _claim().size());
        if (!jsonValidate(doc)) {
            _claim().release(req);
            AsyncConfigPortal::sendProgmemLine(req, 400, "text/plain",
                PSTR("The file is not a valid MQTT backup. Nothing was changed."));
            return;
        }

        MqttProfile::MqttConfig c;
        if (!_profile->getConfig(c)) {
            _claim().release(req);
            AsyncConfigPortal::sendProgmemLine(req, 500, "text/plain",
                PSTR("Busy; nothing was changed."));
            return;
        }
        const MqttProfile::MqttConfig before = c;   // to tell a changed username

        // Absent keys keep the current value: a backup written by an older
        // firmware may not have every field, and dropping the rest of the
        // settings over that would be the wrong answer.
        if (jsonHas(doc, "enabled")) {
            char b[8];
            if (jsonVal(doc, "enabled", b, sizeof(b)) > 0) c.enabled = (b[0] == 't');
        }
        if (jsonHas(doc, "tls")) {
            char b[8];
            if (jsonVal(doc, "tls", b, sizeof(b)) > 0) c.tls = (b[0] == 't');
        }
        jsonVal(doc, "host", c.host, sizeof(c.host));
        jsonVal(doc, "user", c.user, sizeof(c.user));
        jsonNum(doc, "port", c.port);

        // The password is never in the file, so the form answers for it. One
        // control cannot: an empty field could mean "no password", "keep the
        // stored one", or "I forgot". The checkbox separates them, exactly as
        // the Wi-Fi restore does:
        //
        //   mqttreq absent   -> the broker wants a username but no password
        //   mqttreq + value  -> that is the password
        //   mqttreq + empty  -> keep the stored one (only if it still applies)
        //
        // "Still applies" matters: a password stored for one username is not
        // the password for another, so a restore that brings a different
        // username has to be given one.
        const bool wantsPassword = req->hasParam("mqttreq", true);
        String typed;
        if (req->hasParam("mqttpass", true)) typed = req->getParam("mqttpass", true)->value();

        if (!wantsPassword) {
            c.password[0] = '\0';
        } else if (typed.length()) {
            snprintf(c.password, sizeof(c.password), "%s", typed.c_str());
        } else {
            const bool sameUser = (strcmp(c.user, before.user) == 0);
            if (!sameUser || before.password[0] == '\0') {
                _claim().release(req);
                AsyncConfigPortal::sendProgmemLine(req, 400, "text/plain",
                    PSTR("This backup uses a username, and the broker password "
                         "is not in the file. Enter it above, or untick the box "
                         "if the broker wants no password. Nothing was changed."));
                return;
            }
            // same username and one is stored: keeping it is a real answer
        }

        // An anonymous connection has no password to keep.
        if (c.user[0] == '\0') c.password[0] = '\0';

        const MqttProfile::ConfigCheck cc = _profile->checkConfig(c);
        if (cc != MqttProfile::ConfigCheck::Ok) {
            char why[CHECK_STR_SIZE];
            _checkStr(cc, why, sizeof(why));
            _claim().release(req);
            req->send(400, "text/plain", why);
            return;
        }
        if (!_profile->setConfig(c) || !_profile->saveCfg(_ns)) {
            _claim().release(req);
            AsyncConfigPortal::sendProgmemLine(req, 500, "text/plain",
                PSTR("Could not be saved. Nothing was changed."));
            return;
        }
        _claim().release(req);
        _srv->logf(AsyncConfigPortal::LogLevel::Info, "mqtt", PSTR("restored"));
        AsyncConfigPortal::sendProgmem(req, 200, "text/html", CONFIG_PORTAL_SAVED_HTML);

        if (_onSaved) {
            Changed ch; ch.enabled = true; ch.connection = true;
            _onSaved(ch);           // a restore may have changed anything
        }
    }

    static char* _buffer() { static char b[RESTORE_LEN]; return b; }
    static std::atomic_flag& _busy() {
        static std::atomic_flag f = ATOMIC_FLAG_INIT;
        return f;
    }
    /// Ownership of that buffer for the duration of one upload or download.
    UploadClaim& _claim() {
        static UploadClaim c(_buffer(), RESTORE_LEN, _busy(), RESTORE_IDLE_MS);
        return c;
    }

    /// Copies a form field if present, leaving the current value otherwise.
    static void _str(AsyncWebServerRequest* req, const char* name,
                     char* dst, size_t len) {
        if (!req->hasParam(name, true)) return;
        snprintf(dst, len, "%s", req->getParam(name, true)->value().c_str());
    }

    /// Longest reason below, plus room.
    static constexpr size_t CHECK_STR_SIZE = 32;

    // Copies into the caller's buffer rather than returning a pointer: the
    // texts live in flash, and on ESP8266 a %s in a format string would read a
    // flash address as if it were RAM. See NetConfigComponent::_checkMsg() for
    // the same reasoning at length.
    //
    // Every enumerator is listed and the fallback is the initialiser rather
    // than a default: label, which would silence -Wswitch and let a value added
    // to MqttProfile::ConfigCheck later arrive unnoticed.
    static void _checkStr(MqttProfile::ConfigCheck c, char* out, size_t len) {
        PGM_P p = PSTR("invalid");
        switch (c) {
            case MqttProfile::ConfigCheck::Ok:      p = PSTR("valid"); break;
            case MqttProfile::ConfigCheck::NoHost:  p = PSTR("no broker address"); break;
            case MqttProfile::ConfigCheck::BadHost: p = PSTR("malformed broker address"); break;
            case MqttProfile::ConfigCheck::BadPort: p = PSTR("invalid port"); break;
            case MqttProfile::ConfigCheck::PasswordWithoutUser:
                                                    p = PSTR("password without a username"); break;
        }
        if (!out || len == 0) return;
        strncpy_P(out, p, len);
        out[len - 1] = '\0';
    }
};
