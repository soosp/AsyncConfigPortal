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
// The 8-byte locator signature is "FWMARK01" — but that byte run may exist
// NOWHERE in an image except inside real markers, or it is a decoy an
// incoming-image scan can find first (see FW_MARKER_MAGIC_X below). So there
// is no string literal and no plain array for it anywhere in this header:
// the signature is stored XOR-ed and decoded where needed. Bump the trailing
// digit (the last byte, '1' -> '2') if the layout ever changes incompatibly.

struct FirmwareMarker {
    char     magic[8];     ///< "FWMARK01" (no null terminator stored)
    char     project[24];  ///< FIRMWARE_PROJECT, null-padded
    char     board[24];    ///< FIRMWARE_BOARD, null-padded
    uint32_t version;      ///< FIRMWARE_VERSION (e.g. 100 = 0.1.0)
    uint32_t crc;          ///< CRC-32 of the fields above (fwMarkerCrcOf);
                           ///< 0 in images from before 0.18.3, which never
                           ///< wrote the field (it was reserved, zero)
};

/**
 * @brief The locator signature, stored transformed (each byte XOR 0x5A).
 *
 * The scanner's comparison constant must not itself read as the magic: a
 * plain 8-byte array lands in .rodata as a contiguous "FWMARK01", and in any
 * image that carries the OTA scanner it is a decoy — whichever of the decoy
 * and the real marker the linker places first is the one an incoming-image
 * scan finds. Builds of 2026-11-04 placed the decoy first on the classic
 * ESP32 and every OTA of them was refused with empty marker fields
 * ("marker: wrong project (project= board=)"). Stored XOR-ed, the plain
 * bytes exist only in real markers, so an image built with this header scans
 * correctly under old scanners too, wherever the linker puts things.
 */
static constexpr uint8_t FW_MARKER_XORK = 0x5A;
static constexpr char FW_MARKER_MAGIC_X[8] = {
    0x1C,   // 'F' ^ 0x5A
    0x0D,   // 'W' ^ 0x5A
    0x17,   // 'M' ^ 0x5A
    0x1B,   // 'A' ^ 0x5A
    0x08,   // 'R' ^ 0x5A
    0x11,   // 'K' ^ 0x5A
    0x6A,   // '0' ^ 0x5A
    0x6B    // '1' ^ 0x5A
};

/** @brief Whether the 8 bytes at @p p are the magic, without a plain copy. */
inline bool fwMagicAt(const uint8_t* p) {
    for (size_t i = 0; i < sizeof(FW_MARKER_MAGIC_X); i++) {
        if ((char)(p[i] ^ FW_MARKER_XORK) != FW_MARKER_MAGIC_X[i]) return false;
    }
    return true;
}

/** @brief One byte into a CRC-32 (reflected, polynomial 0xEDB88320). */
inline constexpr uint32_t fwCrcStep(uint32_t crc, char byte) {
    crc ^= (uint8_t)byte;
    for (int b = 0; b < 8; b++) {
        crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
    }
    return crc;
}

/**
 * @brief The CRC-32 a marker with these fields carries: the magic, the two
 *        names as the 24-byte null-padded fields they are stored in, the
 *        version as its four little-endian bytes, and the crc field as four
 *        zero bytes.
 *
 * Field-wise rather than over the struct's bytes, so the same code runs at
 * compile time (g_fwMarker's initializer), on the device and in the host
 * harness, independent of byte order and without reading past a name's
 * terminator. Bitwise, no table: it runs over 64 bytes once at boot and once
 * per scanned candidate.
 */
inline constexpr uint32_t fwMarkerCrcOf(const char* project, const char* board,
                                        uint32_t version) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < sizeof(FW_MARKER_MAGIC_X); i++) {
        c = fwCrcStep(c, (char)(FW_MARKER_MAGIC_X[i] ^ FW_MARKER_XORK));
    }
    bool end = false;
    for (size_t i = 0; i < 24; i++) {
        const char ch = end ? '\0' : project[i];
        if (ch == '\0') end = true;
        c = fwCrcStep(c, ch);
    }
    end = false;
    for (size_t i = 0; i < 24; i++) {
        const char ch = end ? '\0' : board[i];
        if (ch == '\0') end = true;
        c = fwCrcStep(c, ch);
    }
    for (int i = 0; i < 4; i++) c = fwCrcStep(c, (char)((version >> (8 * i)) & 0xFF));
    for (int i = 0; i < 4; i++) c = fwCrcStep(c, '\0');   // the crc field itself
    return c ^ 0xFFFFFFFFu;
}

