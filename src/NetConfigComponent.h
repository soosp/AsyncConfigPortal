#pragma once

#ifndef NET_CONFIG_RESTORE_IDLE_MS
// How long an upload may go without a chunk before another may take the buffer.
//
// A client that disappears mid-upload leaves no callback behind: the framework
// destroys the request without a final chunk, so the claim can never rely on
// being released and has to expire as well. Measured from the last chunk rather
// than from the claim, so a slow but healthy upload is never declared dead while
// it is still sending.
#  define NET_CONFIG_RESTORE_IDLE_MS 15000
#endif

#include <Arduino.h>
#include <functional>

#include <atomic>

#include "AsyncConfigPortal.h"
#include "NetConfigPages.h"      // Network + Saved page HTML (+ base pages transitively)
#include "detail/upload_claim.h"
#include "WebFormUtils.h"         // String-free POST helpers
// The component is generic over NetworkProfile (the base): it needs the base
// API plus WiFiProfile for SSID/password on Wi-Fi profiles. EthProfile is NOT
// included — Ethernet profiles are handled through the NetworkProfile base, and
// no ETH-specific member is used here. (Both EthProfile and WiFiProfile derive
// from NetworkProfile.)
#include "NetworkProfile.h"
#include "WiFiProfile.h"

// After NetworkProfile.h, which is what defines NTP_PROFILE_ENABLED.
#if NTP_PROFILE_ENABLED
#  include <NtpProfile.h>
#endif

#include <stdarg.h>
#include "JsonReadUtils.h"

#ifndef NET_CONFIG_MAX_PROFILES
#  define NET_CONFIG_MAX_PROFILES 2
#endif

/**
 * @brief Network configuration page component for AsyncConfigPortal.
 *
 * A pure *configuration* module: it shows what is currently configured (GET
 * /netdata) and stores what the user submits (POST /net). It does NOT touch the
 * live interface, query status, or decide when changes take effect — that is
 * the application's responsibility. After a successful save it fires onSaved()
 * with a NetChangeSet describing which interface types actually changed, so the
 * application can apply the profile, restart, or do nothing, as it sees fit.
 *
 * Ownership / coupling:
 *   - The application registers the NetworkProfile(s) to manage via addProfile().
 *   - The component depends only on NetworkProfile/WiFiProfile and
 *     AsyncConfigPortal — NOT on NetworkManager. Live status (IP, link) belongs
 *     to a separate component if needed.
 *
 * Read-before-write:
 *   On POST, each field is compared against the stored value and only written
 *   if it differs. A stray "Submit" with no actual change therefore reports an
 *   empty NetChangeSet, so the application won't needlessly restart.
 *
 * Field visibility:
 *   The page carries fields for all interface types; the JS hides those not
 *   relevant to each profile's type (reported in /netdata). Wi-Fi profiles show
 *   SSID/password; all show DHCP/IP/mask/gateway/hostname/priority.
 */
class NetConfigComponent {
public:
    // -------------------------------------------------------------------------
    // Buffer budgets
    //
    // *_LEN is the maximum content length excluding the null terminator; *_SIZE
    // is the buffer declaration size, always _LEN + 1.
    // -------------------------------------------------------------------------

    /** @brief Max hostname length. Taken from the profile library rather than
     *  restated, so the two can never disagree (it is configurable there). */
    static constexpr size_t HOSTNAME_LEN  = NetworkProfile::MAX_HOSTNAME_LEN;
    static constexpr size_t HOSTNAME_SIZE = HOSTNAME_LEN + 1;

    /** @brief Max POST field *name* length, e.g. "gateway10". */
    static constexpr size_t FIELD_KEY_LEN  = 15;
    static constexpr size_t FIELD_KEY_SIZE = FIELD_KEY_LEN + 1;

    /** @brief Max POST field *value* length. Sized for the longest value we
     *  accept: a WPA2 passphrase is at most 63 characters. */
    static constexpr size_t FIELD_VAL_LEN  = 63;
    static constexpr size_t FIELD_VAL_SIZE = FIELD_VAL_LEN + 1;

#if NTP_PROFILE_ENABLED
    /** @brief Max NTP entry length. An NTP server may be given as a full FQDN,
     *  which reaches the DNS limit and is far longer than any other field. */
    static constexpr size_t NTP_VAL_LEN  = Host::MAX_FQDN_LEN;
    static constexpr size_t NTP_VAL_SIZE = NTP_VAL_LEN + 1;
#endif

    // -------------------------------------------------------------------------
    // /netdata size requirement
    //
    // Derived from the configured counts rather than guessed, in the same style
    // as NetworkManager::STATUS_JSON_LEN. It has to track NetworkProfile's
    // toJson() output; the terms below name the fields they cover so a change
    // there is easy to mirror. Generous rounding on each term.
    // -------------------------------------------------------------------------

    /** @brief Worst-case JSON length contributed by one profile's "cfg" object.
     *
     * No NTP term. The servers belong to NtpProfile, not to a network profile,
     * so toJson() emits none here; they are budgeted once, as their own object,
     * in REQUIRED_JSON_LEN below. Adding a per-profile term as well would
     * reserve SERVER_COUNT x (MAX_FQDN_LEN + 12) bytes per interface for output
     * that does not exist. */
    static constexpr size_t PROFILE_JSON_LEN =
          96                                        // braces, dhcp, prio, punctuation
        + 3 * 26                                    // ip, mask, gateway
        + NetworkProfile::DNS_SERVER_COUNT * 26     // dnsN
        + NetworkProfile::MAX_HOSTNAME_LEN + 12     // host
        + 28                                        // mac
        ;

    /** @brief Length of the per-interface TX-power JSON: the discrete-levels
     *  array on ESP32, or the {min,max,step} range on ESP8266. */
#if defined(ARDUINO_ARCH_ESP32)
    static constexpr size_t TXLIM_LEN = 8 * WiFiProfile::WIFI_TX_POWER_LEVEL_COUNT + 20;
#else
    static constexpr size_t TXLIM_LEN = 48;   // ,"txrange":{"min":..,"max":..,"step":..}
#endif

    /** @brief Extra length a Wi-Fi profile adds. The SSID appears exactly once,
     *  inside "cfg", where NetworkProfile escapes it; JSON escaping can expand
     *  each byte six-fold. A second, unescaped copy would produce invalid JSON
     *  for an SSID containing a quote, so there is none. */
    static constexpr size_t WIFI_EXTRA_LEN = (6 * MAX_SSID_LEN + 12)
                                           + 24     // txpwr
                                           + TXLIM_LEN
                                           + 20;    // secured

    /** @brief Worst-case length of the whole /netdata document.
     *
     * Reduce it by lowering NET_CONFIG_MAX_PROFILES, NTP_PROFILE_SERVER_COUNT
     * or NETWORK_PROFILE_DNS_SERVER_COUNT to what the project actually uses. */
    static constexpr size_t REQUIRED_JSON_LEN =
        16                                        // {"profiles":[ ... ]}
        + NET_CONFIG_MAX_PROFILES * (56 + PROFILE_JSON_LEN + WIFI_EXTRA_LEN)
#if NTP_PROFILE_ENABLED
        // ,"ntp":{"ntp0":"<escaped>",...}
        + 10 + NtpProfile::SERVER_COUNT * (NTP_VAL_LEN + 12)
#endif
        ;

    /** @brief Buffer size for the /netdata document, including the terminator. */
    static constexpr size_t REQUIRED_JSON_SIZE = REQUIRED_JSON_LEN + 1;

    // -------------------------------------------------------------------------
    // Change set reported to the application after a save
    // -------------------------------------------------------------------------

    /**
     * @brief Which interface types actually changed in a POST (type-based, so
     *        it stays valid if new interface types are added later).
     */
    class NetChangeSet {
    public:
    #if NTP_PROFILE_ENABLED
        /// The device-level NTP servers changed. Separate from the interface
        /// flags because they are not one interface's setting: the manager
        /// applies them on the next connect, whichever interface that is.
        bool ntp = false;
        void markNtp() { ntp = true; }
#endif

