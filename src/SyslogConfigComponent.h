#pragma once

/**
 * @file SyslogConfigComponent.h
 * @brief Optional, opt-in syslog configuration page for AsyncConfigPortal.
 *
 * Edits a `SyslogProfile`: server, port, severity floor, facility. It owns no
 * socket and sends nothing — the application's sender does, and this only
 * changes what the sender reads. Including this header is what pulls in the
 * NetworkProfile family; nothing else in the library does.
 *
 * Usage:
 * @code
 * #include <AsyncConfigPortal.h>
 * #include <SyslogConfigComponent.h>
 *
 * AsyncConfigPortal     web;
 * SyslogProfile         syslogProfile;
 * SyslogConfigComponent syslogPage;
 *
 * void setup() {
 *     syslogProfile.loadCfg("syslog");
 *     web.begin(auth);
 *     syslogPage.setProfile(syslogProfile, "syslog");
 *     syslogPage.onSaved([](const SyslogConfigComponent::Changed& c) {
 *         applySyslog();   // re-read the profile into the sender; see below
 *     });
 *     syslogPage.attach(web);
 * }
 * @endcode
 *
 * **Applying a change.** The component saves and reports; the application
 * re-reads the profile into its sender. Unlike an MQTT client, a syslog sender
 * holds no connection: a new target, port or severity can be applied in place
 * (SyslogSender: `end()`, `begin()`, `setMinSeverity()`, `setEnabled()`), so
 * no restart is called for. `Changed` says which kind of change it was.
 */

#include <atomic>

#include <AsyncConfigPortal.h>
#include <SyslogProfile.h>

// SyslogProfile arrived in NetworkProfile 0.9.0. The profile family is not a
// declared dependency of this library on purpose — see MqttConfigComponent.h —
// so the check lives here.
#if !defined(NETWORK_PROFILE_VERSION) || NETWORK_PROFILE_VERSION < 900
#  error "SyslogConfigComponent needs NetworkProfile 0.9.0 or newer (SyslogProfile)"
#endif

#include "JsonReadUtils.h"
#include "detail/upload_claim.h"

#include "SyslogConfigPages.h"

class SyslogConfigComponent {
public:
    /** @brief What a save changed, so the application can decide what to do. */
    struct Changed {
        /// The server or the port: the sender's target has to be re-resolved.
        bool target  = false;
        /// Severity floor or facility: applied in place, no re-resolution.
        bool filter  = false;
        /// Syslog was switched on or off.
        bool enabled = false;

        /// @return true if anything changed at all.
        bool any() const { return target || filter || enabled; }
    };

    /** @brief Called after a successful save. */
    using SavedFn = std::function<void(const Changed&)>;

