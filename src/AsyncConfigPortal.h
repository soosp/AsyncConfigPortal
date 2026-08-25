#pragma once

#include <Arduino.h>
#include <functional>
#include <memory>
#include <stdarg.h>

#include <ESPAsyncWebServer.h>
#include <HttpDigestAuth.h>
#if defined(ARDUINO_ARCH_ESP8266)
#  include <Ticker.h>          // one-shot restart timer (no FreeRTOS on ESP8266)
#endif

// Buffer capacities. Convention: the macro (and the *_LEN constant) is the
// maximum *content* length excluding the null terminator; the matching *_SIZE
// constant is that plus one and is what buffer declarations use.

#ifndef CONFIG_PORTAL_JSON_ENDPOINT_LEN
// The largest consumer is NetConfigComponent's /netdata. With two interfaces,
// two DNS servers each, a long hostname and a 32-character SSID the document
// already reaches ~585 characters, so 512 was not enough for that combination
// (the provider returns false and the endpoint answers HTTP 500 rather than
// truncating, so the page simply fails to load). This is a stack buffer in the
// request handler, so raising it costs task stack, not heap.
#  define CONFIG_PORTAL_JSON_ENDPOINT_LEN 768
#endif

#ifndef CONFIG_PORTAL_BACKUPDATA_LEN
// The /backupdata document: one object per section, each with a title, two
// paths and its extra restore fields. Sized for a handful of sections; raise it
// if a build registers many, or components with long field lists.
#  define CONFIG_PORTAL_BACKUPDATA_LEN 768
#endif

#ifndef CONFIG_PORTAL_SECTION_FIELDS_LEN
// Per-section extra-field list. One label/name/type triple runs about 75 bytes,
// and a component emits one pair per thing that needs a secret — NetConfig emits
// two fields per Wi-Fi profile, roughly 155 bytes. 160 was too tight for a
// second Wi-Fi profile and failed in the worst direction: the section rendered
// with no fields at all, leaving the user no box to type the password into while
// the device refused the restore for want of one.
#  define CONFIG_PORTAL_SECTION_FIELDS_LEN 320
#endif

#ifndef CONFIG_PORTAL_MAX_PAGES
#  define CONFIG_PORTAL_MAX_PAGES 16
#endif

#ifndef CONFIG_PORTAL_MAX_BACKUP_SECTIONS
#  define CONFIG_PORTAL_MAX_BACKUP_SECTIONS 4
#endif

#ifndef CONFIG_PORTAL_MAX_RESET_HANDLERS
#  define CONFIG_PORTAL_MAX_RESET_HANDLERS 4
#endif

#ifndef CONFIG_PORTAL_PROJECT_LEN
#  define CONFIG_PORTAL_PROJECT_LEN 340
#endif

#ifndef CONFIG_PORTAL_MENU_LEN
#  define CONFIG_PORTAL_MENU_LEN 512
#endif

#ifndef CONFIG_PORTAL_STATUS_LEN
#  define CONFIG_PORTAL_STATUS_LEN 512
#endif

#ifndef CONFIG_PORTAL_LOG_LEN
#  define CONFIG_PORTAL_LOG_LEN 160
#endif

// Delay (ms) before the deferred restart, so the HTTP response flushes first.
#ifndef CONFIG_PORTAL_RESTART_DELAY_MS
#  define CONFIG_PORTAL_RESTART_DELAY_MS 600
#endif

