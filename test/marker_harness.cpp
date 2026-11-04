// Host test suite for FirmwareMarker.h (the OTA identity scan).
//
//   make marker     build and run
//
// The regression of 2026-11-04: the scanner's comparison constant was a plain
// 8-byte array, so every image carrying the scanner held a decoy magic with
// empty fields, and whichever of the decoy and the real marker the linker
// placed first decided the scan. The suite drives the scan across decoys,
// orders and chunk offsets — and the Makefile greps this binary to prove the
// plain magic occurs in it exactly once: g_fwMarker and nothing else.

#define FIRMWARE_PROJECT "test-project"
#define FIRMWARE_BOARD   "test-board"
#define FIRMWARE_VERSION 100

#include <cstdio>
#include <cstring>
#include "FirmwareMarker.h"

static int g_fail = 0;
static int g_run  = 0;

static void check(bool ok, const char* what, int line) {
    g_run++;
    if (!ok) { g_fail++; printf("  FAIL line %d: %s\n", line, what); }
}
#define CHECK(c, what) check((c), (what), __LINE__)

// A buffer with the magic written at off (built at run time, byte by byte,
// so the test data itself puts no plain magic into this binary's rodata),
// optionally followed by project/board fields.
static void putMagic(uint8_t* buf, size_t off) {
    for (size_t i = 0; i < sizeof(FW_MARKER_MAGIC_X); i++)
        buf[off + i] = (uint8_t)(FW_MARKER_MAGIC_X[i] ^ FW_MARKER_XORK);
}
static void putMarker(uint8_t* buf, size_t off, const char* project,
                      const char* board, bool withCrc = true) {
    FirmwareMarker m{};
    putMagic(reinterpret_cast<uint8_t*>(&m), 0);
    strncpy(m.project, project, sizeof(m.project) - 1);
    strncpy(m.board, board, sizeof(m.board) - 1);
    m.version = 100;
    // withCrc=false writes a pre-0.18.3 marker: the field was reserved, zero.
    m.crc = withCrc ? fwMarkerCrcOf(m.project, m.board, m.version) : 0;
    memcpy(buf + off, &m, sizeof m);
}

// The scan loop as AsyncConfigPortal.cpp runs it: every candidate judged,
// mismatches scanned past, the verdict the best seen.
static bool scanMatches(const uint8_t* buf, size_t len, int* candidates = nullptr) {
    FirmwareMarker fm;
    size_t at = 0;
    int n = 0;
    bool match = false;
    while (!match && fwMarkerFindFrom(buf, len, at, fm)) {
        n++;
        match = fwMarkerCheck(fm) == FwMatch::Ok;
        at += 1;
    }
    if (candidates) *candidates = n;
    return match;
}