    /**
     * @brief Names the profile to edit and where it is stored.
     * @param profile Profile the page reads and writes. Must outlive the portal.
     * @param ns      Preferences namespace saved to; the application loads from
     *                the same one at boot.
     */
    void setProfile(SyslogProfile& profile, const char* ns) {
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
     * @param label      Menu label, and the heading of the Backup section.
     * @param withBackup Register a Backup-page section of its own. Off by
     *                   default, as in the other components.
     * @return false if a profile was not set, or the page registry is full.
     */
    bool attach(AsyncConfigPortal& srv,
                int8_t      order      = AsyncConfigPortal::MENU_NET - 1,
                const char* label      = "Syslog",
                bool        withBackup = false) {
        _srv = &srv;
        if (!_profile) {
            srv.logf(AsyncConfigPortal::LogLevel::Error, "syslog",
                     PSTR("no profile set — call setProfile() before attach()"));
            return false;
        }

        bool ok = srv.addPage("/syslog", label, CONFIG_PORTAL_SYSLOG_HTML,
                              AsyncConfigPortal::AuthLevel::Required, order);

        ok = srv.addJsonEndpoint("/syslogdata",
            [this](char* buf, size_t len) -> bool { return _data(buf, len); },
            AsyncConfigPortal::AuthLevel::Required) && ok;

        ok = srv.addPostHandler("/syslog",
            [this](AsyncWebServerRequest* req) { _save(req); },
            AsyncConfigPortal::AuthLevel::Required) && ok;

        // A factory reset has to reach this namespace too: a wiped device
        // should not come back logging to somebody's server.
        ok = srv.addResetHandler("syslog", [this]() {
            return _profile->clearCfg(_ns);
        }) && ok;

        if (withBackup) {
            ok = srv.addPostHandler("/syslogbackup",
                [this](AsyncWebServerRequest* req) { _backup(req); },
                AsyncConfigPortal::AuthLevel::Required) && ok;

            ok = srv.addUploadHandler("/syslogrestore",
                [this](AsyncWebServerRequest* req) { _restoreDone(req); },
                [this](AsyncWebServerRequest* req, size_t index, uint8_t* data,
                       size_t len, bool final) {
                    _restoreChunk(req, index, data, len, final);
                },
                AsyncConfigPortal::AuthLevel::Required) && ok;

            // Nothing in this profile is a secret, so the restore needs no
            // extra controls: the file carries everything.
            ok = srv.addBackupSection(label, "/syslogbackup", "/syslogrestore",
                nullptr, order) && ok;
        }
        return ok;
    }

private:
    /// Holds the backup document, in either direction.
    static constexpr size_t RESTORE_LEN = Host::MAX_FQDN_SIZE + 96;
    /// How long a stalled upload keeps the buffer before another may take it.
    static constexpr uint32_t RESTORE_IDLE_MS = 10000;

    SyslogProfile*     _profile = nullptr;
    AsyncConfigPortal* _srv     = nullptr;
    const char*        _ns      = nullptr;
    SavedFn            _onSaved;

    /// The form's data. Everything is sent: nothing here is a secret.
    bool _data(char* buf, size_t len) {
        SyslogProfile::SyslogConfig c;
        if (!_profile->getConfig(c)) return false;
        buf[0] = '\0';   // json_cat() appends; the buffer is reused between requests
        bool ok = json_cat_P(buf, PSTR("{\"enabled\":"), len);
        ok = ok && json_cat_P(buf, c.enabled ? PSTR("true") : PSTR("false"), len);
        ok = ok && json_cat_P(buf, PSTR(",\"host\":\""), len);
        ok = ok && json_cat_esc(buf, c.host, len);
        ok = ok && json_cat_P(buf, PSTR("\","), len);
        char n[56];
        ok = ok && json_fitted(snprintf_P(n, sizeof(n),
                                          PSTR("\"port\":%u,\"minSeverity\":%u,\"facility\":%u}"),
                                          (unsigned)c.port, (unsigned)c.minSeverity,
                                          (unsigned)c.facility), sizeof(n));
        ok = ok && json_cat(buf, n, len);
        return ok;
    }

    /// Serves the backup file.
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
        resp->addHeader("Content-Disposition", "attachment; filename=syslog.json");
        req->send(resp);
    }