        void mark(NetworkProfile::InterfaceType t) {
            _mask |= _bit(t);
        }
        bool changed(NetworkProfile::InterfaceType t) const {
            return (_mask & _bit(t)) != 0;
        }
        bool any() const { return _mask != 0
#if NTP_PROFILE_ENABLED
                || ntp
#endif
            ;
        }

    private:
        static uint8_t _bit(NetworkProfile::InterfaceType t) {
            return static_cast<uint8_t>(1u << static_cast<uint8_t>(t));
        }
        uint8_t _mask = 0;
    };

    /** @brief Fired once after a POST, with the set of types that changed. */
    using SavedCb = std::function<void(const NetChangeSet& changed)>;

        /**
     * @brief Outcome of the Wi-Fi credential check on a restore.
     */
    enum class CredentialGate : uint8_t {
        Ok,            ///< Safe to apply
        NeedPassword,  ///< Would leave the device unable to join; refuse
    };

    /**
     * @brief Decides whether a restore may proceed without a supplied password.
     *
     * Pure decision logic — no I/O, no state — in the spirit of
     * NetworkManagerCore, so the rule can be pinned down by a host test rather
     * than inferred from behaviour on hardware.
     *
     * The rule, and why it is this and not "always ask":
     *
     * | SSID in backup vs stored | password supplied | result        |
     * |--------------------------|-------------------|---------------|
     * | same                     | either            | Ok            |
     * | different                | no                | NeedPassword  |
     * | different                | yes (empty too)   | Ok            |
     *
     * When the SSID is unchanged the stored password is still the right one, so
     * nothing needs to be typed. When it changes, the stored password belongs to
     * a different network and applying the restore would leave a Wi-Fi-only
     * device with no way back — NetworkManager's fallback is between
     * *interfaces*, so a single-interface device has nothing to fall back to.
     *
     * A supplied *empty* password is a valid answer: it means the target is an
     * open network. That is why the caller must distinguish "absent" from
     * "empty" rather than testing for a non-empty string.
     *
     * @param ssidChanged      The backup's SSID differs from the stored one.
     * @param passwordSupplied A password parameter was present in the request,
     *                         regardless of whether it was empty.
     */
    static CredentialGate wifiCredentialGate(bool ssidChanged,
                                             bool passwordSupplied) {
        return (ssidChanged && !passwordSupplied) ? CredentialGate::NeedPassword
                                                  : CredentialGate::Ok;
    }

    /**
     * @brief Restores the network configuration from a backup fragment.
     *
     * The restore counterpart of the /net form path. It consumes the same
     * document shape the component emits at /netdata — one serialiser, so a
     * backup can be diffed against a live device — and ignores the presentation
     * fields (`idx`, `txmin`, `txmax`, `txstep`, `dnscnt`) that travel with it.
     *
     * Two phases, deliberately:
     *
     *   1. Every entry is matched to a registered profile, patched into a staged
     *      config, validated with checkConfig(), and put through the
     *      credential gate. Nothing is written.
     *   2. Only if every entry passed does anything get applied and persisted.
     *
     * This is the cross-profile atomicity the form path still lacks (validation
     * there is per profile, so an earlier interface may already be applied when
     * a later one is rejected). A restore is a rarer, higher-stakes operation
     * than a form save, so it gets the stronger guarantee from the start.
     *
     * Entries are matched by interface `type`, not by position: a backup taken
     * from a differently-ordered device still lands correctly. A type that this
     * device does not have, or that appears twice, fails the whole restore —
     * silently applying half a configuration is worse than refusing it.
     *
     * @param doc    Span covering the array of profile entries.
     * @param req    The request, used to read the supplied Wi-Fi password
     *               (`wifipass<idx>`); presence is what matters, not content.
     * @param err    Filled with a specific, user-facing reason on failure.
     * @param errLen Size of @p err.
     * @return true if the whole document was applied.
     */
    bool loadFromJson(JsonSpan doc, AsyncWebServerRequest* req,
                      char* err, size_t errLen) {
        if (errLen) err[0] = '\0';

        // The document is an object now: the profiles array beside the
        // device-level NTP servers, rather than the servers repeated inside
        // every profile.
        const JsonSpan ntpDoc  = jsonMember(doc, "ntp");   // read before doc moves
        const JsonSpan profiles = jsonMember(doc, "profiles");
        if (!profiles) {
            _err(err, errLen, "the backup is not a network backup file");
            return false;
        }
        doc = profiles;

        const size_t n = jsonCount(doc);
        if (n == 0) {
            _err(err, errLen, "the backup contains no network profiles");
            return false;
        }
        if (n > NET_CONFIG_MAX_PROFILES) {
            _err(err, errLen,
                "the backup holds %u profiles; this firmware supports %u",
                (unsigned)n, (unsigned)NET_CONFIG_MAX_PROFILES);
            return false;
        }

        Staged* st = _staged();
        for (uint8_t i = 0; i < NET_CONFIG_MAX_PROFILES; i++) st[i] = Staged();

        // -------------------------------------------------------------------------
        // Phase 1 — build and validate everything. Nothing is written.
        // -------------------------------------------------------------------------
        for (size_t i = 0; i < n; i++) {
            JsonSpan e = jsonElement(doc, i);

            uint8_t type = 0;
            if (jsonNum(e, "type", type) != 0) {
                _err(err, errLen, "backup entry %u has no valid interface type",
                    (unsigned)i);
                return false;
            }

            const int slot =
                _findByType(static_cast<NetworkProfile::InterfaceType>(type));
            if (slot < 0) {
                // Refused rather than skipped: applying the half of a backup this
                // device understands would leave a configuration nobody chose.
                _err(err, errLen,
                    "the backup contains an interface type (%u) that this device "
                    "does not have", (unsigned)type);
                return false;
            }
            if (st[slot].used) {
                _err(err, errLen,
                    "the backup contains two entries for the same interface type");
                return false;
            }

            JsonSpan c = jsonMember(e, "cfg");
            if (!c) {
                _err(err, errLen, "backup entry %u has no \"cfg\" object",
                    (unsigned)i);
                return false;
            }

            NetworkProfile* p = _entries[slot].profile;
            bool failed = false;
            bool changed = false;

            if (p->getInterfaceType() == NetworkProfile::InterfaceType::WIFI) {
                WiFiProfile* w = static_cast<WiFiProfile*>(p);
                if (!w->getConfig(st[slot].cfg)) {
                    _err(err, errLen, "could not read the current Wi-Fi profile");
                    return false;
                }
                changed = _patchCommonJson(c, st[slot].cfg, err, errLen, failed);
                if (_patchWifiJson(c, (uint8_t)slot, req, st[slot].cfg,
                                err, errLen, failed)) {
                    changed = true;
                }
                if (failed) return false;
                {
                    NetworkProfile::ConfigCheck cc =
                        WiFiProfile::checkConfig(st[slot].cfg);
                    if (cc != NetworkProfile::ConfigCheck::Ok) {
                        char why[CHECK_MSG_SIZE];
                        _checkMsg(cc, why, sizeof(why));
                        _err(err, errLen,
                            "the Wi-Fi configuration in the backup is not valid: %s",
                            why);
                        return false;
                    }
                }
            } else {
                // checkConfig() is static, so a call through a base pointer would
                // pick the base overload at compile time. The branch-and-cast is
                // mandatory, exactly as in _applyProfile().
                NetworkProfile::NetworkConfig& base = st[slot].cfg;
                if (!p->getConfig(base)) {
                    _err(err, errLen, "could not read the current network profile");
                    return false;
                }
                changed = _patchCommonJson(c, base, err, errLen, failed);
                if (failed) return false;
                {
                    NetworkProfile::ConfigCheck cc = NetworkProfile::checkConfig(base);
                    if (cc != NetworkProfile::ConfigCheck::Ok) {
                        char why[CHECK_MSG_SIZE];
                        _checkMsg(cc, why, sizeof(why));
                        _err(err, errLen,
                            "the network configuration in the backup is not valid: %s",
                            why);
                        return false;
                    }
                }
            }

            st[slot].used    = true;
            st[slot].slot    = (uint8_t)slot;
            st[slot].changed = changed;
        }

        // -------------------------------------------------------------------------
        // Phase 2 — apply. Every entry has already been validated, so the only
        // remaining failure mode is a profile mutex timeout.
        //
        // Honest limitation: that residue is not atomic. Validation removes the
        // *validation* class of partial application, which is the one a bad backup
        // can trigger; a mutex timeout half-way through can still leave an earlier
        // profile applied. Closing that too would need a two-phase commit inside
        // NetworkProfile, which is a profile-library change.
        // -------------------------------------------------------------------------
        NetChangeSet changed;
        for (uint8_t i = 0; i < _count; i++) {
            if (!st[i].used || !st[i].changed) continue;

            NetworkProfile* p = _entries[i].profile;
            const bool ok =
                (p->getInterfaceType() == NetworkProfile::InterfaceType::WIFI)
                    ? static_cast<WiFiProfile*>(p)->setConfig(st[i].cfg)
                    : p->setConfig(static_cast<NetworkProfile::NetworkConfig&>(
                        st[i].cfg));
            if (!ok) {
                _err(err, errLen,
                    "applying the restored configuration failed part-way; the "
                    "device may hold a mixture of old and new settings");
                return false;
            }
            p->saveCfg(_entries[i].ns);
            changed.mark(p->getInterfaceType());
        }

        // Fired even with an empty change set, matching _handlePost(): consumers
        // rely on being told that an apply happened but changed nothing.
        if (_onSaved) _onSaved(changed);
#if NTP_PROFILE_ENABLED
        // The NTP servers, restored once for the device. An older file without
        // them leaves the stored ones alone rather than clearing them: absence
        // in a backup means "not carried", not "empty".
        if (_ntp && ntpDoc) {
            NtpProfile::NtpConfig ncfg;
            if (_ntp->getConfig(ncfg)) {
                bool touched = false;
                for (uint8_t t = 0; t < NtpProfile::SERVER_COUNT; t++) {
                    char key[8], val[NTP_VAL_SIZE];
                    snprintf_P(key, sizeof(key), PSTR("ntp%u"), t);
                    const int r = jsonVal(ntpDoc, key, val, sizeof(val));
                    if (r == JSON_INVALID) {
                        _err(err, errLen, "NTP server %u in the backup is invalid", t);
                        return false;
                    }
                    if (r >= 0 && strcmp(val, ncfg.server[t]) != 0) {
                        snprintf_P(ncfg.server[t], sizeof(ncfg.server[t]),
                                   PSTR("%s"), val);
                        touched = true;
                    }
                }
                if (touched) {
                    if (_ntp->checkConfig(ncfg) != NtpProfile::ConfigCheck::Ok) {
                        _err(err, errLen, "an NTP server in the backup is not valid");
                        return false;
                    }
                    if (!_ntp->setConfig(ncfg) || !_ntp->saveCfg(_ntpNs)) {
                        _err(err, errLen, "the NTP servers could not be saved");
                        return false;
                    }
                }
            }
        }
#endif

        return true;
    }

