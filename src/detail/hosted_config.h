#pragma once
/**
 * @file hosted_config.h
 * @brief Decides whether the portal carries the ESP-Hosted co-processor section.
 *
 * Internal implementation detail — do not include directly.
 *
 * CONFIG_ESP_HOSTED_ENABLED alone does not say a board has a co-processor: the
 * Arduino core ships one prebuilt set of libraries per chip, and on the
 * ESP32-P4 it is on in every build, Wi-Fi-less boards included. What says so
 * is the board variant, which defines BOARD_HAS_SDIO_ESP_HOSTED where one is
 * fitted (esp32p4, esp32p4_core_board, dfrobot_firebeetle2_esp32p4,
 * m5stack_tab5 in core 3.3.12) and not where it is not (waveshare_p4_poe_eth).
 * Both are reached through Arduino.h: sdkconfig.h and pins_arduino.h.
 *
 * Default: on where both hold. =1 turns it on with ESP-Hosted but without the
 * variant's word for it (a board of one's own, pins set with WiFi.setPins()).
 * =1 without ESP-Hosted warns and stays off; =0 turns it off everywhere.
 *
 * CONFIG_PORTAL_HOSTED_FETCH=1: the device can also download the image from
 * Espressif itself (a "Download and update" button). Off by default: it brings
 * the TLS client and HTTPClient into the build, ~310 KB of flash on an
 * ESP32-P4 whose application uses no TLS otherwise (measured) — more than the
 * default app partition can spare. Needs the co-processor section; warns and
 * stays off without it.
 */
#include <Arduino.h>

#ifndef CONFIG_PORTAL_HOSTED_OTA
#  if defined(CONFIG_ESP_HOSTED_ENABLED) && defined(BOARD_HAS_SDIO_ESP_HOSTED)
#    define CONFIG_PORTAL_HOSTED_OTA 1
#  else
#    define CONFIG_PORTAL_HOSTED_OTA 0
#  endif
#elif CONFIG_PORTAL_HOSTED_OTA && !defined(CONFIG_ESP_HOSTED_ENABLED)
#  warning "CONFIG_PORTAL_HOSTED_OTA=1 has no effect: this build has no ESP-Hosted"
#  undef  CONFIG_PORTAL_HOSTED_OTA
#  define CONFIG_PORTAL_HOSTED_OTA 0
#endif

#ifndef CONFIG_PORTAL_HOSTED_FETCH
#  define CONFIG_PORTAL_HOSTED_FETCH 0
#elif CONFIG_PORTAL_HOSTED_FETCH && !CONFIG_PORTAL_HOSTED_OTA
#  warning "CONFIG_PORTAL_HOSTED_FETCH=1 has no effect: the co-processor section is off"
#  undef  CONFIG_PORTAL_HOSTED_FETCH
#  define CONFIG_PORTAL_HOSTED_FETCH 0
#endif

#include <stddef.h>

namespace acp_detail {
/**
 * @brief Length of a printf format with its %s and %lu conversions taken out:
 *        the fixed text a buffer has to hold besides the arguments. Knows only
 *        those two, which is all the formats it is used on contain.
 */
constexpr size_t fmtFixedLen(const char* f) {
    size_t n = 0;
    while (*f) {
        if (f[0] == '%' && f[1] == 's') { f += 2; continue; }
        if (f[0] == '%' && f[1] == 'l' && f[2] == 'u') { f += 3; continue; }
        n++; f++;
    }
    return n;
}
}  // namespace acp_detail
