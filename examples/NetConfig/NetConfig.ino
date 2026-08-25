// NetConfig.ino
//
// Demonstrates the library's optional NetConfigComponent: an Ethernet and a
// Wi-Fi profile are driven by NetworkManager and exposed on the /net page, so
// addressing, DNS, hostname, interface priority, Wi-Fi credentials and Wi-Fi
// transmit power can all be changed from a browser and survive a reboot.
//
// Because two interfaces are registered, the page also shows the priority
// fields and enforces that the two cannot share a priority. With a single
// interface the priority field is hidden entirely.
//
// -----------------------------------------------------------------------------
// Dependencies
// -----------------------------------------------------------------------------
//
// NetConfigComponent is opt-in: the base library does not depend on the profile
// libraries, and you only pull them in by including the component. Add to your
// own lib_deps:
//
//     soosp/AsyncConfigPortal
//     soosp/NetworkProfile
//     soosp/NetworkManager
//     soosp/HttpDigestAuth
//     ESP32Async/AsyncTCP
//     ESP32Async/ESPAsyncWebServer
//
// PlatformIO also needs the default LDF mode:
//
//     lib_ldf_mode = chain
//
// (deep / deep+ scan every library header regardless of includes and would
// resolve NetworkProfile.h even for projects that never opt in.)
//
// Default credentials on first boot: admin / admin.

// -----------------------------------------------------------------------------
// Compile-time configuration — must precede the adapter includes below
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// build_opt.h
// -----------------------------------------------------------------------------
//
// Macros read by the libraries are set in build_opt.h next to this sketch, not
// here. The Arduino IDE compiles library sources as separate translation units,
// which a #define in the sketch never reaches; build_opt.h is passed to all of
// them. (PlatformIO: put the same values in build_flags.)
//
// That file's contents go straight onto the compiler command line, so it holds
// nothing but flags — it may NOT contain comments, despite the .h extension.
// What it sets, and why, is therefore documented here:
//
//   NET_CONFIG_MAX_PROFILES is left at its default of 2, which is exactly what
//   this example registers (Ethernet and Wi-Fi). Raise it for a third interface
//   — an LTE modem, say — since the component sizes its /netdata and restore
//   buffer from it, and addProfile() returns false for anything beyond it.
//
//   -DNTP_PROFILE_SERVER_COUNT=3
//       How many NTP servers the device stores. The Time group on the network
//       page renders one row per server, so this also decides how many fields
//       appear. It is a count, 1 to 3.
//
//   -DNTP_PROFILE_ENABLED=0
//       Leaves NTP out of the build altogether — the profile class and
//       NetworkManager's SNTP support with it. A different question from how
//       many servers to store, which is why it is a separate macro.
//
// Two practical notes: the IDE only picks up build_opt.h from the sketch
// folder, so open the example as a folder; and a change to it does NOT take
// effect on the next build, because the IDE serves the libraries from its build
// cache. Force a clean rebuild after editing it — close the IDE, or change any
// board menu option (the upload method, say).
//
// ESP8266: build_opt.h belongs to the ESP32 core and is ignored there. The
// equivalent is a NetConfig.ino.globals.h next to this sketch, which is not
// shipped because the IDE hides an example folder containing one — copy the
// block below into a file of that name. It carries the same flags as
// build_opt.h, plus the three that make TLS fit on an ESP8266 (see the TLS
// section further down); drop those three if MQTT will not use TLS.
//
//     /*@create-file:build.opt@
//     // Fewer NTP servers: each one costs about 300 bytes in the status
//     // document and more in the Network page's, and one is enough here.
//     -DNTP_PROFILE_SERVER_COUNT=1
//
//     -DFIRMWARE_PROJECT="\"netconfig\""
//     -DFIRMWARE_BOARD="\"esp8266\""
//     -DFIRMWARE_VERSION=100
//
//     // Host names are stored per profile and per NTP server, so
//     // the standard's 253-byte maximum is the single biggest lever on RAM
//     // here. 64 is ample for a broker or an NTP pool.
//     -DHOST_FQDN_LEN=64
//
//     // The basic BearSSL cipher set: smaller, and a modern broker will not
//     // pick the ones it drops. (Arduino IDE also offers this as
//     // Tools > SSL Support > "Basic SSL ciphers".)
//     -DBEARSSL_SSL_BASIC
//     */
//
// The whole thing is a C comment as far as the compiler is concerned; the
// ESP8266 core extracts the block into build.opt, skipping blank lines and lines
// starting with '//', '*' or '#'. So comments are allowed here — unlike in
// build_opt.h, which goes to the command line verbatim and must contain flags
// only. Same caveat as that file, though: a change is not noticed on the next
// build, so force a full rebuild after editing.
//
// Do not #include this file from the sketch; the core includes it in every
// translation unit by itself, which is the point.
//
// ESP8266 also needs a flash layout that includes a filesystem: the profiles and
// credentials are stored through Preferences, which is backed by LittleFS there.
// A layout without an FS saves nothing.