    // -------------------------------------------------------------------------
    // Setup
    // -------------------------------------------------------------------------

    NetConfigComponent() = default;

    /**
     * @brief Registers a profile for the component to manage.
     *
     * The application registers exactly the interfaces it uses; the component
     * does not discover them. Order determines display order.
     *
     * @param profile Profile to manage. Must outlive this component.
     * @param ns      NVS namespace used for saveCfg() on this profile.
     * @return false if the registry is full — the profile was NOT registered,
     *         and will appear on no page and in no backup. Raise
     *         NET_CONFIG_MAX_PROFILES. Ignoring the result is safe: attach()
     *         reports the loss through the server's log.
     */
#if NTP_PROFILE_ENABLED
    /**
     * @brief Names the NTP profile the Time group edits.
     *
     * The servers are a device-level setting — one clock, one set of servers —
     * so they are edited once here rather than repeated in every interface. Not
     * calling this simply leaves the group off the page.
     *
     * Exactly one profile, which is the ordinary case: a device synchronises
     * from the same servers whichever interface carries the traffic.
     * NetworkProfile also allows a profile per interface, bound with
     * NetworkAdapter::setNtpProfile() — for a wired LAN with an internal time
     * server that a fallback cannot reach, say. That is a speciality, and this
     * page does not edit it: bind those in the sketch and leave this unset, or
     * hand it the device-level profile and accept that the per-interface ones
     * stay as the sketch left them.
     *
     * They are saved and restored with the rest of this page, since that is what
     * the file means to whoever downloads it: the settings this page shows.
     *
     * @param profile Must outlive the portal.
     * @param ns      Preferences namespace to save to.
     */
    void setNtpProfile(NtpProfile& profile, const char* ns) {
        _ntp   = &profile;
        _ntpNs = ns;
    }
#endif

    bool addProfile(NetworkProfile& profile, const char* ns) {
        if (_count >= NET_CONFIG_MAX_PROFILES) {
            if (_dropped < 255) _dropped++;
            return false;
        }
        _entries[_count].profile = &profile;
        _entries[_count].ns      = ns;
        _count++;
        return true;
    }

    /** @brief Sets the callback fired after a successful save. */
    void onSaved(SavedCb cb) { _onSaved = std::move(cb); }

    /**
     * @brief Registers the /net page and its REST endpoints on the server.
     *
     * Adds the page (auth-required), the /netdata GET (current config), and the
     * /net POST (save). Call from the subclass's registerRoutes().
     *
     * @param srv   The config web server.
     * @param order Menu weight (default MENU_NET so it sits near the end).
     * @param label      Menu label, and the heading of the Backup section:
     *                   renaming the page renames both, so they cannot drift.
     */
    bool attach(AsyncConfigPortal& srv,
                int8_t      order      = AsyncConfigPortal::MENU_NET,
                const char* label      = "Network",
                bool        withBackup = false) {

        _srv = &srv;
 
        // Registration happens before attach(), so a dropped profile has had
        // nowhere to be reported until now.
        if (_dropped) {
            _logf(AsyncConfigPortal::LogLevel::Error,
                  PSTR("%u profile(s) not registered: NET_CONFIG_MAX_PROFILES is %u"),
                  (unsigned)_dropped, (unsigned)NET_CONFIG_MAX_PROFILES);
        }

        bool registered = srv.addPage("/net", label, CONFIG_PORTAL_NET_HTML,
                              AsyncConfigPortal::AuthLevel::Required, order);

        // Served from this component's own buffer rather than the server's
        // shared one. /netdata is the largest document in the library and its
        // size follows the consumer's profile, NTP and DNS counts, so it is
        // sized here, next to the computation — the alternative made the size
        // depend on a macro that does not reach every translation unit under the
        // Arduino IDE, which could leave the check and the buffer disagreeing.
        registered = srv.addJsonEndpoint("/netdata",
            [this]() -> const char* {
                char* buf = _netDataBuffer();
                // Same single-task invariant as the server's shared buffer; the
                // claim turns a broken invariant into a visible error rather
                // than a race. See AsyncConfigPortal.cpp for the reasoning.
                if (_netDataBusy().test_and_set(std::memory_order_acquire)) {
                    _logf(AsyncConfigPortal::LogLevel::Error,
                          PSTR("/netdata buffer contended — the single-task "
                          "invariant does not hold on this build"));
                    return nullptr;                       // contended: nothing to release
                }
                bool ok = _buildNetData(buf, REQUIRED_JSON_SIZE);
                if (!ok) { _netDataBusy().clear(std::memory_order_release); return nullptr; }
                return buf;   // flag stays held; the framework releases it on send completion
            },
            []() { _netDataBusy().clear(std::memory_order_release); },
            AsyncConfigPortal::AuthLevel::Required) && registered;

        registered = srv.addPostHandler("/net",
            [this](AsyncWebServerRequest* req) { _handlePost(req); },
            AsyncConfigPortal::AuthLevel::Required) && registered;

        registered = srv.addResetHandler("network", [this]() { return _factoryReset(); }) && registered;

        if (!withBackup) return registered;

        registered = srv.addPostHandler("/netbackup",
            [this](AsyncWebServerRequest* req) { _handleBackup(req); },
            AsyncConfigPortal::AuthLevel::Required) && registered;

        registered = srv.addUploadHandler("/netrestore",
            [this](AsyncWebServerRequest* req) { _handleRestoreDone(req); },
            [this](AsyncWebServerRequest* req, size_t index, uint8_t* data,
                   size_t len, bool final) {
                _handleRestoreChunk(req, index, data, len, final);
            },
            AsyncConfigPortal::AuthLevel::Required) && registered;

        registered = srv.addBackupSection(label, "/netbackup", "/netrestore",
            [this](char* buf, size_t len) { return _restoreFields(buf, len); },
            order) && registered;
        return registered;
    }

private:
    struct Entry {
        NetworkProfile* profile = nullptr;
        const char*     ns      = nullptr;
    };