/**
 * @brief Reusable base class for a modular web configuration interface.
 *
 * Provides the common scaffolding every project needs — HTTP digest auth,
 * a dynamic menu, system-info Status page, an Other page (OTA / restart /
 * factory reset / password change), and the shared REST helpers —
 * while letting a subclass add project-specific pages via registerRoutes().
 *
 * Design:
 *   - Universal parts live here (auth, system status, OTA). Interface- and
 *     project-specific parts (the Network page, GPS/NTP pages) are added by
 *     the subclass, because only it knows the network model and config store.
 *   - Content is added by *composition*: addPage() / addJsonEndpoint() /
 *     addPostHandler(). A subclass typically calls these from registerRoutes().
 *   - The menu is built dynamically from registered pages, ordered by an
 *     int8_t weight (see MENU_* constants), not by registration order — so the
 *     base can register "Other" early yet keep it last in the menu.
 *
 * What the base can serve without any network hardware:
 *   The default Status page shows only interface-independent system info
 *   (uptime, heap, chip, reset reason). A subclass overrides statusJson()
 *   (and optionally statusPageHtml()) to add IP/MAC from whatever interface
 *   it actually has (ETH, Wi-Fi, or both).
 *
 * Threading note:
 *   ESPAsyncWebServer runs in the AsyncTCP task. Pin that task to core 0 via
 *   build flags (e.g. -DCONFIG_ASYNC_TCP_RUNNING_CORE=0) so it never competes
 *   with the PPS discipline / NTP tasks on core 1.
 *
 * Platform: ESP32 family (Arduino-ESP32), ESPAsyncWebServer + HttpDigestAuth.
 */
class AsyncConfigPortal {
public:
    // -------------------------------------------------------------------------
    // Menu ordering weights (int8_t; -1 = hidden). Named for the common levels;
    // subclasses may also pass raw intermediate values (e.g. 75).
    // -------------------------------------------------------------------------

    // Ordering follows what a page costs to look at: the read-only views come
    // first, so anything a passer-by may check sits at the front of the menu,
    // and the pages that change the device — and ask for a login — are grouped
    // at the end. Status, Net status | project pages | Network, Backup, Other.
    static constexpr int8_t MENU_HIDDEN    =  -1;  ///< Not shown in the menu
    static constexpr int8_t MENU_STATUS    =   0;  ///< Status — first
    static constexpr int8_t MENU_NETSTATUS =  10;  ///< Net status — read-only too
    static constexpr int8_t MENU_NORMAL    =  50;  ///< Project pages — default
    static constexpr int8_t MENU_NET       = 100;  ///< Network — near the end
    static constexpr int8_t MENU_OTHER     = 110;  ///< Other — last

    /** @brief Maximum number of registrable pages (menu + hidden).
     *
     * Overridable with CONFIG_PORTAL_MAX_PAGES, as the backup-section and
     * reset-handler registries are. This is the one most likely to fill up —
     * every built-in page, every module and every project page takes a slot —
     * and addPage() returning false is how a page that could not register makes
     * itself known. */
    static constexpr uint8_t MAX_PAGES = CONFIG_PORTAL_MAX_PAGES;

    // -------------------------------------------------------------------------
    // Buffer budgets (named rather than scattered literals)
    //
    // *_LEN is the maximum content length excluding the null terminator (this is
    // the knob); *_SIZE is the buffer declaration size, always _LEN + 1.
    // -------------------------------------------------------------------------

    /** @brief Max /json endpoint body from an addJsonEndpoint provider. A JSON
     *  exceeding this makes the endpoint return HTTP 500 rather than truncate. */
    static constexpr size_t JSON_ENDPOINT_LEN  = CONFIG_PORTAL_JSON_ENDPOINT_LEN;
    static constexpr size_t JSON_ENDPOINT_SIZE = JSON_ENDPOINT_LEN + 1;

    /** @brief Max /project metadata document length. */
    static constexpr size_t PROJECT_LEN  = CONFIG_PORTAL_PROJECT_LEN;
    static constexpr size_t PROJECT_SIZE = PROJECT_LEN + 1;

    /** @brief Max /menu document length. */
    static constexpr size_t MENU_LEN  = CONFIG_PORTAL_MENU_LEN;
    static constexpr size_t MENU_SIZE = MENU_LEN + 1;

    /** @brief Max /status document length. */
    static constexpr size_t STATUS_LEN  = CONFIG_PORTAL_STATUS_LEN;
    static constexpr size_t STATUS_SIZE = STATUS_LEN + 1;

    /** @brief Max formatted log line length. */
    static constexpr size_t LOG_LEN  = CONFIG_PORTAL_LOG_LEN;
    static constexpr size_t LOG_SIZE = LOG_LEN + 1;

