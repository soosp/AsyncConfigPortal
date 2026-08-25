#pragma once

#include <Arduino.h>
#include <string.h>

/**
 * @file FirmwareMarker.h
 * @brief Embedded firmware identity marker for safe OTA validation.
 *
 * Embeds a small, searchable marker in the firmware image identifying the
 * project, the target board, and a numeric version. During a web OTA upload
 * the incoming image is scanned for this marker and compared against the
 * running firmware's marker, so a firmware built for a different project or a
 * different board is rejected before it can overwrite the working image.
 *
 * Under the Arduino framework the esp_app_desc_t project_name/version fields
 * cannot be set from platformio.ini (they live in the prebuilt core), so this
 * self-contained marker is used instead. The values come from per-[env] build
 * flags:
 *
 *   build_flags =
 *     -DFIRMWARE_PROJECT='"p4-ntp"'
 *     -DFIRMWARE_BOARD='"waveshare-p4-poe"'
 *     -DFIRMWARE_VERSION=10000     ; 1.0.0 encoded as MMmmpp
 *
 * IMPORTANT: this is an integrity/mismatch guard against uploading the wrong
 * file — NOT a cryptographic authenticity check. A determined attacker can
 * forge a matching marker. Genuine authenticity requires Secure Boot v2 +
 * signed images (a later, final-release step). The two are complementary: the
 * marker is the fast "is this the right file" check, Secure Boot is the
 * "is this image authentic" guarantee.
 */

// -----------------------------------------------------------------------------
// Build-flag defaults (so a plain build still compiles; real values come from
// per-[env] build_flags in platformio.ini).
// -----------------------------------------------------------------------------
#ifndef FIRMWARE_PROJECT
#  define FIRMWARE_PROJECT "unknown-project"
#endif
#ifndef FIRMWARE_BOARD
#  define FIRMWARE_BOARD "unknown-board"
#endif
#ifndef FIRMWARE_VERSION
#  define FIRMWARE_VERSION 0
#endif

/**
 * @brief Firmware identity marker, embedded once in the image.
 *
 * The 8-byte magic makes the struct locatable by a byte scan of the first OTA
 * chunk without relying on linker placement. Keep the layout stable — both the
 * running firmware and any image being validated must agree on it.
 */
// Single source of truth for the 8-byte locator signature. Bump the trailing
// digits if the marker layout ever changes incompatibly (e.g. "FWMARK02").
#define FW_MARKER_MAGIC_STR "FWMARK01"

struct FirmwareMarker {
    char     magic[8];     ///< FW_MARKER_MAGIC_STR (no null terminator stored)
    char     project[24];  ///< FIRMWARE_PROJECT, null-padded
    char     board[24];    ///< FIRMWARE_BOARD, null-padded
    uint32_t version;      ///< FIRMWARE_VERSION (e.g. 100 = 0.1.0)
    uint32_t reserved;     ///< reserved, zero
};

/** @brief The 8-byte locator signature (not null-terminated on purpose). */
static constexpr char FW_MARKER_MAGIC[8] = {
    FW_MARKER_MAGIC_STR[0], FW_MARKER_MAGIC_STR[1], FW_MARKER_MAGIC_STR[2],
    FW_MARKER_MAGIC_STR[3], FW_MARKER_MAGIC_STR[4], FW_MARKER_MAGIC_STR[5],
    FW_MARKER_MAGIC_STR[6], FW_MARKER_MAGIC_STR[7]
};

/**
 * @brief The running firmware's marker. `used` keeps it from being stripped;
 *        placed in .rodata so it lands in the DROM image data.
 */
__attribute__((used, section(".rodata")))
inline const FirmwareMarker g_fwMarker = {
    // Char-array init (not a string literal) so the 8-byte magic field carries
    // no null terminator — a string literal "FWMARK01" would be 9 bytes and
    // overflow char[8] under C++17.
    {FW_MARKER_MAGIC_STR[0], FW_MARKER_MAGIC_STR[1], FW_MARKER_MAGIC_STR[2],
     FW_MARKER_MAGIC_STR[3], FW_MARKER_MAGIC_STR[4], FW_MARKER_MAGIC_STR[5],
     FW_MARKER_MAGIC_STR[6], FW_MARKER_MAGIC_STR[7]},
    FIRMWARE_PROJECT,
    FIRMWARE_BOARD,
    FIRMWARE_VERSION,
    0
};

/**
 * @brief Locates a FirmwareMarker in a raw byte buffer (e.g. an OTA chunk).
 *
 * Scans for FW_MARKER_MAGIC and, if found with enough trailing bytes for the
 * full struct, copies it out.
 *
 * @param data Buffer to scan.
 * @param len  Buffer length.
 * @param out  Receives the marker on success.
 * @return true if a complete marker was found.
 */
inline bool fwMarkerFind(const uint8_t* data, size_t len, FirmwareMarker& out) {
    if (!data || len < sizeof(FirmwareMarker)) return false;
    const size_t last = len - sizeof(FirmwareMarker);
    for (size_t i = 0; i <= last; i++) {
        if (memcmp(data + i, FW_MARKER_MAGIC, sizeof(FW_MARKER_MAGIC)) == 0) {
            memcpy(&out, data + i, sizeof(FirmwareMarker));
            return true;
        }
    }
    return false;
}

/**
 * @brief Result of validating an incoming marker against the running one.
 */
enum class FwMatch : uint8_t {
    Ok = 0,           ///< Project and board match — safe to flash
    NoMarker,         ///< No marker found in the image — reject
    ProjectMismatch,  ///< Different project — reject
    BoardMismatch,    ///< Different board — reject
};

/**
 * @brief Compares an image's marker against the running firmware's marker.
 *
 * Version is intentionally NOT enforced here (any version of the same
 * project+board is accepted); an anti-downgrade policy can be layered on top
 * by inspecting @p found.version if desired.
 *
 * @param found A marker extracted from an image via fwMarkerFind().
 * @return FwMatch::Ok only when both project and board match.
 */
inline FwMatch fwMarkerCheck(const FirmwareMarker& found) {
    if (memcmp(found.magic, FW_MARKER_MAGIC, sizeof(FW_MARKER_MAGIC)) != 0) {
        return FwMatch::NoMarker;
    }
    if (strncmp(found.project, g_fwMarker.project, sizeof(found.project)) != 0) {
        return FwMatch::ProjectMismatch;
    }
    if (strncmp(found.board, g_fwMarker.board, sizeof(found.board)) != 0) {
        return FwMatch::BoardMismatch;
    }
    return FwMatch::Ok;
}

/** @brief Human-readable reason string for logging/UI. */
inline const char* fwMatchStr(FwMatch m) {
    switch (m) {
        case FwMatch::Ok:              return "ok";
        case FwMatch::NoMarker:        return "no marker in image";
        case FwMatch::ProjectMismatch: return "wrong project";
        case FwMatch::BoardMismatch:   return "wrong board";
    }
    return "unknown";
}