    // -------------------------------------------------------------------------
    // GET /netdata — array of { "type":..., <profile fields> }
    // -------------------------------------------------------------------------

    bool _buildNetData(char* buf, size_t len) {
        size_t n = 0;
        // An object, not a bare array: the NTP servers sit beside the profiles
        // rather than inside each of them, which is the whole point of moving
        // them to a device-level profile.
        n += snprintf_P(buf + n, len - n, PSTR("{\"profiles\":["));
        for (uint8_t i = 0; i < _count && n < len; i++) {
            NetworkProfile* p = _entries[i].profile;

            static char pj[NetworkProfile::JSON_SIZE];  // static: 1.2 KB off the tiny ESP8266 stack (single-task + _netDataBusy guard it)
                        if (!p->toJson(pj, sizeof(pj))) {
                _logf(AsyncConfigPortal::LogLevel::Error,
                      PSTR("profile %u could not be serialised; /netdata unavailable"),
                      (unsigned)i);
                return false;
            }

            // Wrap each profile's JSON with its interface type and, for Wi-Fi,
            // the presentation metadata the page cannot derive on its own. The
            // password is intentionally NOT sent back; the SSID is not repeated
            // here either, since "cfg" already carries an escaped copy.
            uint8_t type = static_cast<uint8_t>(p->getInterfaceType());

            // TX-power limits travel with the interface because they are
            // platform dependent (ESP32 -1..19.5 dBm, ESP8266 0..20.5), and the
            // page is static PROGMEM that cannot know which it is running on.
            char txlim[TXLIM_LEN] = {0};

            // "secured" drives the initial state of the password toggle. It is
            // one bit about the *shape* of the credential, never the credential
            // itself, and getSecurity() keeps the password inside the profile —
            // the passphrase stays write-only through the web interface.
            //
            // UNKNOWN means the profile mutex could not be taken, which is not
            // an answer: reported as a failure rather than folded into either
            // state, since guessing "open" would be a lie about security and
            // guessing "secured" would hide an open network.
            char secured[20] = {0};

            if (p->getInterfaceType() == NetworkProfile::InterfaceType::WIFI) {
                WiFiProfile* w = static_cast<WiFiProfile*>(p);
                // TX power is a discrete set on ESP32; emit the actual levels
                // (dBm) so the page offers a dropdown of real values instead of
                // a free number that could land between levels.
#if defined(ARDUINO_ARCH_ESP32)
                // ESP32: a discrete set of levels; emit the actual values (dBm)
                // so the page offers a dropdown of real values instead of a free
                // number that could land between levels.
                int off = snprintf_P(txlim, sizeof(txlim), PSTR(",\"txlevels\":["));
                for (uint8_t li = 0; li < WiFiProfile::WIFI_TX_POWER_LEVEL_COUNT; li++) {
                    off += snprintf_P(txlim + off, sizeof(txlim) - off, PSTR("%s%.2f"),
                                    li ? "," : "",
                                    (double)WiFiProfile::WIFI_TX_POWER_LEVELS_Q[li]
                                        / WiFiProfile::WIFI_TX_POWER_MULTIPLIER);
                }
                snprintf_P(txlim + off, sizeof(txlim) - off, PSTR("]"));
#elif defined(ARDUINO_ARCH_ESP8266)
                // ESP8266: a continuous range; emit {min,max,step} so the page
                // offers a bounded number input.
                snprintf_P(txlim, sizeof(txlim),
                         PSTR(",\"txrange\":{\"min\":%.2f,\"max\":%.2f,\"step\":%.2f}"),
                         (double)WiFiProfile::MIN_WIFI_TX_POWER_dBm,
                         (double)WiFiProfile::MAX_WIFI_TX_POWER_dBm,
                         (double)WiFiProfile::WIFI_TX_POWER_STEP_dBm);
#endif

                const WiFiProfile::WiFiSecurity sec = w->getSecurity();
                if (sec == WiFiProfile::WiFiSecurity::UNKNOWN) {
                    _logf(AsyncConfigPortal::LogLevel::Warn,
                          PSTR("profile %u: Wi-Fi security unreadable (mutex timeout)"),
                          (unsigned)i);
                    return false;
                }
                snprintf_P(secured, sizeof(secured), PSTR(",\"secured\":%u"),
                         (sec == WiFiProfile::WiFiSecurity::PASSWORD) ? 1u : 0u);
            }

            // dnscnt travels with the entry because the profile only serialises
            // its dnsN keys while DHCP is off. Building the DNS rows from the
            // keys present would therefore render none at all under DHCP, and
            // un-ticking the box could not create rows that were never there —
            // the fields only appeared after a reboot. The slot count is a
            // firmware constant, so it is always available.
            n += snprintf_P(buf + n, len - n,
                PSTR("%s{\"idx\":%u,\"type\":%u%s%s,\"dnscnt\":%u,\"cfg\":%s}"),
                (i == 0) ? "" : ",", i, type, secured, txlim,
                (unsigned)NetworkProfile::DNS_SERVER_COUNT, pj);
        }

        if (n < len) n += snprintf_P(buf + n, len - n, PSTR("]"));

#if NTP_PROFILE_ENABLED
        if (_ntp && n < len) {
            n += snprintf_P(buf + n, len - n, PSTR(",\"ntp\":"));
            if (n < len && !_ntp->toJson(buf + n, len - n)) return false;
            n += strlen(buf + n);
        }
#endif
        if (n < len) n += snprintf_P(buf + n, len - n, PSTR("}"));
        if (n >= len) {
            _logf(AsyncConfigPortal::LogLevel::Error,
                  PSTR("/netdata needs %u bytes but has %u; raise "
                  "NET_CONFIG_MAX_PROFILES, DNS or NTP counts to match, or the "
                  "page will stay empty"),
                  (unsigned)n + 1, (unsigned)len);
            return false;
        }
        return true;

    }

    /**
     * @brief Erases every managed profile's persisted configuration.
     *
     * Goes through NetworkProfile::clearCfg(), the symmetric partner of the
     * saveCfg() already used to persist: the component never touches NVS
     * itself, so the profile stays the only thing that knows how it is stored.
     *
     * Every namespace is attempted even after one fails, the NTP one included.
     * Stopping early would leave a device that is neither configured nor
     * default, which is harder to reason about than one where the failure is
     * named in the log.
     */
    bool _factoryReset() {
        bool ok = true;
        for (uint8_t i = 0; i < _count; i++) {
            if (_entries[i].profile->clearCfg(_entries[i].ns)) continue;
            ok = false;
            _logf(AsyncConfigPortal::LogLevel::Error,
                  PSTR("profile %u: could not erase namespace \"%s\""),
                  (unsigned)i, _entries[i].ns);
        }
#if NTP_PROFILE_ENABLED
        // The Time group saves into its own namespace, so erasing the interface
        // profiles alone would leave a reset device still synchronising from
        // whatever servers it was given.
        if (_ntp && _ntpNs && !_ntp->clearCfg(_ntpNs)) {
            ok = false;
            _logf(AsyncConfigPortal::LogLevel::Error,
                  PSTR("could not erase NTP namespace \"%s\""), _ntpNs);
        }
#endif
        return ok;
    }

    // -------------------------------------------------------------------------
    // POST /net — read-before-write per field, per profile
    // -------------------------------------------------------------------------