/** @brief The CRC a marker read from an image should carry, its own crc
 *         field taken as zero. A name of the full 24 bytes needs no
 *         terminator: the walk reads at most the 24 stored bytes. */
inline uint32_t fwMarkerCrc(const FirmwareMarker& m) {
    return fwMarkerCrcOf(m.project, m.board, m.version);
}

/**
 * @brief The running firmware's marker. `used` keeps it from being stripped;
 *        placed in .rodata so it lands in the DROM image data.
 */
/** @brief This build's marker CRC, computed at compile time. */
static constexpr uint32_t FW_MARKER_CRC =
    fwMarkerCrcOf(FIRMWARE_PROJECT, FIRMWARE_BOARD, FIRMWARE_VERSION);

__attribute__((used, section(".rodata")))
inline const FirmwareMarker g_fwMarker = {
    // Decoded from the XOR-ed signature at compile time (a static
    // initializer), so the plain bytes appear in the image here and only
    // here. A string literal would be 9 bytes (the terminator) and would
    // also put a second plain copy in .rodata on toolchains that emit
    // indexed literals.
    {(char)(FW_MARKER_MAGIC_X[0] ^ FW_MARKER_XORK),
     (char)(FW_MARKER_MAGIC_X[1] ^ FW_MARKER_XORK),
     (char)(FW_MARKER_MAGIC_X[2] ^ FW_MARKER_XORK),
     (char)(FW_MARKER_MAGIC_X[3] ^ FW_MARKER_XORK),
     (char)(FW_MARKER_MAGIC_X[4] ^ FW_MARKER_XORK),
     (char)(FW_MARKER_MAGIC_X[5] ^ FW_MARKER_XORK),
     (char)(FW_MARKER_MAGIC_X[6] ^ FW_MARKER_XORK),
     (char)(FW_MARKER_MAGIC_X[7] ^ FW_MARKER_XORK)},
    FIRMWARE_PROJECT,
    FIRMWARE_BOARD,
    FIRMWARE_VERSION,
    FW_MARKER_CRC
};

/**
 * @brief Locates the next FirmwareMarker in a raw byte buffer, from an offset.
 *
 * Scans for the magic from @p from and, if found with enough trailing bytes
 * for the full struct, copies it out and leaves its offset in @p from. The
 * caller judges the marker and, when it does not match, scans on from
 * `from + 1`: stray magic bytes in an image (an old scanner's comparison
 * constant, data) must not shadow the real marker.
 *
 * @param data Buffer to scan.
 * @param len  Buffer length.
 * @param from In: first offset to consider. Out: the found marker's offset.
 * @param out  Receives the marker on success.
 * @return true if a complete marker was found at or after @p from.
 */
inline bool fwMarkerFindFrom(const uint8_t* data, size_t len, size_t& from,
                             FirmwareMarker& out) {
    if (!data || len < sizeof(FirmwareMarker)) return false;
    const size_t last = len - sizeof(FirmwareMarker);
    for (size_t i = from; i <= last; i++) {
        if (fwMagicAt(data + i)) {
            memcpy(&out, data + i, sizeof(FirmwareMarker));
            from = i;
            return true;
        }
    }
    return false;
}

/**
 * @brief Locates the first FirmwareMarker in a raw byte buffer.
 *
 * @param data Buffer to scan.
 * @param len  Buffer length.
 * @param out  Receives the marker on success.
 * @return true if a complete marker was found.
 */
inline bool fwMarkerFind(const uint8_t* data, size_t len, FirmwareMarker& out) {
    size_t from = 0;
    return fwMarkerFindFrom(data, len, from, out);
}

/**
 * @brief Result of validating an incoming marker against the running one.
 */
enum class FwMatch : uint8_t {
    Ok = 0,           ///< Project and board match — safe to flash
    NoMarker,         ///< No marker found in the image — reject
    ProjectMismatch,  ///< Different project — reject
    BoardMismatch,    ///< Different board — reject
    BadChecksum,      ///< Magic bytes whose CRC does not check: stray bytes
                      ///< that look like a marker, not a marker — scan on
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
    if (!fwMagicAt(reinterpret_cast<const uint8_t*>(found.magic))) {
        return FwMatch::NoMarker;
    }
    // A marker from 0.18.3 on carries its own CRC. The field reads 0 in
    // images from before (it was reserved, zero), and those markers are
    // taken on their fields, as they always were; a nonzero value that does
    // not check marks stray bytes, not a marker.
    if (found.crc != 0 && found.crc != fwMarkerCrc(found)) {
        return FwMatch::BadChecksum;
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
        case FwMatch::BadChecksum:     return "not a marker (bad checksum)";
    }
    return "unknown";
}
