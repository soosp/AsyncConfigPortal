// EthConfigButton.ino
//
// Getting a headless Ethernet device configured, on a network that may have no
// DHCP server and where nobody knows the device's address.
//
// Two ways in, for the two situations that actually occur:
//
//   * A NEW OR FACTORY-RESET DEVICE has nothing in flash. It tries DHCP, and if
//     none answers within the reconnect timeout it settles on CONFIG_IP. No
//     button, nobody present — a device out of the box comes up findable.
//
//   * A DEPLOYED DEVICE keeps whatever it was configured with, whatever happens
//     to the network. If it was set to DHCP and the server disappears, it stays
//     put and waits: it is in service, and wandering off its address would be the
//     wrong answer. To reconfigure it you go to it and PRESS THE BUTTON, which
//     moves it to CONFIG_IP for as long as the visit lasts.
//
// Either way: plug a laptop into the same switch, give it an address in the same
// range (192.168.4.2/24), browse to http://192.168.4.1, set the fixed address the
// network needs, save. The device restarts on the saved configuration — and from
// then on it is a deployed device, so only the button will move it again.
//
// This is the wired counterpart of AsyncWiFiPortal's reboot-into-portal pattern,
// and it needs a good deal less. Wi-Fi has to switch the radio to AP mode, run a
// captive DNS, and restart to get out of it, so it keeps a persisted state
// machine across boots. Ethernet has none of that: the wire is already there, the
// portal is already running, and only the addressing has to change — which
// NetworkManager can do while it runs. Nothing is written to flash to enter
// config mode, so nothing can strand the device in it, and a reset always returns
// to the saved configuration.
//
// Config mode ends by saving on the Network page, by a second button press, or by
// CONFIG_MODE_TIMEOUT_MS — so a button pressed by accident, or in a cabinet by a
// sleeve, does not leave the device on an address nobody is looking for.
//
// Nothing here calls saveCfg(). The only way into the provisioned state is saving
// on the Network page, and that is deliberate: a config address that saved itself
// would make the device provisioned without anyone having configured it, and by
// the rule above it would then never move on its own again — stuck on an address
// its network has no use for.
//
// WHAT THE BUTTON DOES NOT COVER, and what a real product should add.
//
// An address that is wrong for the network is already handled: the device comes
// up, the button moves it to CONFIG_IP, the portal fixes it. A forgotten portal
// password is not. The Other page has a factory reset that clears the profiles
// and the credentials — putting the defaults this sketch sets on the next boot —
// but reaching that page needs a login, which is exactly what is missing.
//
// The usual answer is a long press on this same button, wired to the same
// erasures the portal's reset performs:
//
//     ethProfile.clearCfg(NS_ETH);   // unprovisioned again: back to CONFIG_IP
//     auth.clear();                  // next boot restores admin/admin below
//     ESP.restart();
//
// Deliberately not implemented here. How long a press should be, whether it needs
// a second confirmation, and whether a device in a public place should have such
// a button at all are product decisions, not library ones — and a portal cannot
// make them for the device it runs on.
//
// Dependencies: as the NetConfig example. See that sketch's header for the
// lib_deps list and the lib_ldf_mode note.

// -----------------------------------------------------------------------------
// Compile-time board configuration — must precede the adapter include
// -----------------------------------------------------------------------------

/* WIZnet W5500 PHY (SPI) — ESP32. See the Ethernet example for other PHYs. */
// #define ETH_PHY_TYPE         ETH_PHY_W5500
// #define ETH_PHY_SPI_HOST     SPI3_HOST
// #define ETH_PHY_ADDR          1
// #define ETH_PHY_CS           13
// #define ETH_PHY_IRQ          12
// #define ETH_PHY_RST          14
// #define ETH_PHY_SPI_SCK      15
// #define ETH_PHY_SPI_MISO     11
// #define ETH_PHY_SPI_MOSI     16

#include <Arduino.h>
#include <EthAdapter.h>
#include <NetworkManager.h>
#include <HttpDigestAuth.h>
#include <AsyncConfigPortal.h>
#include <NetConfigComponent.h>

// The button handler must live in IRAM; the attribute differs by core.
#ifdef ARDUINO_ARCH_ESP32
#  define ISR_ATTR ARDUINO_ISR_ATTR
#elif defined(ARDUINO_ARCH_ESP8266)
#  define ISR_ATTR IRAM_ATTR
#else
#  error "This sketch targets ESP32 and ESP8266."
#endif

// -----------------------------------------------------------------------------
// Configuration
// -----------------------------------------------------------------------------

static const int BUTTON_PIN = 0;        // GPIO0 = BOOT button on most dev boards