// Ethernet PHY. Boards known to the Arduino core (Olimex ESP32-POE,
// ESP32-Gateway, ...) supply these automatically — just select the board. For
// custom hardware, define ETH_PHY_* here before <EthAdapter.h> is included.
#ifdef ARDUINO_ARCH_ESP32
// #define ETH_PHY_TYPE         ETH_PHY_W5500
// #define ETH_PHY_SPI_HOST     SPI3_HOST
// #define ETH_PHY_SPI_FREQ_MHZ 25
// #define ETH_PHY_ADDR          1
// #define ETH_PHY_CS           14
// #define ETH_PHY_IRQ          10
// #define ETH_PHY_RST           9
// #define ETH_PHY_SPI_SCK      13
// #define ETH_PHY_SPI_MISO     12
// #define ETH_PHY_SPI_MOSI     11
#elif defined(ARDUINO_ARCH_ESP8266)
// #define ETH_PHY_TYPE         ETH_PHY_W5500
// #define ETH_PHY_CS           16
#else
#error "Unsupported architecture."
#endif

// Default Wi-Fi transmit power in dBm. Some ESP32-C3 and -S3 boards have chip
// antenna routing that disturbs the oscillator; turning the radio down to about
// 13-15 dBm is a known workaround that makes Wi-Fi usable at all. This only sets
// the default — the value is configurable from the web page afterwards.
// #define WIFI_PROFILE_DEFAULT_WIFI_TX_POWER 13.0f

#include <EthAdapter.h>
#include <WiFiAdapter.h>
#include <NetworkManager.h>
#if NTP_PROFILE_ENABLED            // the header refuses to be included otherwise
#  include <NtpProfile.h>
#endif
#include <HttpDigestAuth.h>

#include <AsyncConfigPortal.h>
#include <NetConfigComponent.h>   // opt-in: this include is what pulls in the
                                  // NetworkProfile family
#include <MqttConfigComponent.h>  // opt-in: the MQTT settings page. Editing them
                                  // needs no MQTT client; connecting does, and
                                  // that stays in this sketch.
// -----------------------------------------------------------------------------
// TLS on ESP8266
//
// It fits, but only just, and only with all three of these together:
//   - a broker that agrees to MFLN, so the receive buffer is 512 bytes instead
//     of 16 KB (Mosquitto does; the sketch probes and reports it);
//   - -DHOST_FQDN_LEN=64, which shrinks the largest portal document — the
//     Network page's /netdata carries a name field per profile and per NTP
//     server, and the standard's 253-byte maximum is not what deployments use;
//   - the basic SSL cipher set, which drops the ciphers a modern broker will not
//     pick anyway. Arduino IDE: Tools > SSL Support > "Basic SSL ciphers".
//     PlatformIO: build_flags = -D BEARSSL_SSL_BASIC.
//
// With those, a device serving this portal holds around 12 KB free and 10 KB
// contiguous — enough. Without them the pages start coming up blank while MQTT
// keeps working, which is the worst outcome: the setting that caused it can no
// longer be changed from the portal. Hence the measured check after connecting.
//
// ESP32 has none of these constraints.
// -----------------------------------------------------------------------------

#if defined(ARDUINO_ARCH_ESP32)
#  include <NetworkClient.h>
#  include <NetworkClientSecure.h>
#elif defined(ARDUINO_ARCH_ESP8266)
#  include <WiFiClientSecure.h>
   // The ESP8266 core has no NetworkClient; the Wi-Fi ones are the same thing
   // here, so the sketch below can be written once for both.
   using NetworkClient       = WiFiClient;
   using NetworkClientSecure = WiFiClientSecure;
