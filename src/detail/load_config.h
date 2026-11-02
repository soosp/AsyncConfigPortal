#pragma once
/**
 * @file load_config.h
 * @brief How much of the heap a page load may take at once.
 *
 * Internal implementation detail — do not include directly.
 *
 * On ESP8266 every request in flight holds about 2.5–3 kB: the request and
 * response objects, the connection, and a receive buffer. A page loads its HTML
 * and two assets, then asks for /menu, /project, /chkauth and its own data, and
 * the browser sends all of those at once. Measured on a NodeMCU running a full
 * application (Geiger counter, MQTT, Home Assistant), the Other page took the
 * free heap from 21 kB to 4.5 kB, the largest block to 4.3 kB. One more client,
 * or an MQTT announcement at that moment, and allocations fail; connections
 * then stall holding their memory, the browser retries, and the device stops
 * accepting connections while its MQTT session carries on.
 *
 * Two limits, both off on ESP32, where the heap takes a page load many times
 * over:
 *
 * CONFIG_PORTAL_SERIALIZED_JS_REQUESTS — 1: the requests a page's own script
 * makes (getJSON() and the heartbeat) go out one at a time, each when the one
 * before has ended; 0: all at once. The browser's own fetches of the HTML, /css
 * and /common.js are not the script's to order and are not affected, and nor
 * are form posts.
 *
 * CONFIG_PORTAL_MIN_FREE_BLOCK — a GET that arrives while the largest free block
 * is below this many bytes is answered 503 at once, before a handler builds
 * anything. Refusing one request is cheap; running out serves no one. getJSON()
 * retries a 503 after a second, up to three times. 0 turns the check off.
 */

#ifndef CONFIG_PORTAL_SERIALIZED_JS_REQUESTS
#  if defined(ARDUINO_ARCH_ESP8266)
#    define CONFIG_PORTAL_SERIALIZED_JS_REQUESTS 1
#  else
#    define CONFIG_PORTAL_SERIALIZED_JS_REQUESTS 0
#  endif
#endif

// The script's limit on requests in flight: 1, or 0 for none.
#if CONFIG_PORTAL_SERIALIZED_JS_REQUESTS
#  define ACP_DETAIL_JS_QMAX "1"
#else
#  define ACP_DETAIL_JS_QMAX "0"
#endif

#ifndef CONFIG_PORTAL_MIN_FREE_BLOCK
#  if defined(ARDUINO_ARCH_ESP8266)
#    define CONFIG_PORTAL_MIN_FREE_BLOCK 6144
#  else
#    define CONFIG_PORTAL_MIN_FREE_BLOCK 0
#  endif
#endif
