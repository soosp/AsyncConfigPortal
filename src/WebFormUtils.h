#pragma once

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

/**
 * @file WebFormUtils.h
 * @brief String-free helpers for reading POST body fields from ESPAsyncWebServer.
 *
 * ESPAsyncWebServer exposes POST body parameters via getParam(name, true)
 * and returns their values as Arduino String. To keep our own code free of the
 * String type (for heap-fragmentation determinism, important for long-running
 * industrial deployments), these helpers confine the library's String to a
 * single line: the value is copied straight into a caller-provided fixed buffer
 * and the String is released. Nothing outside these functions ever holds one.
 *
 * Note: name lookups MUST use the (name, true) form for POST bodies; the
 * hasArg()/arg(name) compatibility path only reliably sees GET query params.
 */

// Textual length of an IP address, excluding the null terminator. IPv4 needs 15
// ("255.255.255.255"); 45 also covers the longest IPv6 form. As elsewhere, the
// knob is the *_LEN value and the *_SIZE constant is that plus one.
#ifndef CONFIG_PORTAL_FORM_IP_STR_LEN
#  define CONFIG_PORTAL_FORM_IP_STR_LEN 45
#endif

static constexpr size_t PORTAL_FORM_IP_STR_LEN  = CONFIG_PORTAL_FORM_IP_STR_LEN;
static constexpr size_t PORTAL_FORM_IP_STR_SIZE = PORTAL_FORM_IP_STR_LEN + 1;

/**
 * @brief Returns true if a POST body field with the given name is present.
 */
inline bool postHas(AsyncWebServerRequest* req, const char* name) {
    return req->hasParam(name, /*post=*/true);
}

/**
 * @brief Copies a POST body field's value into a fixed buffer (String-free).
 *
 * The library String returned by value() lives and dies inside this call; we
 * immediately copy it to @p buf via snprintf and never expose it.
 *
 * @param req  The request.
 * @param name Field name (POST body).
 * @param buf  Destination buffer; always null-terminated if len > 0.
 * @param len  Size of the destination buffer.
 * @return Length of the copied string, or -1 if the field is absent (buffer is
 *         set to an empty string in that case).
 */
inline int postVal(AsyncWebServerRequest* req, const char* name,
                   char* buf, size_t len) {
    const AsyncWebParameter* p = req->getParam(name, /*post=*/true);
    if (!p) {
        if (len) buf[0] = '\0';
        return -1;
    }
    // The String from value() is confined to this statement: copied out to a
    // C-string buffer immediately, then released when the expression ends.
    snprintf(buf, len, "%s", p->value().c_str());
    return (int)strlen(buf);
}

/**
 * @brief Reads a POST field and parses it as an IPAddress (String-free).
 *
 * @return true if the field was present and parsed into @p out.
 */
inline bool postIp(AsyncWebServerRequest* req, const char* name, IPAddress& out) {
    char v[PORTAL_FORM_IP_STR_SIZE];
    if (postVal(req, name, v, sizeof(v)) < 0) return false;
    return out.fromString(v);
}