    // -------------------------------------------------------------------------
    // Auth level for a route
    // -------------------------------------------------------------------------

    enum class AuthLevel : uint8_t {
        None = 0,   ///< Public — no authentication
        Required,   ///< Requires valid digest credentials
    };

    // -------------------------------------------------------------------------
    // Callback types
    // -------------------------------------------------------------------------

    /** @brief Fills @p buf with a JSON body; returns true on success. */
    using JsonProvider = std::function<bool(char* buf, size_t len)>;

    /**
     * @brief Emits a section's extra restore fields as a JSON array.
     *
     * Field descriptors, not markup: the page builds the inputs itself, so a
     * component cannot inject arbitrary HTML into a built-in page, and nothing
     * has to be HTML-escaped through JSON. Each element is
     *
     *     {"name":"...","label":"...","type":"checkbox"|"password"[,"checked":0|1]}
     *
     * The two types are not a placeholder set. Extra fields exist for exactly
     * one reason — a secret cannot travel in a backup document, because putting
     * it there would undo the write-only property that keeps it out of every
     * other response. Anything that is not a secret belongs in the document, so
     * before adding a type, ask why the value is not simply backed up.
     *
     * @return false on failure; the section then renders without extra fields.
     */
    using FieldProvider = std::function<bool(char* buf, size_t len)>;

    /**
     * @brief Erases one component's persisted state for a factory reset.
     *
     * @return false if anything could not be erased. The reset does not stop:
     *         every handler runs and the device restarts regardless. A device
     *         left half-erased but still running, with no clear signal, is
     *         worse than one that reboots into a known-mostly-default state
     *         and can simply be reset again.
     */
    using ResetFn = std::function<bool()>;

    /**
     * @brief JSON source that returns a pointer to storage it owns itself.
     *
     * The alternative to JsonProvider, for components whose document is large or
     * whose size depends on their own configuration. Such a component sizes and
     * owns its buffer in its own header, which keeps the size and any size check
     * in one translation unit — a macro that only reaches some translation units
     * (the Arduino IDE compiles library sources separately from the sketch) can
     * then no longer make the two disagree.
     *
     * Return nullptr to report failure; the endpoint answers HTTP 500. The
     * returned text is copied into the response before the handler returns.
     */
    using JsonSource = std::function<const char*()>;

    /**
     * @brief Releases a JsonSource's buffer once its streamed response has been
     *        fully sent (or the client disconnected). See the streaming
     *        addJsonEndpoint() overload.
     */
    using JsonRelease = std::function<void()>;

    /** @brief Handles a POST request (subclass writes its own config store). */
    using PostHandler = std::function<void(AsyncWebServerRequest* req)>;

    /**
     * @brief Called for each chunk of an uploaded file.
     *
     * Mirrors ESPAsyncWebServer's upload callback minus the filename, which no
     * caller in this library has needed: @p index is this chunk's offset in the
     * file, @p final marks the last one. Fires several times per request, before
     * the completion handler runs.
     */
    using UploadHandler = std::function<void(AsyncWebServerRequest* req,
                                             size_t index, uint8_t* data,
                                             size_t len, bool final)>;

    /**
     * @brief Severity level carried by the log hook.
     *
     * Lets the application map diagnostics to the ESP log severities
     * (ESP_LOGE/W/I/D) so an industrial deployment can filter by level,
     * instead of everything landing at a single severity.
     */
    enum class LogLevel : uint8_t {
        Error = 0,  ///< Operation failed / subsystem fault (e.g. OTA write error)
        Warn,       ///< Attempt blocked or state change worth noticing (e.g. OTA rejected)
        Info,       ///< Normal milestone (e.g. OTA success)
        Debug,      ///< Verbose diagnostic
    };

    /** @brief Optional log sink. If unset, the server is silent — it never
     *  assumes a configured Serial. Set via setLogger() to route diagnostics
     *  to Serial, SafeSerial, or anywhere.
     *
     *  @param level maps to the ESP log severities (ESP_LOGE/W/I/D).
     *  @param tag   names the subsystem that emitted the line ("ota", ...).
     *               Pass it straight to ESP_LOGx as the tag — ESP_LOGx accepts a
     *               runtime tag — so the log is filterable per subsystem via
     *               esp_log_level_set() instead of collapsing into one label for
     *               the whole server. Always a string literal, never null. */
    using LogFn = std::function<void(LogLevel level, const char* tag, const char* msg)>;

