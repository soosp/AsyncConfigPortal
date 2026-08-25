#pragma once

/**
 * @file NetStatusComponent.h
 * @brief Optional, opt-in Network status page for AsyncConfigPortal.
 *
 * Shows what the device is *actually* on — interface, address, DNS, NTP, and on
 * Wi-Fi the associated network and signal — read live from NetworkManager rather
 * than from the stored configuration. The two can differ, and that difference is
 * the reason a status page exists.
 *
 * @warning The sketch must include <NetworkManager.h> itself, even though this
 *          header does. The Arduino IDE builds the include path from the
 *          *sketch's* includes, so a library the sketch never names is not on
 *          it — and the ESP32 core ships a NetworkManager.h of its own, which is
 *          what the compiler finds instead. The symptom is
 *          "'NetworkManagerClass' has not been declared" from inside this file.
 *          PlatformIO resolves it either way.
 *
 * Separate from NetConfigComponent on purpose. That one needs only the
 * NetworkProfile family; this one needs NetworkManager as well, and folding it in
 * would hand that dependency to every project that merely edits profiles.
 * Including this header is what pulls it in — nothing else in the library does.
 *
 * Usage:
 * @code
 * #include <AsyncConfigPortal.h>
 * #include <NetStatusComponent.h>
 *
 * AsyncConfigPortal   web;
 * NetStatusComponent  netStatus;
 *
 * void setup() {
 *     web.begin(auth);
 *     netStatus.attach(web);        // adds the page and its menu entry
 * }
 * @endcode
 *
 * The page is read-only, so there is no POST handler, no validation and no
 * backup section — it is the thinnest component the composition API supports,
 * and a useful shape to copy: one page, one JSON endpoint.
 */

#include <atomic>

#include <AsyncConfigPortal.h>
#include <NetworkManager.h>

#include "NetStatusPages.h"

// The reserve is paid for whether or not a section is registered, so the
// defaults are what a device actually uses rather than a round number. Raise
// them if a project registers more sections, or reports more per section.
#ifndef NET_STATUS_MAX_SECTIONS
#  define NET_STATUS_MAX_SECTIONS 2
#endif
#ifndef NET_STATUS_SECTION_LEN
#  define NET_STATUS_SECTION_LEN 192
#endif

class NetStatusComponent {
public:
    /**
     * @brief Fills a JSON object of flat label/value pairs for one section.
     *
     * Returning false leaves the section out of the document entirely, which is
     * how a service that is switched off disappears from the page rather than
     * showing empty rows.
     */
    using SectionProvider = std::function<bool(char* buf, size_t len)>;

    /**
     * @brief Adds an application section below the network ones.
     *
     * The page shows what NetworkManager knows; anything else — an MQTT
     * connection, a cloud link, a sensor bus — is the application's state, and
     * the application is the only thing that can report it. Rather than teach
     * this component about each service, it renders whatever the provider hands
     * over:
     *
     * @code
     * netStatus.addSection("MQTT", [](char* buf, size_t len) {
     *     if (!mqttEnabled) return false;                  // section omitted
     *     return snprintf(buf, len,
     *         "{\"Broker\":\"%s\",\"State\":\"%s\"}",
     *         host, mqtt.connected() ? "connected" : "offline") < (int)len;
     * });
     * @endcode
     *
     * @param title    Section heading.
     * @param provider Fills a JSON object; string values are shown as they are.
     * @return false if the section registry is full.
     */
    bool addSection(const char* title, SectionProvider provider) {
        if (_sectionCount >= MAX_SECTIONS || !title || !provider) return false;
        _sections[_sectionCount].title    = title;
        _sections[_sectionCount].provider = provider;
        _sectionCount++;
        return true;
    }