    void _handlePost(AsyncWebServerRequest* req) {
        NetChangeSet changed;
        bool rejected = false;

        // Per-interface fields. The device-level hostname is patched inside each
        // profile's config (it is one form field applied to every profile), so
        // it takes part in the same atomic write as everything else.
        for (uint8_t i = 0; i < _count; i++) {
            bool failed = false;
            if (_applyProfile(req, i, _entries[i], failed)) {
                changed.mark(_entries[i].profile->getInterfaceType());
            }
            if (failed) rejected = true;
        }

#if NTP_PROFILE_ENABLED
        // The NTP servers are one device-level setting, so they are read once
        // here rather than per interface. Staged and checked before being kept,
        // the same way a profile is.
        //
        // An empty field clears its slot, where an empty address field means
        // "unchanged". The difference is deliberate: the manager asks DHCP for a
        // time server only when every slot is empty, so emptying them is how the
        // page says "take what the network offers" — and a page that could not
        // express it would strand a device on servers it can no longer reach.
        //
        // val is NTP_VAL_SIZE rather than the shared FIELD_VAL_LEN because an
        // FQDN is far longer than any other field on this page.
        if (_ntp) {
            NtpProfile::NtpConfig ncfg;
            if (_ntp->getConfig(ncfg)) {
                bool ntpChanged = false;
                for (uint8_t t = 0; t < NtpProfile::SERVER_COUNT; t++) {
                    char key[8], val[NTP_VAL_SIZE];
                    snprintf_P(key, sizeof(key), PSTR("ntp%u"), t);
                    if (postVal(req, key, val, sizeof(val)) >= 0
                        && strcmp(val, ncfg.server[t]) != 0) {
                        snprintf_P(ncfg.server[t], sizeof(ncfg.server[t]),
                                   PSTR("%s"), val);
                        ntpChanged = true;
                    }
                }
                if (ntpChanged) {
                    if (_ntp->checkConfig(ncfg) != NtpProfile::ConfigCheck::Ok) {
                        _logf(AsyncConfigPortal::LogLevel::Warn,
                              PSTR("an NTP server address was rejected"));
                        rejected = true;
                    } else if (_ntp->setConfig(ncfg) && _ntp->saveCfg(_ntpNs)) {
                        changed.markNtp();
                    } else {
                        _logf(AsyncConfigPortal::LogLevel::Error,
                              PSTR("NTP servers could not be saved"));
                        rejected = true;
                    }
                }
            }
        }
#endif

        // Persist every profile whose type is marked as changed. A profile that
        // was rejected is never marked, so it is not persisted either.
        for (uint8_t i = 0; i < _count; i++) {
            if (changed.changed(_entries[i].profile->getInterfaceType())) {
                _entries[i].profile->saveCfg(_entries[i].ns);
            }
        }

        // Always invoked, including with an empty change set: consumers rely on
        // being told that a submit happened but changed nothing.
        if (_onSaved) _onSaved(changed);

        if (rejected) {
            // Validation happens per profile, so with several interfaces an
            // earlier one may already have been applied; say so rather than
            // promising a clean rollback.
            AsyncConfigPortal::sendProgmem(req, 400, "text/html", CONFIG_PORTAL_SAVE_FAILED_HTML);
            return;
        }

        // The component only reports "saved"; the application decides whether
        // to apply live or restart. The page shows a generic confirmation.
        AsyncConfigPortal::sendProgmem(req, 200, "text/html", CONFIG_PORTAL_SAVED_HTML);
    }

    // Storage for /netdata. Function-local statics rather than data members:
    // the component is header-only, so this keeps the definition in the header
    // without needing an inline variable, and one buffer is enough because the
    // handlers are serialised on the AsyncTCP task.
    static char* _netDataBuffer() {
        static char buf[REQUIRED_JSON_SIZE];
        return buf;
    }
    static std::atomic_flag& _netDataBusy() {
        static std::atomic_flag busy = ATOMIC_FLAG_INIT;
        return busy;
    }

    // Applies the POSTed fields for profile #idx.
    //
    // Read-modify-write against a whole config struct rather than field-by-field
    // setters. Three reasons: the profile mutex is taken once instead of once per
    // field; setConfig() validates the complete configuration *before* touching
    // anything, so an invalid field cannot leave a half-updated profile; and a
    // mutex timeout surfaces as a failure rather than being reported as a
    // successful change.
    //
    // Fields absent from the POST (a hidden priority, a blank password, static
    // address fields hidden while DHCP is on) simply keep the value read back
    // from the profile.
    //
    // @param failed set when the profile could not be read or the new config was
    //        rejected; the caller reports this to the user.
    // @return true if something actually changed and was applied.
    bool _applyProfile(AsyncWebServerRequest* req, uint8_t idx, Entry& e,
                       bool& failed) {
        NetworkProfile* p = e.profile;

        if (p->getInterfaceType() == NetworkProfile::InterfaceType::WIFI) {
            WiFiProfile* w = static_cast<WiFiProfile*>(p);
            WiFiProfile::WiFiConfig cfg;
            if (!w->getConfig(cfg)) {
                _logf(AsyncConfigPortal::LogLevel::Error,
                      PSTR("profile %u: getConfig failed (mutex timeout); "
                      "configuration not saved"), (unsigned)idx);
                failed = true;
                return false;
            }

            bool changed = _patchCommon(req, idx, cfg, failed);
            if (_patchWifi(req, idx, cfg, failed)) changed = true;
            // A rejected field rejects the whole profile: applying the rest
            // would store a configuration the user did not submit.
            if (failed || !changed) return false;

            // Authoritative gate, with the specific reason for the log. setConfig()
            // re-checks, but only names yes/no; checking here turns "invalid" into
            // an actionable line instead of the busy path below.
            NetworkProfile::ConfigCheck cc = WiFiProfile::checkConfig(cfg);
            if (cc != NetworkProfile::ConfigCheck::Ok) {
                char why[CHECK_MSG_SIZE];
                _checkMsg(cc, why, sizeof(why));
                _logf(AsyncConfigPortal::LogLevel::Warn,
                      PSTR("profile %u: rejected - %s"), (unsigned)idx, why);
                failed = true;
                return false;
            }

            if (!w->setConfig(cfg)) {
                _logf(AsyncConfigPortal::LogLevel::Error,
                      PSTR("profile %u: could not be saved (device busy); try again"),
                      (unsigned)idx);
                failed = true;
                return false;
            }
            return true;
        }

        NetworkProfile::NetworkConfig cfg;
        if (!p->getConfig(cfg)) {
            _logf(AsyncConfigPortal::LogLevel::Error,
                    PSTR("profile %u: getConfig failed (mutex timeout); "
                    "configuration not saved"), (unsigned)idx);
            failed = true;
            return false;
        }

        bool changed = _patchCommon(req, idx, cfg, failed);
        if (failed || !changed) return false;

        NetworkProfile::ConfigCheck cc = NetworkProfile::checkConfig(cfg);
        if (cc != NetworkProfile::ConfigCheck::Ok) {
            char why[CHECK_MSG_SIZE];
            _checkMsg(cc, why, sizeof(why));
            _logf(AsyncConfigPortal::LogLevel::Warn,
                  PSTR("profile %u: rejected - %s"), (unsigned)idx, why);
            failed = true;
            return false;
        }

        if (!p->setConfig(cfg)) {
            _logf(AsyncConfigPortal::LogLevel::Error,
                    PSTR("profile %u: could not be saved (device busy); try again"),
                    (unsigned)idx);
            failed = true;
            return false;
        }
        return true;
    }

    // Patches the fields common to every interface type. Templated because
    // Reads an optional IP field. Present + empty clears it to 0.0.0.0; present
    // + a valid address sets it; absent or unparseable leaves it unchanged. The
    // address counterpart of the NTP "empty clears" idiom below — an isolated
    // segment must be able to empty its gateway, and any DNS slot. Returns true
    // if the stored value changed.
    static bool _patchOptIp(AsyncWebServerRequest* req, const char* key,
                            IPAddress& out) {
        char buf[PORTAL_FORM_IP_STR_SIZE];
        if (postVal(req, key, buf, sizeof(buf)) < 0) return false;   // absent
        IPAddress next = out;
        if (buf[0] == '\0') {
            next = IPAddress(0, 0, 0, 0);                            // empty -> clear
        } else {
            IPAddress tmp;
            if (tmp.fromString(buf)) next = tmp;                     // valid -> set
            // unparseable -> keep current; the page validates syntax already
        }
        if (next == out) return false;
        out = next;
        return true;
    }

    // Maps a ConfigCheck to a short user-facing reason, for the log line when a
    // save or restore is rejected.
    /// Longest reason below, plus room. Sized so a caller's buffer is one
    /// number rather than a guess.
    static constexpr size_t CHECK_MSG_SIZE = 64;