    void _save(AsyncWebServerRequest* req) {
        SyslogProfile::SyslogConfig c;
        if (!_profile->getConfig(c)) {
            AsyncConfigPortal::sendProgmem(req, 500, "text/html",
                                           CONFIG_PORTAL_SAVE_FAILED_HTML);
            return;
        }
        const SyslogProfile::SyslogConfig before = c;

        // A checkbox absent from the body means unticked — that is the only way
        // a browser reports one.
        c.enabled = req->hasParam("enabled", true);
        _str(req, "host", c.host, sizeof(c.host));
        if (req->hasParam("port", true))
            c.port = (uint16_t)req->getParam("port", true)->value().toInt();
        if (req->hasParam("sev", true))
            c.minSeverity = (uint8_t)req->getParam("sev", true)->value().toInt();
        if (req->hasParam("fac", true))
            c.facility = (uint8_t)req->getParam("fac", true)->value().toInt();

        const SyslogProfile::ConfigCheck cc = _profile->checkConfig(c);
        if (cc != SyslogProfile::ConfigCheck::Ok) {
            char why[CHECK_STR_SIZE];
            _checkStr(cc, why, sizeof(why));
            _srv->logf(AsyncConfigPortal::LogLevel::Warn, "syslog",
                       PSTR("rejected: %s"), why);
            AsyncConfigPortal::sendProgmem(req, 400, "text/html",
                                           CONFIG_PORTAL_SAVE_FAILED_HTML);
            return;
        }
        if (!_profile->setConfig(c) || !_profile->saveCfg(_ns)) {
            _srv->logf(AsyncConfigPortal::LogLevel::Error, "syslog",
                       PSTR("could not be saved"));
            AsyncConfigPortal::sendProgmem(req, 500, "text/html",
                                           CONFIG_PORTAL_SAVE_FAILED_HTML);
            return;
        }

        Changed ch;
        ch.enabled = (c.enabled != before.enabled);
        ch.target  = strcmp(c.host, before.host) != 0 || c.port != before.port;
        ch.filter  = c.minSeverity != before.minSeverity || c.facility != before.facility;
        _srv->logf(AsyncConfigPortal::LogLevel::Info, "syslog",
                   PSTR("saved%s%s%s"),
                   ch.enabled ? ", enabled state changed" : "",
                   ch.target  ? ", target changed" : "",
                   ch.filter  ? ", severity or facility changed" : "");
        AsyncConfigPortal::sendProgmem(req, 200, "text/html",
                                       CONFIG_PORTAL_SAVED_HTML);
        if (_onSaved && ch.any()) _onSaved(ch);
    }

    // ---------------------------------------------------------------- restore ---

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
                PSTR("The file is not a valid syslog backup. Nothing was changed."));
            return;
        }
        SyslogProfile::SyslogConfig c;
        if (!_profile->getConfig(c)) {
            _claim().release(req);
            AsyncConfigPortal::sendProgmemLine(req, 500, "text/plain",
                PSTR("Busy; nothing was changed."));
            return;
        }
        // Absent keys keep the current value: a backup written by an older
        // firmware may not have every field.
        if (jsonHas(doc, "enabled")) {
            char b[8];
            if (jsonVal(doc, "enabled", b, sizeof(b)) > 0) c.enabled = (b[0] == 't');
        }
        jsonVal(doc, "host", c.host, sizeof(c.host));
        jsonNum(doc, "port", c.port);
        jsonNum(doc, "minSeverity", c.minSeverity);
        jsonNum(doc, "facility", c.facility);

        const SyslogProfile::ConfigCheck cc = _profile->checkConfig(c);
        if (cc != SyslogProfile::ConfigCheck::Ok) {
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
        _srv->logf(AsyncConfigPortal::LogLevel::Info, "syslog", PSTR("restored"));
        AsyncConfigPortal::sendProgmem(req, 200, "text/html", CONFIG_PORTAL_SAVED_HTML);
        if (_onSaved) {
            Changed ch; ch.enabled = true; ch.target = true; ch.filter = true;
            _onSaved(ch);           // a restore may have changed anything
        }
    }

    static char* _buffer() { static char b[RESTORE_LEN]; return b; }
    static std::atomic_flag& _busy() {
        static std::atomic_flag f = ATOMIC_FLAG_INIT;
        return f;
    }
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

    static constexpr size_t CHECK_STR_SIZE = 32;
    // Flash texts copied into the caller's buffer — see MqttConfigComponent.
    static void _checkStr(SyslogProfile::ConfigCheck c, char* out, size_t len) {
        PGM_P p = PSTR("invalid");
        switch (c) {
            case SyslogProfile::ConfigCheck::Ok:          p = PSTR("valid"); break;
            case SyslogProfile::ConfigCheck::NoHost:      p = PSTR("no server address"); break;
            case SyslogProfile::ConfigCheck::BadHost:     p = PSTR("malformed server address"); break;
            case SyslogProfile::ConfigCheck::BadPort:     p = PSTR("invalid port"); break;
            case SyslogProfile::ConfigCheck::BadSeverity: p = PSTR("invalid severity"); break;
            case SyslogProfile::ConfigCheck::BadFacility: p = PSTR("invalid facility"); break;
        }
        if (!out || len == 0) return;
        strncpy_P(out, p, len);
        out[len - 1] = '\0';
    }
};