    // -------------------------------------------------------------------------
    // Constructor / Destructor
    // -------------------------------------------------------------------------

    /**
     * @brief Constructs the server on the given port (default 80).
     * @param port TCP port for the HTTP server.
     */
    explicit AsyncConfigPortal(uint16_t port = 80);

    virtual ~AsyncConfigPortal() = default;

    AsyncConfigPortal(const AsyncConfigPortal&)            = delete;
    AsyncConfigPortal& operator=(const AsyncConfigPortal&) = delete;

    // -------------------------------------------------------------------------
    // Lifecycle
    // -------------------------------------------------------------------------

    /**
     * @brief Wires up the common routes, calls registerRoutes(), and starts.
     *
     * Registers the system endpoints (/css, /favicon.ico, /menu, /project,
     * /chkauth, /logout), the default Status and Other pages, then invokes
     * registerRoutes() so the subclass can add its own pages, and finally
     * begins the underlying AsyncWebServer.
     *
     * @param auth Digest-auth credentials store. Must outlive this server.
     */
    void begin(HttpDigestAuth& auth);

    // -------------------------------------------------------------------------
    // Content registration (composition API)
    // -------------------------------------------------------------------------

    /**
     * @brief Registers an HTML page and (if visible) a menu entry.
     *
     * @param path  URL path, e.g. "/gps".
     * @param label Menu label, e.g. "GPS". Ignored if @p order is MENU_HIDDEN.
     * @param html  PROGMEM HTML body served at @p path (JS fills it via REST).
     * @param auth  Whether the page requires authentication.
     * @param order Menu weight; MENU_HIDDEN to omit from the menu.
     * @return false if the page registry is full (MAX_PAGES reached).
     */
    bool addPage(const char* path, const char* label, const char* html,
                 AuthLevel auth = AuthLevel::None, int8_t order = MENU_NORMAL);

    /**
     * @brief Registers a GET endpoint that returns a JSON body.
     *
     * Not a menu entry. Used for the data the pages fetch (e.g. "/gpsdata").
     *
     * @param path      URL path.
     * @param provider  Fills the JSON body (buffer is JSON_ENDPOINT_SIZE bytes).
     * @param auth      Whether the endpoint requires authentication.
     * @return true (reserved for future failure signalling; always registers).
     */
    bool addJsonEndpoint(const char* path, JsonProvider provider,
                         AuthLevel auth = AuthLevel::None);

    /**
     * @brief Registers a JSON endpoint served from the caller's own storage.
     * @see JsonSource for when to prefer this over the JsonProvider form.
     */
    bool addJsonEndpoint(const char* path, JsonSource source,
                         AuthLevel auth = AuthLevel::None);

    /**
     * @brief Streaming JsonSource endpoint, for bodies too large to copy into a
     *        String on a fragmented heap (the ESP8266 failure mode).
     *
     * The body is streamed straight from the source's buffer — no String copy.
     * The buffer must therefore stay valid until the send completes, so the
     * source keeps it claimed and returns it *without* releasing; @p release is
     * then called exactly once, when the response has been fully sent or the
     * client disconnects, for the source to unlock/free it. A nullptr from the
     * source yields HTTP 500 and @p release is not called (the source handles
     * its own failure path).
     */
    bool addJsonEndpoint(const char* path, JsonSource source,
                         JsonRelease release, AuthLevel auth = AuthLevel::None);

    /**
     * @brief Registers a POST handler (e.g. the subclass's "/net" save).
     *
     * @param path     URL path.
     * @param handler  Handles the request (parse args, write config store).
     * @param auth     Whether the endpoint requires authentication.
     * @return true (reserved for future failure signalling; always registers).
     */
    bool addPostHandler(const char* path, PostHandler handler,
                        AuthLevel auth = AuthLevel::Required);