#endif
#include <PubSubClient.h>          // hmueller01/pubsubclient3
#include <NetStatusComponent.h>   // opt-in as well, and a second dependency:
                                  // this one needs NetworkManager, which is why
                                  // it is a separate component from NetConfig

#include <atomic>
#ifdef ARDUINO_ARCH_ESP32
#include <esp_log.h>
#endif

// -----------------------------------------------------------------------------
// First-boot defaults (used only when nothing is stored in NVS yet)
// -----------------------------------------------------------------------------

static const char*   DEFAULT_HOSTNAME  = "esp-netconfig";
static const char*   DEFAULT_WIFI_SSID = "your-ssid";
static const char*   DEFAULT_WIFI_PASS = "your-password";
static const uint8_t ETH_PRIORITY      = 0;   // preferred interface
static const uint8_t WIFI_PRIORITY     = 1;   // fallback

// -----------------------------------------------------------------------------
// Objects
// -----------------------------------------------------------------------------

EthProfile  ethProfile;
WiFiProfile wifiProfile;
EthAdapter  ethAdapter(ethProfile);
WiFiAdapter wifiAdapter(wifiProfile);

// The NTP servers belong to the device, not to an interface: one clock, one set
// of servers, wherever the traffic happens to go. NetworkManager applies them on
// connect; NetConfigComponent puts them in a Time group on the network page.
#if NTP_PROFILE_ENABLED
NtpProfile ntpProfile;
static constexpr char NS_NTP[] = "ntp";
#endif

AsyncConfigPortal  web(80);
HttpDigestAuth     auth;
NetConfigComponent netConfig;
NetStatusComponent netStatus;   // read-only live view of the active interface

MqttProfile         mqttProfile;
MqttConfigComponent mqttPage;

// The MQTT client belongs to the application, not to the portal. Both transports
// are instantiated and one is chosen at startup from the stored TLS flag: a live
// PubSubClient cannot have the ground swapped underneath it, which is why
// changing that flag asks for a restart.
NetworkClient       plainClient;
PubSubClient        mqtt(plainClient);

// The TLS client is created only if TLS is actually switched on. On ESP8266 a
// BearSSL client reserves a ~6 KB secondary stack the moment it is instantiated,
// so a global one would spend that even on a device that never uses TLS.
static NetworkClientSecure* tlsClient = nullptr;

// TLS is not judged before it runs: the free heap at the end of setup() says
// nothing useful, because the portal's own peak comes later, when a page is
// actually served. What matters is the largest *contiguous* block left once the
// connection stands — that is what a page response needs — so the check happens
// there, on a measurement rather than a prediction.
//
// Below this, the Network page (the largest document) starts coming up blank,
// which is the same trap as a reboot loop in slow motion: TLS is up, the pages
// are not, and the setting cannot be changed from the portal any more.
static constexpr uint32_t MQTT_TLS_MIN_BLOCK = 8000;

static bool mqttTlsRefused = false;   // switched on, but there was no room

static constexpr char NS_MQTT[] = "mqtt";

// Topic buffers sized from what actually goes into them, rather than from a
// round number: the prefix is fixed, the hostname is not, and a hostname at its
// maximum would otherwise be truncated silently.
static constexpr char   MQTT_PREFIX[]    = "netconfig/";
static constexpr size_t MQTT_BASE_SIZE   = sizeof(MQTT_PREFIX)
                                         + NetworkProfile::MAX_HOSTNAME_LEN;
static constexpr size_t MQTT_TOPIC_SIZE  = MQTT_BASE_SIZE + 8;   // + "/uptime"

static MqttProfile::MqttConfig mqttCfg;    // what this run is using
static char     mqttBase[MQTT_BASE_SIZE]  = {};   // netconfig/<hostname>
static char     mqttWill[MQTT_TOPIC_SIZE] = {};   // <base>/status
static bool     mqttStarted   = false;
static uint32_t mqttLastPub   = 0;
static uint32_t mqttLastTry   = 0;

// Set from the async web server task, read in loop(): atomic rather than
// volatile, which orders nothing between tasks on ESP32.
static std::atomic<bool> s_restartPending{false};