// Where the device goes in config mode. Pick a subnet you do not otherwise use,
// so a laptop configured for it cannot collide with the real network. A gateway
// of 0.0.0.0 is valid and correct here: this address only ever serves a directly
// attached machine, and there is nowhere to route.
static const IPAddress CONFIG_IP  (192, 168,   4,   1);
static const IPAddress CONFIG_MASK(255, 255, 255,   0);

// Long enough to find a cable and a browser, short enough that an accidental
// press does not outlive the visit.
static const uint32_t CONFIG_MODE_TIMEOUT_MS = 10UL * 60 * 1000;

static const char NS_ETH[] = "eth";

// -----------------------------------------------------------------------------
// Objects
// -----------------------------------------------------------------------------

EthProfile ethProfile;
EthAdapter ethAdapter(ethProfile);

AsyncConfigPortal  web(80);
HttpDigestAuth     auth;
NetConfigComponent netConfig;

// Set in the ISR, acted on in loop(): entering config mode rewrites a profile and
// restarts an interface, neither of which belongs in an interrupt handler.
static volatile bool buttonPressed = false;

static bool     provisioned    = false;   // a saved configuration was found at boot
static bool     configMode     = false;   // on CONFIG_IP, by button or by default
static bool     byButton       = false;   // ... and it was the button that asked
static uint32_t configModeFrom = 0;       // millis() when a button press started it
static bool     wasConnected   = false;   // isConnected() as of the previous loop()
static bool     switchPending  = false;   // the next rising edge is one we caused
static bool     restartPending = false;

void ISR_ATTR buttonISR() { buttonPressed = true; }

// -----------------------------------------------------------------------------
// Addressing
// -----------------------------------------------------------------------------

// Rewrites the profile and asks NetworkManager to restart the interface with it.
//
// setConfig() writes the profile in memory only — saveCfg() is what persists, and
// it is deliberately never called here. The config address is temporary by
// construction: a reset returns the device to whatever the portal saved, and no
// failure of this sketch can make it permanent.
static bool applyAddressing(bool useConfigAddress) {
    NetworkProfile::NetworkConfig cfg;

    if (useConfigAddress) {
        if (!ethProfile.getConfig(cfg)) {
            Serial.println(F("  getConfig failed (mutex timeout); unchanged"));
            return false;
        }
        cfg.dhcp    = false;
        cfg.ip      = CONFIG_IP;
        cfg.mask    = CONFIG_MASK;
        cfg.gateway = IPAddress();
        for (uint8_t i = 0; i < NetworkProfile::DNS_SERVER_COUNT; i++)
            cfg.dns[i] = IPAddress();
        if (!ethProfile.setConfig(cfg)) {
            Serial.print(F("  config address rejected: "));
            Serial.println(NetworkProfile::configCheckName(
                               NetworkProfile::checkConfig(cfg)));
            return false;
        }
    } else {
        // Back to what is stored. Reloading rather than editing the struct: the
        // saved profile is the truth, and the config address never reached it.
        if (!ethProfile.loadCfg(NS_ETH) && !ethProfile.setConfig(cfg)) {
            Serial.println(F("  nothing saved and defaults rejected; unchanged"));
            return false;                    // cfg is default-constructed: DHCP
        }
    }

    switchPending = true;                    // do not read our own restart as a link event
    NetworkManager.applyProfile(ethAdapter); // stop() + start() with the new config
    return true;
}

static void onButton() {
    if (configMode) {                        // second press: leave again
        Serial.println(F("Config mode: cancelled by button."));
        configMode = byButton = false;
        applyAddressing(false);
        return;
    }
    Serial.println(F("Config mode: taking the configuration address."));
    configMode     = true;
    byButton       = true;                   // this one is timed; the other is not
    configModeFrom = millis();
    applyAddressing(true);
}

// -----------------------------------------------------------------------------
// Reporting
// -----------------------------------------------------------------------------

// Driven from the link edge in loop(), not from the CONNECTED event: with one
// interface the manager reports a change of interface, and a reconnect on the
// only adapter is not one — except right after DISCONNECTED, which makes the next
// connect look cold. Listening to both would report some transitions twice. The
// edge happens exactly once per connect, whichever way it came about.
static void reportStatus() {
    NetworkStatus s = NetworkManager.getStatus();
    Serial.println(configMode ? F("Network: up on the CONFIG address")
                              : F("Network: up"));

    char host[NetworkProfile::MAX_HOSTNAME_SIZE];
    if (NetworkManager.getHostname(host, sizeof(host))) {
        Serial.print(F("  Host:    ")); Serial.println(host);
    }
    Serial.print(F("  IP:      ")); Serial.println(s.localIP);
    Serial.print(F("  Netmask: ")); Serial.println(s.subnetMask);
    if (s.gateway != IPAddress(0, 0, 0, 0)) {
        Serial.print(F("  Gateway: ")); Serial.println(s.gateway);
    }
    if (configMode) {
        Serial.println(F("  Give a directly attached machine 192.168.4.2/24"));
        Serial.println(F("  and browse to http://192.168.4.1 (admin/admin)."));
        Serial.print(F("  Leaving config mode after "));
        Serial.print((unsigned long)(CONFIG_MODE_TIMEOUT_MS / 60000));
        Serial.println(F(" minutes if nothing is saved."));
    }
}