    /**
     * @brief Replaces the stylesheet served on /css.
     *
     * @param css A PROGMEM stylesheet, or nullptr to restore the built-in one.
     *
     * Runtime rather than compile-time on purpose: the handler that serves /css
     * lives in the library's own translation unit, so a `#define` in a sketch
     * never reaches it, and a macro passed through build flags would name a
     * symbol that unit cannot see. A pointer set from setup() has neither
     * problem. Taking this route means the built-in sheet stays in flash; see
     * docs/CSS.md if that matters.
     */
    void setCss(const char* css) { _css = css; }

    /**
     * @brief Appends an application stylesheet after the one served on /css.
     *
     * Later rules win by cascade order, so a supplement only states what differs
     * — usually a handful of colour variables. Every page loads the same /css,
     * the built-in ones included, so this restyles the whole portal.
     *
     * @param css A PROGMEM stylesheet, or nullptr for none.
     */
    void setCssExtra(const char* css) { _cssExtra = css; }

    /**
     * @brief Serve a PROGMEM body safely on both platforms.
     *
     * On ESP8266 a plain send() would byte-read flash and fault (LoadStoreError),
     * so the body is streamed with FPSTR, which reads PROGMEM correctly; ESP32
     * flash is memory-mapped, so a plain send() is fine. Part of the composition
     * API: components (e.g. NetConfigComponent) that serve their own PROGMEM
     * pages from a handler should use this instead of req->send().
     */
    static void sendProgmem(AsyncWebServerRequest* req, int code,
                            const char* type, const char* body,
                            const char* extra) {
        // Two PROGMEM segments sent as one body. Used for the stylesheet, where
        // an application supplement follows the built-in sheet, so that later
        // rules win by cascade order without the application having to restate
        // the whole sheet.
        const size_t n1 = strlen_P(body);
        const size_t n2 = extra ? strlen_P(extra) : 0;
        const size_t total = n1 + n2;
        AsyncWebServerResponse* r = req->beginChunkedResponse(type,
            [body, extra, n1, total](uint8_t* dst, size_t maxLen, size_t index) -> size_t {
                const size_t rem = total - index;
                size_t n = rem < maxLen ? rem : maxLen;
                if (!n) return 0;
                if (index < n1) {                       // still in the first segment
                    if (index + n > n1) n = n1 - index;
                    memcpy_P(dst, body + index, n);
                } else {
                    memcpy_P(dst, extra + (index - n1), n);
                }
                return n;
            });
        r->setCode(code);
        req->send(r);
    }

    static void sendProgmem(AsyncWebServerRequest* req, int code,
                            const char* type, const char* body) {
#if defined(ARDUINO_ARCH_ESP8266)
        // Stream straight out of flash. A plain send() would byte-read PROGMEM
        // and fault; buffering the page into a response stream would copy the
        // whole thing into RAM, and several assets loading at once exhaust the
        // heap. Chunked + memcpy_P costs only the TCP chunk.
        const size_t total = strlen_P(body);
        AsyncWebServerResponse* r = req->beginChunkedResponse(type,
            [body, total](uint8_t* dst, size_t maxLen, size_t index) -> size_t {
                const size_t rem = total - index;
                const size_t n = rem < maxLen ? rem : maxLen;
                if (n) memcpy_P(dst, body + index, n);
                return n;
            });
        r->setCode(code);
        req->send(r);
#else
        req->send(code, type, body);
#endif
    }

    /**
     * @brief Sends a short PROGMEM body — an error line, not a page.
     *
     * sendProgmem() above streams a chunked response, which is what a page
     * needs and more machinery than a one-line message deserves. This is the
     * short form.
     *
     * The platform split is settled here rather than at every call site.
     * ESP8266 needs send_P() to read the body out of flash; on ESP32 flash is
     * memory-mapped, plain send() reads it directly, and send_P() is marked
     * deprecated — so writing send_P() everywhere would warn on one platform
     * and writing send() would fault on the other.
     *
     * @param body PROGMEM string, i.e. a PSTR() literal. A RAM string sent
     *             through here would be read as a flash address on ESP8266.
     */
    static void sendProgmemLine(AsyncWebServerRequest* req, int code,
                                const char* type, PGM_P body) {
#if defined(ARDUINO_ARCH_ESP8266)
        req->send_P(code, type, body);
#else
        req->send(code, type, body);
#endif
    }