// -----------------------------------------------------------------------------
// Callbacks
// -----------------------------------------------------------------------------

// NetworkManager uses a plain function pointer, not a capturing lambda.
static void onNetworkEvent(NetworkManagerClass::Event event,
                           NetworkAdapter& adapter) {
    switch (event) {
        case NetworkManagerClass::Event::CONNECTED:
            Serial.println(F("net: connected"));
            break;
        case NetworkManagerClass::Event::FALLBACK:
            Serial.println(F("net: primary interface failed - fallback active"));
            break;
        case NetworkManagerClass::Event::RESTORED:
            Serial.println(F("net: primary interface recovered"));
            break;
        case NetworkManagerClass::Event::DISCONNECTED:
            Serial.println(F("net: disconnected - all interfaces failed"));
            return;
        default:
            return;
    }

    NetworkStatus s = NetworkManager.getStatus();
    Serial.print(F("  interface: "));
    Serial.println(s.interfaceType == NetworkProfile::InterfaceType::ETH
                       ? F("ETH") : F("WiFi"));
    Serial.print(F("  address:   ")); Serial.println(s.localIP);

    // The callback always receives the base adapter, so check the type before
    // down-casting to reach the Wi-Fi accessors.
    if (adapter.getProfile().getInterfaceType()
            == NetworkProfile::InterfaceType::WIFI) {
        WiFiAdapter& w = static_cast<WiFiAdapter&>(adapter);
        Serial.print(F("  RSSI:      ")); Serial.print(w.getRssi());
        Serial.println(F(" dBm"));
        Serial.print(F("  TX power:  ")); Serial.print(w.getTxPower(), 2);
        Serial.println(F(" dBm"));
    }
}

#if NTP_PROFILE_ENABLED
void onTimeSync() {
    Serial.println("ntp: synced");
    time_t now = time(nullptr);
    Serial.print("  time:   "); Serial.print(ctime(&now));  // ctime() ends in '\n'

    // The SDK exposes SNTP servers by slot, not which one delivered the sync, so
    // list the active servers — this also reveals whether DHCP-provided or
    // statically configured servers are in use.
    for (uint8_t i = 0; i < NtpProfile::SERVER_COUNT; i++) {
        IPAddress ip = NetworkManager.getActiveNtpIP(i);
        if (ip == IPAddress(0, 0, 0, 0)) continue;   // unset / not yet resolved
        char name[Host::MAX_FQDN_SIZE];
        Serial.print("  server: ");
        if (NetworkManager.getActiveNtpName(i, name, sizeof(name))) {
            Serial.print(name); Serial.print(" (");
            Serial.print(ip);   Serial.println(")");
        } else {
            Serial.println(ip);
        }
    }
}
#endif

// -----------------------------------------------------------------------------
// First-boot profile setup
// -----------------------------------------------------------------------------

static void initEthProfile() {
    if (ethProfile.loadCfg("eth")) {
        Serial.println(F("eth:  saved configuration loaded"));
        return;
    }
    Serial.println(F("eth:  no saved configuration - applying defaults"));

    NetworkProfile::NetworkConfig cfg;
    cfg.dhcp     = true;
    cfg.priority = ETH_PRIORITY;
    snprintf(cfg.hostname, sizeof(cfg.hostname), "%s", DEFAULT_HOSTNAME);

    ethProfile.setConfig(cfg);
    ethProfile.saveCfg("eth");
}

static void initWiFiProfile() {
    if (wifiProfile.loadCfg("wifi")) {
        Serial.println(F("wifi: saved configuration loaded"));
        return;
    }
    Serial.println(F("wifi: no saved configuration - applying defaults"));

    WiFiProfile::WiFiConfig cfg;
    cfg.dhcp     = true;
    cfg.priority = WIFI_PRIORITY;
    snprintf(cfg.hostname, sizeof(cfg.hostname), "%s", DEFAULT_HOSTNAME);
    snprintf(cfg.ssid,     sizeof(cfg.ssid),     "%s", DEFAULT_WIFI_SSID);
    snprintf(cfg.password, sizeof(cfg.password), "%s", DEFAULT_WIFI_PASS);
    // cfg.txPower keeps DEFAULT_WIFI_TX_POWER_dBm; the page can change it.

    wifiProfile.setConfig(cfg);
    wifiProfile.saveCfg("wifi");
}