int main() {
    printf("FirmwareMarker suite:\n");

    {   // The transformed constant really is transformed: no byte of it is
        // the plain one (every plain byte differs from its XOR by 0x5A != 0).
        bool transformed = true;
        uint8_t m0[8]; putMagic(m0, 0);
        for (size_t i = 0; i < sizeof(FW_MARKER_MAGIC_X); i++)
            transformed = transformed && FW_MARKER_MAGIC_X[i] != (char)m0[i];
        CHECK(transformed, "FW_MARKER_MAGIC_X does not spell the magic");
        uint8_t m[8]; putMagic(m, 0);
        // Single-char compares: any contiguous spelling of the magic here
        // would put the very byte run this suite polices into its own binary.
        CHECK(m[0] == 'F' && m[1] == 'W' && m[2] == 'M' && m[3] == 'A' &&
              m[4] == 'R' && m[5] == 'K' && m[6] == '0' && m[7] == '1',
              "and decodes back to FWMARK-0-1");
        CHECK(fwMagicAt(m), "fwMagicAt sees decoded magic");
        m[7] ^= 1;
        CHECK(!fwMagicAt(m), "and not a near miss");
    }

    {   // g_fwMarker carries the plain magic, this build's identity and a
        // compile-time CRC that checks.
        CHECK(fwMagicAt(reinterpret_cast<const uint8_t*>(g_fwMarker.magic)),
              "g_fwMarker magic");
        CHECK(g_fwMarker.crc != 0, "g_fwMarker carries a CRC");
        CHECK(g_fwMarker.crc == fwMarkerCrc(g_fwMarker), "and it checks");
        CHECK(fwMarkerCheck(g_fwMarker) == FwMatch::Ok, "g_fwMarker matches itself");
    }

    {   // The CRC separates markers from stray bytes, and old markers from
        // corrupt ones.
        uint8_t raw[sizeof(FirmwareMarker)];
        FirmwareMarker m;

        putMarker(raw, 0, "test-project", "test-board");            // 0.18.3+
        memcpy(&m, raw, sizeof m);
        CHECK(fwMarkerCheck(m) == FwMatch::Ok, "a marker with its CRC: ok");
        raw[12] ^= 0x20;                                            // corrupt a name byte
        memcpy(&m, raw, sizeof m);
        CHECK(fwMarkerCheck(m) == FwMatch::BadChecksum, "corrupted: bad checksum");

        putMarker(raw, 0, "test-project", "test-board", false);     // pre-0.18.3
        memcpy(&m, raw, sizeof m);
        CHECK(fwMarkerCheck(m) == FwMatch::Ok, "an old marker (crc 0): still ok");
        raw[12] ^= 0x20;
        memcpy(&m, raw, sizeof m);
        CHECK(fwMarkerCheck(m) == FwMatch::ProjectMismatch,
              "an old corrupted one falls back to the field check");

        putMarker(raw, 0, "test-project", "other-board");           // foreign, CRC valid
        memcpy(&m, raw, sizeof m);
        CHECK(fwMarkerCheck(m) == FwMatch::BoardMismatch,
              "a real marker of a wrong board: board mismatch, not checksum");

        memset(raw, 0, sizeof raw);                                 // decoy: magic + junk crc
        putMagic(raw, 0);
        raw[60] = 0xAB;
        memcpy(&m, raw, sizeof m);
        CHECK(fwMarkerCheck(m) == FwMatch::BadChecksum,
              "magic followed by junk with a nonzero crc field: not a marker");
    }

    {   // The 2026-11-04 image: a decoy (magic, empty fields) BEFORE the real
        // marker. The old scan stopped at the decoy; the loop must not.
        uint8_t img[512] = {};
        putMagic(img, 32);                                   // decoy: fields all zero
        putMarker(img, 256, "test-project", "test-board");   // the real one
        int n = 0;
        CHECK(scanMatches(img, sizeof img, &n), "decoy first: the real marker is found");
        CHECK(n == 2, "both candidates were judged");
        // And the other order, which is what happened to work before.
        uint8_t img2[512] = {};
        putMarker(img2, 32, "test-project", "test-board");
        putMagic(img2, 256);
        CHECK(scanMatches(img2, sizeof img2), "real marker first: found");
    }

    {   // A genuinely foreign image stays refused, decoy or not.
        uint8_t img[512] = {};
        putMagic(img, 32);
        putMarker(img, 256, "test-project", "other-board");
        CHECK(!scanMatches(img, sizeof img), "wrong board: no match, however far it scans");
        uint8_t img2[512] = {};
        CHECK(!scanMatches(img2, sizeof img2), "no marker at all: no match");
    }

    {   // fwMarkerFindFrom walks every offset: back-to-back magics.
        uint8_t img[512] = {};
        putMagic(img, 64);
        putMagic(img, 72);                                   // inside the first's fields
        putMarker(img, 256, "test-project", "test-board");
        int n = 0;
        CHECK(scanMatches(img, sizeof img, &n), "two overlapping decoys, then the marker");
        CHECK(n == 3, "all three candidates were judged");
    }

    {   // The marker at the very end of the buffer, and one byte past it.
        uint8_t img[256] = {};
        putMarker(img, sizeof img - sizeof(FirmwareMarker), "test-project", "test-board");
        CHECK(scanMatches(img, sizeof img), "marker flush with the end: found");
        CHECK(!scanMatches(img, sizeof img - 1), "cut by one byte: not found");
    }

    printf("%d checks, %d failed.\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}
