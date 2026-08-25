/**
 * Minimal AsyncConfigPortal example — bare server.
 *
 * Serves the built-in Status and Other pages only: no network-config module,
 * and therefore no dependency on the NetworkProfile family. Bring up your own
 * network (ETH.begin / WiFi.begin) before web.begin(); this sketch shows only
 * the config-server setup.
 *
 * Compiles as-is: FirmwareMarker.h provides FIRMWARE_PROJECT/BOARD/VERSION
 * defaults, so no build flags are required for a first run. For a real
 * deployment set those flags so the OTA marker check is meaningful (see README).
 *
 * Default credentials on first boot: admin / admin.
 *
 * build_opt.h next to this sketch carries the firmware-marker identity that the
 * OTA check compares against (FIRMWARE_PROJECT / FIRMWARE_BOARD / FIRMWARE_VERSION).
 * It exists because the Arduino IDE compiles library sources separately and a
 * #define in the sketch would not reach them. Its contents go straight onto the
 * compiler command line, so it holds only flags and may NOT contain comments.
 *
 * ESP8266: build_opt.h is an ESP32-core file and is ignored there. The
 * equivalent is a Minimal.ino.globals.h, which holds the same flags inside a
 * block comment tagged @create-file:build.opt@ (see the ESP8266 core docs). It
 * is not shipped because the IDE hides an example folder that contains one.
 * Since the two cores read different files, this is also how one sketch can
 * carry different flags per platform.
 *
 * ESP8266 also needs a flash layout that includes a filesystem: the credentials
 * are stored through Preferences, which is backed by LittleFS there. Without an
 * FS in the layout the save silently does nothing.
 */
#include <Arduino.h>
#include <AsyncConfigPortal.h>
#include <HttpDigestAuth.h>

AsyncConfigPortal web(80);
HttpDigestAuth    auth;

void setup() {
    Serial.begin(115200);

    // --- Bring up Ethernet or Wi-Fi here for your board ---
    // e.g. ETH.begin(...);  or  WiFi.begin(ssid, pass);

    // Credentials: restore from NVS, or set a default on first boot.
    if (!auth.restore()) {
        auth.setCredentials("admin", "esp32", "admin");
        auth.save();
    }

    web.setProject("Minimal", "0.1.0", "Bare config server", "2026", "you");

    // Optional: route diagnostics somewhere. Silent if never set.
    web.setLogger([](AsyncConfigPortal::LogLevel lvl, const char* tag, const char* msg) {
        Serial.printf("[%s][%d] %s\n", tag, static_cast<int>(lvl), msg);
    });

    web.begin(auth);
}

void loop() {
    delay(100);
}