// -----------------------------------------------------------------------------
// Events
// -----------------------------------------------------------------------------

void onNetworkEvent(NetworkManagerClass::Event event, NetworkAdapter& /*adapter*/) {
    if (event != NetworkManagerClass::Event::DISCONNECTED) return;

    Serial.println(F("Network: disconnected"));

    // Only an unprovisioned device reaches for CONFIG_IP on its own. A deployed
    // one stays on what it was given: it is in service, someone chose that
    // addressing, and the answer to a network that changed under it is a visit
    // with the button — not an address of our own choosing.
    if (provisioned) {
        Serial.println(F("  Configured device - staying put. Press the button to reconfigure."));
        return;
    }
    if (configMode) {
        Serial.println(F("  Config address did not come up either (cable?)."));
        return;
    }
    Serial.println(F("  Unconfigured and no DHCP - taking the configuration address."));
    configMode = true;
    byButton   = false;                      // no timeout: there is nothing to go back to
    applyAddressing(true);
}

// -----------------------------------------------------------------------------
// Setup
// -----------------------------------------------------------------------------

void setup() {
    Serial.begin(115200);
    while (!Serial) delay(10);
    Serial.println(F("\nEthernet config-button example starts."));

    pinMode(BUTTON_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), buttonISR, FALLING);

    // Saved configuration if there is one, DHCP if not. The config address is
    // never among the candidates: it is a runtime decision, not a stored one.
    provisioned = ethProfile.loadCfg(NS_ETH);
    if (!provisioned) {
        NetworkProfile::NetworkConfig cfg;    // defaults: DHCP, priority 0
        if (!ethProfile.setConfig(cfg)) {
            Serial.println(F("Default configuration rejected - halting."));
            while (true) delay(1000);
        }
        Serial.println(F("Unconfigured device - trying DHCP, then the config address."));
    } else {
        Serial.println(F("Saved configuration loaded."));
    }

    NetworkManager.onEvent(onNetworkEvent);
    NetworkManager.addAdapter(ethAdapter);
    NetworkManager.begin();

    // The portal runs from the start, on whatever address the device has. The
    // button does not start it — it makes it findable.
    // Credentials store: restore from NVS, or set a default on first boot.
    if (!auth.restore()) {
        auth.setCredentials("admin", "esp32", "admin");
        auth.save();
        Serial.println(F("auth: no stored credentials - defaults set (admin/admin)"));
    }

    netConfig.addProfile(ethProfile, NS_ETH);
    netConfig.onSaved([](const NetConfigComponent::NetChangeSet& changed) {
        if (!changed.any()) {
            Serial.println(F("net: submit with no changes - ignoring"));
            return;
        }
        // Restart rather than re-apply: the saved settings are what should be
        // running, and a restart is the shortest way to be sure nothing of the
        // config address survives.
        // The device is provisioned from here on: after the restart it loads
        // this and never takes the config address by itself again.
        Serial.println(F("net: configuration saved - restarting shortly"));
        restartPending = true;
    });
    netConfig.attach(web, AsyncConfigPortal::MENU_NET, "Network", true);
    web.begin(auth);

    Serial.print(F("Press the button on GPIO"));
    Serial.print(BUTTON_PIN);
    Serial.println(F(" to move to the configuration address."));
}

// -----------------------------------------------------------------------------
// Loop
// -----------------------------------------------------------------------------

void loop() {
    NetworkManager.update();

    if (buttonPressed) {
        buttonPressed = false;
        onButton();
    }

    // Rising edge of connectivity. Our own restarts produce one too, so those are
    // filtered out; the rest is the link coming back on its own.
    const bool nowConnected = NetworkManager.isConnected();
    if (nowConnected && !wasConnected) {
        switchPending = false;
        reportStatus();
    }
    wasConnected = nowConnected;

    // Only a button press is timed. An unconfigured device has nothing to return
    // to, so leaving CONFIG_IP would only make it unreachable again.
    if (configMode && byButton && millis() - configModeFrom > CONFIG_MODE_TIMEOUT_MS) {
        Serial.println(F("Config mode: timed out, returning to the saved configuration."));
        configMode = byButton = false;
        applyAddressing(false);
    }

    if (restartPending) {
        delay(1500);              // let the browser receive the "saved" page
        ESP.restart();
    }

    delay(10);
}