    /**
     * @brief Registers the page and its data endpoint.
     *
     * @param srv   The portal to attach to.
     * @param order Menu position; defaults next to Status, since both are
     *              read-only views and the menu groups those first.
     * @param label Menu label.
     * @param auth  Whether the page requires a login. Open by default: checking
     *              a signal level or an address is the kind of thing one does
     *              while standing next to the device, and demanding admin
     *              credentials for a read-only view mostly trains people to log
     *              in for everything. Pass AuthLevel::Required where that is not
     *              the right trade — the page reveals the SSID, the addressing
     *              and the NTP servers, and a BSSID can be looked up in public
     *              wardriving databases, which turns it into a location hint.
     */
    bool attach(AsyncConfigPortal& srv,
                int8_t      order = AsyncConfigPortal::MENU_NETSTATUS,
                const char* label = "Net status",
                AsyncConfigPortal::AuthLevel auth =
                    AsyncConfigPortal::AuthLevel::None) {
        _srv = &srv;
        bool ok = srv.addPage("/netstatus", label, CONFIG_PORTAL_NETSTATUS_HTML,
                              auth, order);

        // Served from this component's own buffer rather than the server's
        // shared one, for the same reason NetConfigComponent does it: the size
        // follows NetworkManager's NTP server count, and each name may be a
        // full-length FQDN — three of them already exceed the shared buffer's
        // default. Sizing it here keeps the buffer and the requirement in one
        // place, instead of depending on a macro that does not reach every
        // translation unit under the Arduino IDE.
        //
        // Streamed rather than copied: the send path would otherwise put the
        // whole document through a String, which a fragmented heap can refuse.
        ok = srv.addJsonEndpoint("/netstatusdata",
            [this]() -> const char* {
                char* buf = _buffer();
                if (_busy().test_and_set(std::memory_order_acquire)) {
                    // Says so rather than answering 500 without a trace: the
                    // single-task invariant makes this a bug, not a race.
                    if (_srv) _srv->logf(AsyncConfigPortal::LogLevel::Error,
                                         "netstatus",
                                         PSTR("buffer contended"));
                    return nullptr;
                }
                if (!_build(buf, BUF_SIZE)) {
                    _busy().clear(std::memory_order_release);
                    return nullptr;
                }
                return buf;   // released by the framework once the send completes
            },
            []() { _busy().clear(std::memory_order_release); },
            auth) && ok;   // same level as the page: a public page whose data
                           // endpoint is locked would just render empty
        return ok;
    }

private:
    /** @brief Maximum number of application sections. */
    static constexpr uint8_t MAX_SECTIONS = NET_STATUS_MAX_SECTIONS;

    /** @brief Buffer reserved for one application section's JSON. */
    static constexpr size_t SECTION_LEN = NET_STATUS_SECTION_LEN;

    struct Section {
        const char*     title = nullptr;
        SectionProvider provider;
    };
    Section            _sections[MAX_SECTIONS];
    uint8_t            _sectionCount = 0;
    AsyncConfigPortal* _srv          = nullptr;   ///< for logging only

    /// Wraps NetworkManager's document and appends whatever the application
    /// registered. A section whose provider declines is simply not there — the
    /// page renders what it is given.
    bool _build(char* buf, size_t len) {
        buf[0] = '\0';
        if (!json_cat_P(buf, PSTR("{\"net\":"), len)) return false;

        const size_t n = strlen(buf);
        if (NetworkManager.statusToJson(buf + n, len - n, /*includeNtp=*/true) == 0)
            return false;

        bool ok = json_cat_P(buf, PSTR(",\"sections\":["), len);
        char sec[SECTION_LEN];
        bool first = true;
        for (uint8_t i = 0; i < _sectionCount && ok; i++) {
            sec[0] = '\0';
            if (!_sections[i].provider(sec, sizeof(sec))) continue;   // declined
            if (!first) ok = ok && json_cat_P(buf, PSTR(","), len);
            ok = ok && json_cat_P(buf, PSTR("{\"title\":\""), len);
            ok = ok && json_cat_esc(buf, _sections[i].title, len);
            ok = ok && json_cat_P(buf, PSTR("\",\"fields\":"), len);
            ok = ok && json_cat(buf, sec, len);
            ok = ok && json_cat_P(buf, PSTR("}"), len);
            first = false;
        }
        ok = ok && json_cat_P(buf, PSTR("]}"), len);
        if (!ok) buf[0] = '\0';
        return ok;
    }

    /// Worst case for NetworkManager::statusToJson(), plus room for the
    /// application sections this page may carry.
    static constexpr size_t BUF_SIZE = NetworkManagerClass::STATUS_JSON_SIZE
                                     + MAX_SECTIONS * (SECTION_LEN + 48)
                                     + 32;   // {"net":...,"sections":[...]}

    /// Function-local statics: one buffer for the class, created on first use,
    /// with no ordering question at static-init time.
    static char* _buffer() { static char buf[BUF_SIZE]; return buf; }

    /// Guards the buffer for the duration of a send. The single-task invariant
    /// makes contention a bug rather than a race; the claim makes it visible as
    /// an HTTP error instead of a corrupted document.
    static std::atomic_flag& _busy() {
        static std::atomic_flag f = ATOMIC_FLAG_INIT;
        return f;
    }
};