    /**
     * @brief Build a chunked response that streams a RAM buffer without copying
     *        it into a String (which fails for multi-KB bodies on a fragmented
     *        ESP8266 heap).
     *
     * The buffer must stay valid until sending finishes; @p release runs exactly
     * once when it does — on the final chunk or on client disconnect — for the
     * caller to unlock/free it. Returns the response so the caller can add
     * headers before req->send().
     */
    static AsyncWebServerResponse* beginStreamed(AsyncWebServerRequest* req,
                                                 const char* body, const char* type,
                                                 JsonRelease release) {
        const size_t total = body ? strlen(body) : 0;
        auto done = std::make_shared<bool>(false);
        auto fire = [done, release]() { if (release && !*done) { *done = true; release(); } };
        AsyncWebServerResponse* r = req->beginChunkedResponse(type,
            [body, total, fire](uint8_t* dst, size_t maxLen, size_t index) -> size_t {
                const size_t rem = total - index;
                const size_t n = rem < maxLen ? rem : maxLen;
                if (n) memcpy(dst, body + index, n);
                if (index + n >= total) fire();     // last data chunk (or the 0 terminator)
                return n;
            });
        req->onDisconnect([fire]() { fire(); });     // covers an early disconnect
        return r;
    }

    /**
     * @brief Registers a section on the built-in Backup page.
     *
     * The page and its menu entry come into existence with the first section:
     * a device whose whole configuration is two checkboxes should not carry a
     * Backup menu item it has no use for.
     *
     * Both paths are taken in one call deliberately. A component offering a
     * download but no upload — or the reverse — is a trap for the user, and
     * separate registration calls would make that state reachable.
     *
     * @param title        Section heading, e.g. "Network".
     * @param backupPath   POST route that returns the component's document as a
     *                     download.
     * @param restorePath  POST route that accepts it back as a file upload.
     * @param extraFields  Optional extra inputs for the restore form.
     * @param order        Weight within the page (same convention as menus).
     * @return false if the section registry is full.
     */
    bool addBackupSection(const char* title,
                          const char* backupPath,
                          const char* restorePath,
                          FieldProvider extraFields = nullptr,
                          int8_t order = MENU_NORMAL);

    /**
     * @brief Registers state to be erased by a factory reset.
     *
     * With no handler registered the reset still runs: it clears the stored
     * credentials and restarts. That is the base's own state — everything else
     * belongs to a component, and a component that does not register keeps its
     * data through a factory reset.
     *
     * @param what Short name for the log, e.g. "network". Not shown in the UI:
     *             a factory reset is all-or-nothing by definition, so there is
     *             nothing for the user to choose between — but when one part
     *             fails, the log has to be able to name it.
     * @param fn   Performs the erase.
     * @return false if the registry is full.
     */
    bool addResetHandler(const char* what, ResetFn fn);

    /**
     * @brief Registers a POST route that accepts an uploaded file.
     *
     * The difference from addPostHandler() is that the body is delivered in
     * pieces: @p onChunk runs repeatedly while the file arrives, @p onComplete
     * once afterwards. A handler that needs the whole document must accumulate
     * it itself — this library does not buffer it, because the right buffer size
     * and lifetime are the component's business, not the server's.
     *
     * Two things are easy to get wrong here, so they are done for the caller:
     *
     *   - Auth is checked on the chunk callback as well as on completion.
     *     Checking only the completion handler would let an unauthenticated
     *     client push a whole file through the chunk handler first and be
     *     refused afterwards, which is a free write into someone's buffer.
     *
     *   - Nothing is sent from the chunk callback. Refusing an upload by
     *     answering early or closing the connection leaves the browser with a
     *     broken transfer instead of an error page, because the client keeps
     *     sending regardless. The convention, which the OTA route already
     *     follows, is to swallow the remaining chunks without acting on them
     *     and deliver the verdict from @p onComplete.
     *
     * @param path       URL path.
     * @param onComplete Runs after the last chunk; sends the response.
     * @param onChunk    Runs per chunk; may be null to ignore the body.
     * @param auth       Whether the route requires authentication.
     * @return true (reserved for future failure signalling; always registers).
     */
    bool addUploadHandler(const char* path, PostHandler onComplete,
                          UploadHandler onChunk,
                          AuthLevel auth = AuthLevel::Required);