// -----------------------------------------------------------------------------
// Setup
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// MQTT. Topics are the application's business: they follow from this project's
// naming, and a stored topic layout would be one more thing to keep in step with
// the code publishing to it.
// -----------------------------------------------------------------------------

/// Applies the stored settings to the client. Once per boot — see the note by
/// the client objects.
static void mqttStart() {
    if (!mqttProfile.getConfig(mqttCfg) || !mqttCfg.enabled) return;

    if (mqttCfg.tls) {
        // A secure client is built and measured; refusing outright here would
        // mean guessing, and the guess was wrong in both directions.
        tlsClient = new NetworkClientSecure();
        if (!tlsClient) { mqttTlsRefused = true; return; }

        // A demo, not a deployment: with no CA the link is encrypted but not
        // authenticated. Load one with setCACert()/setTrustAnchors() before
        // trusting it.
        tlsClient->setInsecure();

#if defined(ARDUINO_ARCH_ESP8266)
        // Ask the broker to agree to smaller TLS records. Without this the
        // receive buffer alone is 16 KB, which an ESP8266 running a web server
        // does not have. Not every broker supports it; when it does not, the
        // smaller buffers are still set — messages this sketch exchanges are
        // well under 512 bytes — but a large one from the broker would then fail
        // to be received rather than silently truncate.
        const bool mfln = tlsClient->probeMaxFragmentLength(mqttCfg.host,
                                                            mqttCfg.port, 512);
        Serial.printf("mqtt: MFLN %s\n", mfln ? "agreed" : "not supported by broker");
        tlsClient->setBufferSizes(512, 512);
#endif
        mqtt.setClient(*tlsClient);
    } else {
        mqtt.setClient(plainClient);
    }
    mqtt.setServer(mqttCfg.host, mqttCfg.port);
    mqtt.setKeepAlive(30);

    char host[NetworkProfile::MAX_HOSTNAME_SIZE];
    ethProfile.getHostname(host, sizeof(host));
    snprintf(mqttBase, sizeof(mqttBase), "%s%s", MQTT_PREFIX, host);
    snprintf(mqttWill, sizeof(mqttWill), "%s/status", mqttBase);

    mqttStarted = true;
    Serial.printf("mqtt: %s:%u%s, topics under \"%s\"\n",
                  mqttCfg.host, (unsigned)mqttCfg.port,
                  mqttCfg.tls ? " (TLS)" : "", mqttBase);
}

/// One connection attempt. Credentials go in only when there are any: an
/// anonymous connection is not the same as one with empty strings, and some
/// brokers reject the latter.
static void mqttConnect() {
    const bool ok = mqttCfg.user[0]
        ? mqtt.connect(mqttBase, mqttCfg.user, mqttCfg.password,
                       mqttWill, 0, true, "offline")
        : mqtt.connect(mqttBase, mqttWill, 0, true, "offline");

    if (ok) {
        // Retained, so a subscriber arriving later still learns the device is
        // up — and the will replaces it with "offline" if the link drops.
        mqtt.publish(mqttWill, "online", true);
        Serial.println(F("mqtt: connected"));

        if (mqttCfg.tls) {
            // Now the TLS buffers exist, so this is the real figure.
#if defined(ARDUINO_ARCH_ESP8266)
            const uint32_t block = ESP.getMaxFreeBlockSize();
#else
            const uint32_t block = ESP.getMaxAllocHeap();
#endif
            Serial.printf("mqtt: largest free block with TLS up: %lu bytes\n",
                          (unsigned long)block);
            if (block < MQTT_TLS_MIN_BLOCK) {
                // Back out rather than leave a portal that cannot answer: the
                // setting has to stay reversible from the pages that change it.
                mqtt.publish(mqttWill, "offline", true);
                mqtt.disconnect();
                mqttTlsRefused = true;
                Serial.printf("mqtt: only %lu bytes contiguous, the portal needs "
                              "about %lu to serve its pages — disconnecting so it "
                              "keeps working. Switch TLS off, shorten "
                              "HOST_FQDN_LEN, or build with the basic SSL "
                              "cipher set (-D BEARSSL_SSL_BASIC).\n",
                              (unsigned long)block, (unsigned long)MQTT_TLS_MIN_BLOCK);
            }
        }
    } else {
        Serial.printf("mqtt: connect failed, state %d\n", mqtt.state());
    }
}