    // Copies into the caller's buffer rather than returning a pointer, because
    // the texts live in flash: on ESP8266 a %s in the format string would read a
    // flash address as if it were RAM. Returning a pointer would work on ESP32,
    // where flash is memory-mapped, and fail on the platform with the RAM to
    // spare — the worst way round. So the strings stay PROGMEM (446 bytes of
    // .rodata that ESP8266 would otherwise copy into RAM at startup) and the
    // hot path pays a stack buffer only when a save is actually rejected.
    //
    // Every enumerator is listed and the fallback sits after the switch rather
    // than in a default: label. The two behave identically at run time — an
    // unrecognised value still produces the generic line — but default: also
    // silences -Wswitch, so a value added to NetworkProfile::ConfigCheck later
    // would reach the portal as "not valid" with nothing to say it had arrived.
    // This way the compiler names it. Do not fold these back into a default:.
    static void _checkMsg(NetworkProfile::ConfigCheck c, char* out, size_t len) {
        using CC = NetworkProfile::ConfigCheck;
        // Fallback first, so the switch needs no default: label and -Wswitch
        // still fires on an enumerator nobody handled.
        PGM_P p = PSTR("the configuration is not valid");
        switch (c) {
            case CC::Ok:               p = PSTR("the configuration is valid"); break;
            case CC::BadHostname:      p = PSTR("hostname is not valid"); break;
            case CC::BadIp:            p = PSTR("the IP address is not a usable host address on its subnet"); break;
            case CC::BadMask:          p = PSTR("the subnet mask is not valid"); break;
            case CC::GatewayOffSubnet: p = PSTR("the gateway is not on the configured subnet"); break;
            case CC::BadDns:           p = PSTR("a DNS server address is not valid"); break;
            case CC::BadSsid:          p = PSTR("the Wi-Fi SSID is not valid"); break;
            case CC::BadPassword:      p = PSTR("the Wi-Fi password is not valid"); break;
            case CC::BadTxPower:       p = PSTR("the Wi-Fi TX power is not an allowed level"); break;
        }
        if (!out || len == 0) return;
        strncpy_P(out, p, len);
        out[len - 1] = '\0';
    }

    // WiFiConfig derives from NetworkConfig and setConfig() is an overload rather
    // than a virtual, so the two paths cannot share a base-class reference.
    // Field names carry the profile index, e.g. "dhcp0", "ip0", "dns1_0".
    template <typename CfgT>
    bool _patchCommon(AsyncWebServerRequest* req, uint8_t idx, CfgT& cfg,
                      bool& failed) {
        bool changed = false;
        char key[FIELD_KEY_SIZE];
        char buf[FIELD_VAL_SIZE];
        IPAddress addr;

        // Device-level hostname: a single "host" field applied to every profile
        // (mirrors NetworkManager::setHostname).
        //
        // Read into a buffer one byte larger than the destination. postVal()
        // reports the length it copied, so an over-long submission comes back as
        // exactly MAX + 1 and is rejected instead of being silently shortened —
        // a truncated value would then pass validation and be stored as if the
        // user had typed it.
        {
            char host[NetworkProfile::MAX_HOSTNAME_SIZE + 1];
            int n = postVal(req, "host", host, sizeof(host));
            if (n >= 0) {
                if ((size_t)n > NetworkProfile::MAX_HOSTNAME_LEN) {
                    failed = true;
                } else if (strcmp(host, cfg.hostname) != 0) {
                    memcpy(cfg.hostname, host, (size_t)n + 1);
                    changed = true;
                }
            }
        }

        // DHCP (checkbox: value sent only when checked, so absence = off).
        snprintf_P(key, sizeof(key), PSTR("dhcp%u"), idx);
        {
            bool v = false;
            if (postVal(req, key, buf, sizeof(buf)) >= 0) {
                v = (strcmp(buf, "1") == 0 || strcmp(buf, "on") == 0);
            }
            if (cfg.dhcp != v) { cfg.dhcp = v; changed = true; }
        }

        // Static IP / mask / gateway.
        snprintf_P(key, sizeof(key), PSTR("ip%u"), idx);
        if (postIp(req, key, addr) && addr != cfg.ip) {
            cfg.ip = addr; changed = true;
        }
        snprintf_P(key, sizeof(key), PSTR("mask%u"), idx);
        if (postIp(req, key, addr) && addr != cfg.mask) {
            cfg.mask = addr; changed = true;
        }
        snprintf_P(key, sizeof(key), PSTR("gw%u"), idx);
        if (_patchOptIp(req, key, cfg.gateway)) changed = true;

        // DNS servers: one field per DNS_SERVER_COUNT slot. None are required;
        // an empty field clears that slot (0.0.0.0), an omitted one leaves it.
        for (uint8_t d = 0; d < NetworkProfile::DNS_SERVER_COUNT; d++) {
            snprintf_P(key, sizeof(key), PSTR("dns%u_%u"), d, idx);
            if (_patchOptIp(req, key, cfg.dns[d])) changed = true;
        }

        // Priority. Omitted by the page when there is only one interface, in
        // which case the stored value is kept.
        snprintf_P(key, sizeof(key), PSTR("prio%u"), idx);
        if (postVal(req, key, buf, sizeof(buf)) >= 0) {
            uint8_t v = (uint8_t)atoi(buf);
            if (cfg.priority != v) { cfg.priority = v; changed = true; }
        }

        return changed;
    }

    // Patches the Wi-Fi specific fields. Values are only validated later, by
    // setConfig(), so that the whole configuration stands or falls together.
    bool _patchWifi(AsyncWebServerRequest* req, uint8_t idx,
                    WiFiProfile::WiFiConfig& cfg, bool& failed) {
        bool changed = false;
        char key[FIELD_KEY_SIZE];
        char buf[FIELD_VAL_SIZE];

        // As with the hostname above: read one byte wider than the field and
        // reject anything longer, rather than storing a shortened SSID that the
        // user never entered. (A shared FIELD_VAL_SIZE buffer would also be
        // wider than cfg.ssid, which is what -Wformat-truncation flagged.)
        {
            snprintf_P(key, sizeof(key), PSTR("ssid%u"), idx);
            // MAX_SSID_LEN is a macro, not a class constant — the ESP32 WiFi
            // SDK already defines that name, so WiFiProfile publishes it as a
            // guarded macro and it cannot be written qualified.
            char ssid[WiFiProfile::MAX_SSID_SIZE + 1];
            const int n = postVal(req, key, ssid, sizeof(ssid));
            if (n >= 0) {
                if ((size_t)n > MAX_SSID_LEN) {
                    failed = true;
                } else if (strcmp(ssid, cfg.ssid) != 0) {
                    memcpy(cfg.ssid, ssid, (size_t)n + 1);
                    changed = true;
                }
            }
        }

        // Password. "Requires password" carries the clear-it signal; the text
        // field only ever means "set this value". Keeping them apart is what
        // lets an empty field go back to meaning "unchanged" instead of having
        // to encode two different facts.
        //
        //   pwreq absent (unticked)      -> open network: clear the password
        //   pwreq present, field empty   -> keep the stored password
        //   pwreq present, field filled  -> set it
        //
        // A disabled input submits nothing, so the field alone could never
        // express "clear it" — which is why the signal lives on the checkbox,
        // exactly as dhcp<i> does.
        {
            snprintf_P(key, sizeof(key), PSTR("pwreq%u"), idx);
            const bool secured = postHas(req, key);

            if (!secured) {
                if (cfg.password[0] != '\0') {
                    cfg.password[0] = '\0';
                    changed = true;
                }
            } else {
                char pass[WiFiProfile::MAX_PASSWORD_SIZE + 1];
                snprintf_P(key, sizeof(key), PSTR("pass%u"), idx);
                const int n = postVal(req, key, pass, sizeof(pass));
                if (n > (int)WiFiProfile::MAX_PASSWORD_LEN) {
                    failed = true;
                } else if (n > 0 && strcmp(pass, cfg.password) != 0) {
                    memcpy(cfg.password, pass, (size_t)n + 1);
                    changed = true;
                }
            }
        }

        // Password: only overwrite when a non-empty value is submitted (the page
        // leaves it blank to keep the current one, which getConfig() read back).
        {
            char pass[WiFiProfile::MAX_PASSWORD_SIZE + 1];
            snprintf_P(key, sizeof(key), PSTR("pass%u"), idx);
            int n = postVal(req, key, pass, sizeof(pass));
            if (n > 0) {
                if ((size_t)n > WiFiProfile::MAX_PASSWORD_LEN) {
                    failed = true;
                } else {
                    memcpy(cfg.password, pass, (size_t)n + 1);
                    changed = true;
                }
            }
        }

        // Transmit power in dBm. Worth exposing beyond range and consumption
        // tuning: on boards whose chip-antenna routing disturbs the oscillator,
        // turning the radio down is a real stability workaround.
        //
        // Compared with half-step tolerance rather than ==, because the value
        // makes a round trip through decimal text and is quantised to
        // WIFI_TX_POWER_STEP_dBm anyway.
        snprintf_P(key, sizeof(key), PSTR("txpwr%u"), idx);
        if (postVal(req, key, buf, sizeof(buf)) > 0) {
            float dbm = atof(buf);
            if (!(fabsf(dbm - cfg.txPower) <
                  (WiFiProfile::WIFI_TX_POWER_STEP_dBm / 2.0f))) {
                cfg.txPower = dbm; changed = true;
            }
        }

        return changed;
    }