    /**
     * @brief Sets an optional log sink for diagnostics (OTA, restart, etc.).
     *
     * If never called, the server is silent — it does not assume a configured
     * Serial. Route to Serial, SafeSerial, or elsewhere as the app prefers.
     */
    void setLogger(LogFn fn) { _log = std::move(fn); }

    /**
     * @brief Writes a formatted line to the configured log sink.
     *
     * Public so that components attached by composition can report. They are
     * not subclasses and have no other channel: an HTTP status reaches the
     * browser, not the person reading the serial console.
     *
     * Does nothing when no sink is installed, so a caller never has to check.
     * Note that setLogger() must therefore run before anything that logs —
     * attach() included.
     *
     * The library's own diagnostics go through here too. A private variant
     * existed briefly to keep the internal path off the public contract; it was
     * dropped because the varargs form cannot delegate to another varargs
     * function, so the split bought a duplicated shim and nothing else. Add a
     * private _vlogf() only if the two paths ever genuinely need to differ.
     *
     * @param level Severity.
     * @param tag   Subsystem, e.g. "net". Kept filterable by the sink.
     * @param fmt   printf-style format; the line is truncated at LOG_LEN.
     */
    void logf(LogLevel level, const char* tag, const char* fmt, ...) const
        __attribute__((format(printf, 4, 5)));

    /** @brief va_list form of logf(), for callers forwarding their own varargs. */
    void vlogf(LogLevel level, const char* tag, const char* fmt,
               va_list ap) const;

    // -------------------------------------------------------------------------
    // Project metadata (shown on every page footer / title)
    // -------------------------------------------------------------------------

    /**
     * @brief Sets the project metadata served at /project.
     *
     * @note Setup-only: call before begin(), from the main task. Stores the
     *       caller-owned pointers (no copy); not safe to call concurrently with
     *       request handling.
     *
     * @param name Project name.  @param version Version string.
     * @param desc Short description.  @param year Year string.
     * @param author Author string.
     */
    void setProject(const char* name, const char* version,
                    const char* desc, const char* year,
                    const char* author);

protected:
    // -------------------------------------------------------------------------
    // Overridable Status page (default: interface-independent system info)
    // -------------------------------------------------------------------------

    /**
     * @brief Returns the HTML for the Status page. Default: a generic system
     *        info page. Override to show a project-specific Status page.
     */
    virtual const char* statusPageHtml() const;

    /**
     * @brief Fills @p buf with the Status JSON. Default: uptime, heap, chip,
     *        flash, reset reason — no network assumptions. Override to add
     *        IP/MAC/link from the interface the project actually has.
     * @return true on success.
     */
    virtual bool statusJson(char* buf, size_t len) const;

    /**
     * @brief Hook for the subclass to register its project-specific pages.
     *
     * Called by begin() after the common routes are set up. Default: no-op
     * (a bare AsyncConfigPortal still serves Status + Other + system routes).
     */
    virtual void registerRoutes() {}

    // -------------------------------------------------------------------------
    // Shared helpers available to subclasses
    // -------------------------------------------------------------------------

    /**
     * @brief Checks digest auth on a request; sends a challenge if it fails.
     * @return true if authenticated, false if a challenge was sent (caller
     *         must return immediately).
     */
    bool requireAuth(AsyncWebServerRequest* req) const;

    /** @brief The underlying server, for advanced subclass needs. */
    AsyncWebServer& server() { return _server; }

    /** @brief The auth store (valid after begin()). */
    HttpDigestAuth* auth() { return _auth; }

private:
    // -------------------------------------------------------------------------
    // Internal page registry
    // -------------------------------------------------------------------------