/// The MQTT box on the Net status page. Declining leaves the section out
/// entirely, which is how a disabled MQTT disappears from the page rather than
/// showing empty rows.
static bool mqttStatusSection(char* buf, size_t len) {
    if (!mqttCfg.enabled) return false;
    return snprintf(buf, len,
        "{\"Broker\":\"%s:%u\",\"Transport\":\"%s\",\"Authentication\":\"%s\","
        "\"State\":\"%s\",\"Base topic\":\"%s\"}",
        mqttCfg.host, (unsigned)mqttCfg.port,
        mqttCfg.tls      ? "TLS"      : "plain",
        mqttCfg.user[0]  ? mqttCfg.user : "anonymous",
        mqttTlsRefused   ? "TLS declined - not enough memory"
                         : (mqtt.connected() ? "connected" : "offline"),
        mqttBase) < (int)len;
}

void setup() {
    Serial.begin(115200);
    while (!Serial) delay(10);
    delay(500);
    Serial.println(F("\nAsyncConfigPortal NetConfig example"));

    // Credentials store: restore from NVS, or set a default on first boot.
    if (!auth.restore()) {
        auth.setCredentials("admin", "esp32", "admin");
        auth.save();
        Serial.println(F("auth: no stored credentials - defaults set (admin/admin)"));
    }

    initEthProfile();
    initWiFiProfile();

    // Network. The adapters apply their profiles and NetworkManager handles the
    // priority-based failover between them.
    NetworkManager.onEvent(onNetworkEvent);
#if NTP_PROFILE_ENABLED
    NetworkManager.onNtpSync(onTimeSync);
#endif
    NetworkManager.addAdapter(ethAdapter);
    NetworkManager.addAdapter(wifiAdapter);
    NetworkManager.begin();

    // Register both profiles with the config page. The second argument is the
    // NVS namespace the component saves that profile under, so it must match
    // the one used above.
    netConfig.addProfile(ethProfile,  "eth");
    netConfig.addProfile(wifiProfile, "wifi");
#if NTP_PROFILE_ENABLED
    // Without this the Time group simply does not appear, and nothing configures
    // NTP — the portal never assumes a profile it was not given.
    ntpProfile.loadCfg(NS_NTP);
    netConfig.setNtpProfile(ntpProfile, NS_NTP);
    NetworkManager.setNtpProfile(ntpProfile);
#endif

    // Save policy is the application's choice. Here: flag a restart and perform
    // it from loop(), so the HTTP response is flushed before the reboot. A
    // submit that changed nothing reports an empty change set.
    netConfig.onSaved([](const NetConfigComponent::NetChangeSet& changed) {
        if (changed.any()) {
            Serial.println(F("net: configuration changed - restarting shortly"));
            s_restartPending.store(true);
        } else {
            Serial.println(F("net: submit with no changes - ignoring"));
        }
    });

    // The server tags each line with the subsystem that produced it ("ota", ...);
    // ESP_LOGx takes a runtime tag, so the log stays filterable per subsystem.
    web.setLogger([](AsyncConfigPortal::LogLevel lvl, const char* tag,
                     const char* msg) {
        switch (lvl) {
#ifdef ARDUINO_ARCH_ESP32
            case AsyncConfigPortal::LogLevel::Error: ESP_LOGE(tag, "%s", msg); break;
            case AsyncConfigPortal::LogLevel::Warn:  ESP_LOGW(tag, "%s", msg); break;
            case AsyncConfigPortal::LogLevel::Info:  ESP_LOGI(tag, "%s", msg); break;
            case AsyncConfigPortal::LogLevel::Debug: ESP_LOGD(tag, "%s", msg); break;
#elif defined(ARDUINO_ARCH_ESP8266)
            case AsyncConfigPortal::LogLevel::Error: Serial.printf("[E] [%s] %s\n", tag, msg); break;
            case AsyncConfigPortal::LogLevel::Warn:  Serial.printf("[W] [%s] %s\n", tag, msg); break;
            case AsyncConfigPortal::LogLevel::Info:  Serial.printf("[I] [%s] %s\n", tag, msg); break;
            case AsyncConfigPortal::LogLevel::Debug: Serial.printf("[D] [%s] %s\n", tag, msg); break;
#endif
        }
    });

    // attach() registers the /net page, the /netdata endpoint and the POST
    // handler, so it must run before begin() starts the server.
    //
    // The third argument adds a "Network" section to the built-in Backup page:
    // a Download button, and a Restore form that takes the file back. It is off
    // by default, because a backup earns its own menu entry for an elaborate
    // configuration and not for a DHCP checkbox — but this example is exactly
    // where it is worth seeing working.
    //
    // Two things are worth trying once it is running:
    //
    //   * Download, edit the file, upload it back. The whole document is
    //     validated before anything is written, so a mangled file changes
    //     nothing at all rather than applying the half it could read.
    //
    //   * Restore a backup whose SSID differs from the one in use. The device
    //     refuses and says which network's password it needs, because a
    //     backup never contains one — that is what keeps the passphrase
    //     unreadable through the web interface. Supply it in the Restore form,
    //     or untick "requires password" for an open network.
    // attach(portal, menu order, menu label, with a Backup section)
    netConfig.attach(web, AsyncConfigPortal::MENU_NET, "Network", true);
    // Live status next to the configuration: what the device is actually on,
    // read from NetworkManager rather than from the saved profiles. Left open,
    // so checking a signal level needs no login; pass AuthLevel::Required if the
    // SSID, addressing and BSSID should not be readable without one.
    // MQTT settings page, with a Backup section of its own.
    mqttProfile.loadCfg(NS_MQTT);
    mqttPage.setProfile(mqttProfile, NS_MQTT);
    mqttPage.onSaved([](const MqttConfigComponent::Changed& c) {
        // The connection was built from these and cannot be rebuilt underneath
        // itself — least of all the transport, a different object for TLS.
        if (c.connection) { s_restartPending.store(true); return; }
        // Switching it off needs no restart: just stop talking to the broker.
        if (c.enabled) {
            mqttProfile.getConfig(mqttCfg);
            if (!mqttCfg.enabled && mqtt.connected()) {
                mqtt.publish(mqttWill, "offline", true);
                mqtt.disconnect();
                Serial.println(F("mqtt: disabled"));
            } else if (mqttCfg.enabled && !mqttStarted) {
                mqttStart();
            }
        }
    });
    mqttPage.attach(web, AsyncConfigPortal::MENU_NET - 1, "MQTT", true);

    // The status page shows what NetworkManager knows; the MQTT state is this
    // sketch's to report, since it owns the client.
    netStatus.addSection("MQTT", mqttStatusSection);
    netStatus.attach(web);
 


    web.setProject("NetConfig", "0.1.0",
                   "Web-configurable network settings", "2026", "you");

    web.begin(auth);
    Serial.println(F("web: server started - browse to the device address"));

    mqttStart();
}