    Entry    _entries[NET_CONFIG_MAX_PROFILES];
    uint8_t  _count = 0;

    // Registrations refused because the registry was full. Counted rather than
    // flagged so the message can say how many were lost, and saturating rather
    // than wrapping — a count that rolls over to zero would hide exactly the
    // problem it exists to report.
    uint8_t  _dropped = 0;
    SavedCb  _onSaved;

    static constexpr uint32_t RESTORE_IDLE_MS = NET_CONFIG_RESTORE_IDLE_MS;

    // Held so the component can report outside attach(). Not owned; the server
    // outlives the component in every arrangement this library supports.
#if NTP_PROFILE_ENABLED
    NtpProfile* _ntp   = nullptr;      ///< device-level NTP servers, or none
    const char* _ntpNs = nullptr;
#endif
    AsyncConfigPortal* _srv = nullptr;

    // All of this component's diagnostics go through here: one null check, one
    // tag, in one place. Silent before attach() by design — there is genuinely
    // nowhere to write yet.
    void _logf(AsyncConfigPortal::LogLevel level, const char* fmt, ...) const
        __attribute__((format(printf, 3, 4))) {
        if (!_srv) return;
        va_list ap;
        va_start(ap, fmt);
        _srv->vlogf(level, "net", fmt, ap);
        va_end(ap);
    }

    /// Ownership of the /netdata buffer for the duration of one upload. The
    /// flag is the one /netdata uses, so a build in progress is not trampled by
    /// an upload starting. See detail/upload_claim.h for why an upload needs an
    /// owner rather than a bare flag.
    UploadClaim& _claim() {
        static UploadClaim c(_netDataBuffer(), REQUIRED_JSON_LEN,
                             _netDataBusy(), RESTORE_IDLE_MS);
        return c;
    }


// -----------------------------------------------------------------------------
// 4. Private — the handlers
// -----------------------------------------------------------------------------

    /** @brief Hands out the current configuration as a download. */
    void _handleBackup(AsyncWebServerRequest* req) {
        if (_netDataBusy().test_and_set(std::memory_order_acquire)) {
            AsyncConfigPortal::sendProgmemLine(req, 503, "text/plain",
                PSTR("Busy: another operation is using the buffer. Try again."));
            return;
        }
        char* buf = _netDataBuffer();
        const bool ok = _buildNetData(buf, REQUIRED_JSON_SIZE);

        if (!ok) {
            _netDataBusy().clear(std::memory_order_release);
            AsyncConfigPortal::sendProgmemLine(req, 500, "text/plain",
                PSTR("Could not read the configuration."));
            return;
        }

        // Stream the buffer (no String copy, which would fail for a multi-KB
        // body on a fragmented ESP8266 heap). beginStreamed releases the buffer
        // flag when the send finishes or the client disconnects.
        AsyncWebServerResponse* resp = AsyncConfigPortal::beginStreamed(
            req, buf, "application/json",
            []() { _netDataBusy().clear(std::memory_order_release); });
        resp->addHeader("Content-Disposition",
                        "attachment; filename=network.json");
        req->send(resp);
    }

    /**
     * @brief Accumulates one chunk of an uploaded document.
     *
     * Nothing is answered from here. Refusing an upload by responding early or
     * closing the connection leaves the browser with a broken transfer rather
     * than an error page, because the client keeps sending regardless; the
     * verdict belongs to the completion handler. A chunk that cannot be stored
     * is therefore swallowed, with the reason remembered for later.
     */
    void _handleRestoreChunk(AsyncWebServerRequest* req, size_t index,
                             uint8_t* data, size_t len, bool final) {
        (void)final;
        _claim().collect(req, index, data, len);   // not ours: swallowed
    }

    /** @brief Validates and applies the accumulated document. */
    void _handleRestoreDone(AsyncWebServerRequest* req) {
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

        char* buf = _netDataBuffer();
        JsonSpan doc = jsonRoot(buf, _claim().size());

        // Errors go out as text/plain rather than as an HTML page. The messages
        // quote values from the document — an SSID, for instance — and an SSID
        // is allowed to contain '<'. Sending them as markup would need escaping
        // on a path that only ever runs when something already went wrong;
        // plain text has no injection surface at all.
        char err[160];

        if (!jsonValidate(doc)) {
            _claim().release(req);
            AsyncConfigPortal::sendProgmemLine(req, 400, "text/plain",
                PSTR("The file is not a valid configuration backup. "
                     "Nothing was changed."));
            return;
        }
        if (!loadFromJson(doc, req, err, sizeof(err))) {
            _claim().release(req);
            req->send(400, "text/plain",
                      err[0] ? err : "The configuration could not be restored.");
            return;
        }

        _claim().release(req);
        AsyncConfigPortal::sendProgmem(req, 200, "text/html", CONFIG_PORTAL_SAVED_HTML);
    }

    /**
     * @brief Emits the extra restore fields for the Backup page.
     *
     * A password never travels in a backup, so a restore that changes the SSID
     * has to be given one. "Requires password" carries the clear-it signal in
     * exactly the same way as on the Network page, and starts ticked or not
     * according to what is stored — so restoring onto the same open network
     * needs no thought.
     */
    bool _restoreFields(char* buf, size_t len) {
        size_t n = 0;
        n += snprintf_P(buf + n, len - n, PSTR("["));

        for (uint8_t i = 0; i < _count && n < len; i++) {
            NetworkProfile* p = _entries[i].profile;
            if (p->getInterfaceType() != NetworkProfile::InterfaceType::WIFI) continue;

            const WiFiProfile::WiFiSecurity sec =
                static_cast<WiFiProfile*>(p)->getSecurity();
            if (sec == WiFiProfile::WiFiSecurity::UNKNOWN) return false;

            n += snprintf_P(buf + n, len - n,
                PSTR("%s{\"name\":\"wifireq%u\",\"label\":\"Requires password\","
                "\"type\":\"checkbox\",\"checked\":%u},"
                "{\"name\":\"wifipass%u\",\"label\":\"Wi-Fi password\","
                "\"type\":\"password\"}"),
                (n > 1) ? "," : "", i,
                (sec == WiFiProfile::WiFiSecurity::PASSWORD) ? 1u : 0u, i);
        }

        if (n >= len) return false;
        n += snprintf_P(buf + n, len - n, PSTR("]"));
        return n < len;
    }

    // Phase-1 staging area.
    //
    // A function-local static for the same reasons as _netDataBuffer(): a few
    // hundred bytes per profile does not belong on the AsyncTCP task stack, and
    // a static cannot fragment the heap. WiFiConfig derives from NetworkConfig,
    // so one array covers both kinds; non-Wi-Fi slots use the base subobject.
    struct Staged {
        WiFiProfile::WiFiConfig cfg;
        uint8_t                 slot    = 0;      ///< index into _entries
        bool                    used    = false;
        bool                    changed = false;
    };