    struct PageEntry {
        const char* path  = nullptr;
        const char* label = nullptr;
        int8_t      order = MENU_HIDDEN;
    };

    static constexpr size_t  BACKUPDATA_LEN  = CONFIG_PORTAL_BACKUPDATA_LEN;
    static constexpr size_t  BACKUPDATA_SIZE = BACKUPDATA_LEN + 1;
    static constexpr size_t  SECTION_FIELDS_LEN  = CONFIG_PORTAL_SECTION_FIELDS_LEN;
    static constexpr size_t  SECTION_FIELDS_SIZE = SECTION_FIELDS_LEN + 1;
    static constexpr uint8_t MAX_BACKUP_SECTIONS =
        CONFIG_PORTAL_MAX_BACKUP_SECTIONS;

    struct BackupSection {
        const char*   title       = nullptr;
        const char*   backupPath  = nullptr;
        const char*   restorePath = nullptr;
        FieldProvider fields;
        int8_t        order       = MENU_NORMAL;
    };

    BackupSection _backupSections[MAX_BACKUP_SECTIONS];
    uint8_t       _backupCount = 0;

    static constexpr uint8_t MAX_RESET_HANDLERS =
        CONFIG_PORTAL_MAX_RESET_HANDLERS;

    struct ResetTarget {
        const char* what = nullptr;
        ResetFn     fn;
    };

    ResetTarget _resetTargets[MAX_RESET_HANDLERS];
    uint8_t     _resetCount = 0;

    void _handleBackupData(AsyncWebServerRequest* req) const;

    // -------------------------------------------------------------------------
    // System route handlers
    // -------------------------------------------------------------------------

    void _registerSystemRoutes();
    void _handleMenu(AsyncWebServerRequest* req) const;
    void _handleProject(AsyncWebServerRequest* req) const;
    void _handleStatusData(AsyncWebServerRequest* req) const;

    /** @brief Registers the built-in Other page (OTA/restart/factory reset/password). */
    void _registerOtherPage();

    /** @brief Adds a menu entry to the ordered registry (if not hidden).
     *  @return false if the registry is full. */
    bool _addMenuEntry(const char* path, const char* label, int8_t order);

    /** @brief Reboots via a short-lived task so the HTTP response flushes first. */
    void _scheduleRestart();


 

    // -------------------------------------------------------------------------
    // State
    // -------------------------------------------------------------------------

    AsyncWebServer  _server;
    /// Stylesheet served on /css; nullptr means the built-in one. The macro that
    /// names it lives in ConfigWebPages.h, which this header does not include —
    /// the serving side resolves it instead, so this header stays independent.
    const char*     _css      = nullptr;
    const char*     _cssExtra = nullptr;             ///< appended after it
#if defined(ARDUINO_ARCH_ESP8266)
    Ticker          _restartTicker;   // fires the deferred restart (see _scheduleRestart)
#endif
    HttpDigestAuth* _auth = nullptr;
    LogFn           _log;   ///< Optional diagnostic sink; silent if unset.

    // Set by the OTA upload handler when an image is rejected (wrong project/
    // board marker), read by the completion handler to report the mismatch.
    bool _otaReject = false;

    // OTA marker stream-scan state: the marker may sit anywhere in the image,
    // so we scan every chunk (with a small tail carried across chunk boundaries)
    // and only finalize the update if a matching marker was seen.
    static constexpr size_t OTA_TAIL_KEEP = 64;  ///< bytes kept across boundaries
    bool     _otaMarkerFound = false;
    bool     _otaMarkerMatch = false;
    uint8_t  _otaTail[OTA_TAIL_KEEP];
    size_t   _otaTailLen = 0;

    PageEntry _pages[MAX_PAGES];
    uint8_t   _pageCount = 0;

    // Project metadata (pointers to caller-owned static strings).
    const char* _projName = "Device";
    const char* _projVer  = "0.0.0";
    const char* _projDesc = "Configuration";
    const char* _projYear = "2026";
    const char* _projAuthor = "";
};