// -----------------------------------------------------------------------------
// Loop
// -----------------------------------------------------------------------------

void loop() {
    NetworkManager.update();

    // MQTT: connect when the network is up, retry on a timer rather than as fast
    // as loop() runs — a broker that is down should not be hammered, and each
    // failed attempt blocks for the socket timeout.
    if (mqttStarted && mqttCfg.enabled && !mqttTlsRefused
        && NetworkManager.isConnected()) {
        if (!mqtt.connected()) {
            if (millis() - mqttLastTry >= 5000) { mqttLastTry = millis(); mqttConnect(); }
        } else {
            mqtt.loop();
            if (millis() - mqttLastPub >= 10000) {
                mqttLastPub = millis();
                char topic[MQTT_TOPIC_SIZE], payload[16];
                snprintf(topic, sizeof(topic), "%s/uptime", mqttBase);
                snprintf(payload, sizeof(payload), "%lu", (unsigned long)(millis() / 1000));
                mqtt.publish(topic, payload);
            }
        }
    }

    if (s_restartPending.load()) {
        static uint32_t since = 0;
        if (since == 0) since = millis();
        // Give the async server time to flush the "saved" response, otherwise
        // the browser sees a connection reset instead of the confirmation.
        if (millis() - since >= 1000) ESP.restart();
    }

    delay(10);
}