    static Staged* _staged() {
        static Staged s[NET_CONFIG_MAX_PROFILES];
        return s;
    }

    static void _err(char* err, size_t errLen, const char* fmt, ...) {
        if (!err || errLen == 0) return;
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, errLen, fmt, ap);
        va_end(ap);
    }

    /** @brief Finds the registered profile of the given interface type. */
    int _findByType(NetworkProfile::InterfaceType t) const {
        for (uint8_t i = 0; i < _count; i++) {
            if (_entries[i].profile->getInterfaceType() == t) return (int)i;
        }
        return -1;
    }

    // Patches the common fields from a JSON "cfg" object. The JSON twin of
    // _patchCommon(); the key names are the unindexed ones the profile emits
    // ("ip", not "ip0"), because each entry carries its own object.
    //
    // Absent means "not stored", not "unknown": NetworkProfile::_doSave()
    // removes ip/mask/gw/dnsN from NVS while DHCP is on, and toJson() mirrors
    // that. So leaving the staged value untouched is the correct reading, and
    // the round trip loses nothing.
    // Reads an optional IP field from a backup. Absent leaves cfg unchanged;
    // "" clears it to 0.0.0.0; a valid address sets it; a non-string or a
    // non-empty non-address fails, since a backup should not carry garbage. The
    // JSON counterpart of _patchOptIp(). Returns true if the value changed.
    static bool _patchOptIpJson(JsonSpan c, const char* key, IPAddress& out,
                                char* err, size_t errLen, bool& failed) {
        char buf[PORTAL_FORM_IP_STR_SIZE];
        const int n = jsonVal(c, key, buf, sizeof(buf));
        if (n == JSON_ABSENT) return false;
        if (n == JSON_INVALID) {
            _err(err, errLen, "\"%s\" in the backup is not text", key);
            failed = true; return false;
        }
        IPAddress next = out;
        if (buf[0] == '\0') {
            next = IPAddress(0, 0, 0, 0);
        } else {
            IPAddress tmp;
            if (!tmp.fromString(buf)) {
                _err(err, errLen, "\"%s\" in the backup is not a valid address", key);
                failed = true; return false;
            }
            next = tmp;
        }
        if (next == out) return false;
        out = next;
        return true;
    }

    template <typename CfgT>
    bool _patchCommonJson(JsonSpan c, CfgT& cfg, char* err, size_t errLen,
                          bool& failed) {
        bool changed = false;

        {
            char host[NetworkProfile::MAX_HOSTNAME_SIZE + 1];
            int n = jsonVal(c, "host", host, sizeof(host));
            if (n == JSON_INVALID ||
                (n >= 0 && (size_t)n > NetworkProfile::MAX_HOSTNAME_LEN)) {
                _err(err, errLen, "hostname in the backup is invalid or too long");
                failed = true;
            } else if (n >= 0 && strcmp(host, cfg.hostname) != 0) {
                memcpy(cfg.hostname, host, (size_t)n + 1);
                changed = true;
            }
        }

        // "dhcp" is emitted unconditionally by toJson(), so its absence means
        // the document is not one of ours — rejected rather than defaulted.
        {
            bool v = false;
            const int r = jsonNum(c, "dhcp", v);
            if (r != 0) {
                _err(err, errLen, "backup entry has no valid \"dhcp\" field");
                failed = true;
            } else if (cfg.dhcp != v) {
                cfg.dhcp = v;
                changed = true;
            }
        }

        IPAddress addr;
        if (jsonIp(c, "ip", addr) == 0 && addr != cfg.ip) {
            cfg.ip = addr; changed = true;
        }
        if (jsonIp(c, "mask", addr) == 0 && addr != cfg.mask) {
            cfg.mask = addr; changed = true;
        }
        if (_patchOptIpJson(c, "gw", cfg.gateway, err, errLen, failed)) changed = true;
        if (failed) return changed;

        for (uint8_t d = 0; d < NetworkProfile::DNS_SERVER_COUNT; d++) {
            char key[8];
            snprintf_P(key, sizeof(key), PSTR("dns%u"), d);
            if (_patchOptIpJson(c, key, cfg.dns[d], err, errLen, failed)) changed = true;
            if (failed) return changed;
        }

        {
            uint8_t v = 0;
            const int r = jsonNum(c, "prio", v);
            if (r == JSON_INVALID) {
                _err(err, errLen, "interface priority in the backup is invalid");
                failed = true;
            } else if (r == 0 && cfg.priority != v) {
                cfg.priority = v;
                changed = true;
            }
        }

        return changed;
    }

    // Patches the Wi-Fi fields and applies the credential gate.
    //
    // The password never travels in a backup — WiFiProfile::_doJson() omits it
    // on purpose, which is what keeps the passphrase write-only through the web
    // interface. So it can only come from the request, and the gate decides
    // whether it had to.
    bool _patchWifiJson(JsonSpan c, uint8_t idx,
                        AsyncWebServerRequest* req,
                        WiFiProfile::WiFiConfig& cfg,
                        char* err, size_t errLen, bool& failed) {
        bool changed = false;

        // SSID: the entry carries it too, but "cfg" is the authoritative copy.
        char ssid[WiFiProfile::MAX_SSID_SIZE + 1];
        int n = jsonVal(c, "ssid", ssid, sizeof(ssid));
        // Only "cfg" carries the SSID now; the entry-level copy is gone.

        bool ssidChanged = false;
        if (n == JSON_INVALID || (n >= 0 && (size_t)n > MAX_SSID_LEN)) {
            _err(err, errLen, "SSID in the backup is invalid or too long");
            failed = true;
            return false;
        }
        if (n >= 0 && strcmp(ssid, cfg.ssid) != 0) {
            ssidChanged = true;
        }

        // The credential can only come from the request: a backup never
        // carries the password. The restore form uses the same two-control
        // scheme as /net, so the same three facts stay distinguishable.
        //
        //   wifireq absent   -> the target is an open network (a real answer)
        //   wifireq + value  -> that is the password (a real answer)
        //   wifireq + empty  -> keep the stored one (NOT an answer for a new SSID)
        char key[FIELD_KEY_SIZE];
        char pass[WiFiProfile::MAX_PASSWORD_SIZE + 1];
        snprintf_P(key, sizeof(key), PSTR("wifireq%u"), idx);
        const bool secured = postHas(req, key);
        snprintf_P(key, sizeof(key), PSTR("wifipass%u"), idx);
        const int pn = secured ? postVal(req, key, pass, sizeof(pass)) : JSON_ABSENT;

        const bool passwordSupplied = (!secured || pn > 0);

        if (wifiCredentialGate(ssidChanged, passwordSupplied) ==
            CredentialGate::NeedPassword) {
            // Naming the SSID makes the message actionable: the user learns
            // which network's password to fetch.
            _err(err, errLen,
                 "the backup switches Wi-Fi to \"%s\"; supply that network's "
                 "password, or untick \"requires password\" if it is open", ssid);
            failed = true;
            return false;
        }

        if (!secured) {
            if (cfg.password[0] != '\0') {
                cfg.password[0] = '\0';
                changed = true;
            }
        } else if (pn > 0) {
            if ((size_t)pn > WiFiProfile::MAX_PASSWORD_LEN) {
                _err(err, errLen, "the supplied Wi-Fi password is too long");
                failed = true;
                return false;
            }
            if (strcmp(pass, cfg.password) != 0) {
                memcpy(cfg.password, pass, (size_t)pn + 1);
                changed = true;
            }
        }

        if (ssidChanged) {
            memcpy(cfg.ssid, ssid, (size_t)n + 1);
            changed = true;
        }

        // Transmit power, compared with half-step tolerance for the same reason
        // as the form path: it makes a round trip through decimal text.
        float dbm = 0.0f;
        const int r = jsonNum(c, "txpwr", dbm);
        if (r == JSON_INVALID) {
            _err(err, errLen, "Wi-Fi transmit power in the backup is invalid");
            failed = true;
        } else if (r == 0 &&
                   !(fabsf(dbm - cfg.txPower) <
                     (WiFiProfile::WIFI_TX_POWER_STEP_dBm / 2.0f))) {
            cfg.txPower = dbm;
            changed = true;
        }

        return changed;
    }
};
