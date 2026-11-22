/*
 * Non-volatile settings (Rev 2.28). See nvstore.h for the layout and rules.
 *
 * Record (one 256-byte flash page; unused bytes stay 0xFF):
 *   0  u32 magic  NV_MAGIC
 *   4  u32 seq    1, 2, 3, ... (newest valid record wins)
 *   8  u16 len    data bytes saved (<= NV_DATA_SIZE)
 *  10  u16 fmt    NV_FORMAT
 *  12  u8  data[NV_DATA_SIZE]
 *  76  u32 crc    CRC-32 (zlib) of bytes 0..75
 *
 * Pages are used in ring order: sector 0 pages 0..15, sector 1 pages 0..15,
 * then sector 0 again. A save goes to the first blank page after the newest
 * record (a page left half-written by a power loss is skipped). Moving onto
 * the first page of a sector that is not blank erases that sector first; it
 * only ever holds records older than the newest one.
 */
#include "nvstore.h"

#include <string.h>

#include "boot/picoboot_constants.h"
#include "hardware/sync.h"
#include "pico/bootrom.h"
#include "pico/time.h"

#include "config.h"
#include "mag.h"

#define NV_MAGIC 0xDFC5E771u /* "DFC SETTL" */
#define NV_FORMAT 1u
#define PAGE 256u
#define SECTOR 4096u
#define PAGES_PER_SECTOR (SECTOR / PAGE)
#define PAGES (NV_SECTORS * PAGES_PER_SECTOR)
#define REC_CRC_AT (12u + NV_DATA_SIZE)

_Static_assert(PAGES <= 32u, "blank map is a uint32_t");
_Static_assert(NV_FLASH_OFFSET % SECTOR == 0u, "sector aligned");
/* Stay clear of the last sector (0x1FF000): the RP2350-E10 UF2 block is
 * aimed there (CMakeLists.txt), and of both firmware slots (end 2032K). */
_Static_assert(NV_FLASH_OFFSET >= 2032u * 1024u, "inside a firmware slot");
_Static_assert(NV_FLASH_OFFSET + NV_SECTORS * SECTOR <= 0x1FF000u, "overlaps the E10 sector");

static uint8_t s_page[PAGE] __attribute__((aligned(4)));
static nv_stats_t s_stats;

/* ---- Flash access (bootrom, storage addresses; see updater.c) ------- */

/* Address guard (Rev 2.28, NV-2): erase and program only inside the two
 * settings sectors. The partition table leaves unpartitioned flash
 * writable, so the bootrom would not stop a wrong offset. */
static int flash_op(uint32_t op, uint32_t offset, uint32_t n, uint8_t *buf) {
    if (op != CFLASH_OP_VALUE_READ &&
        (n == 0u || offset < NV_FLASH_OFFSET || n > NV_SECTORS * SECTOR ||
         offset - NV_FLASH_OFFSET > NV_SECTORS * SECTOR - n)) {
        return NV_ERR_GUARD;
    }
    rom_flash_op_fn f = (rom_flash_op_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_OP);
    cflash_flags_t fl = {(op << CFLASH_OP_LSB) | (CFLASH_ASPACE_VALUE_STORAGE << CFLASH_ASPACE_LSB) |
                         (CFLASH_SECLEVEL_VALUE_SECURE << CFLASH_SECLEVEL_LSB)};
    if (!bootrom_try_acquire_lock(BOOTROM_LOCK_FLASH_OP)) {
        return BOOTROM_ERROR_LOCK_REQUIRED;
    }
    int r = f(fl, XIP_BASE + offset, n, buf);
    bootrom_release_lock(BOOTROM_LOCK_FLASH_OP);
    return r;
}

static uint32_t page_offset(uint32_t p) {
    return NV_FLASH_OFFSET + p * PAGE;
}

static uint32_t crc32(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    while (n-- != 0u) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) {
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
        }
    }
    return ~c;
}

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static bool page_blank(const uint8_t *p) {
    for (uint32_t i = 0; i < PAGE; i++) {
        if (p[i] != 0xFFu) {
            return false;
        }
    }
    return true;
}

/* True if s_page holds a valid record; *seq gets its sequence number. */
static bool page_valid(uint32_t *seq) {
    if (get32(&s_page[0]) != NV_MAGIC) {
        return false;
    }
    uint32_t len = (uint32_t)s_page[8] | ((uint32_t)s_page[9] << 8);
    if (len > NV_DATA_SIZE || get32(&s_page[REC_CRC_AT]) != crc32(s_page, REC_CRC_AT)) {
        return false;
    }
    *seq = get32(&s_page[4]);
    return *seq != 0u && *seq != 0xFFFFFFFFu;
}

/* Scan every page: the newest valid record (index, -1 if none) and a map of
 * blank pages. */
typedef struct {
    int newest;
    uint32_t seq;
    uint32_t blank;
} scan_t;

static void scan(scan_t *s) {
    s->newest = -1;
    s->seq = 0u;
    s->blank = 0u;
    for (uint32_t p = 0; p < PAGES; p++) {
        uint32_t seq;
        if (flash_op(CFLASH_OP_VALUE_READ, page_offset(p), PAGE, s_page) != 0) {
            continue; /* unreadable: neither valid nor blank */
        }
        if (page_blank(s_page)) {
            s->blank |= 1u << p;
        } else if (page_valid(&seq) && seq > s->seq) {
            s->seq = seq;
            s->newest = (int)p;
        }
    }
}

/* ---- Raw store ----------------------------------------------------- */

bool nv_load(void *buf, size_t n) {
    if (n > NV_DATA_SIZE) {
        n = NV_DATA_SIZE;
    }
    memset(buf, 0, n);
    scan_t s;
    scan(&s);
    s_stats.seq = s.seq;
    if (s.newest < 0 || flash_op(CFLASH_OP_VALUE_READ, page_offset((uint32_t)s.newest), PAGE, s_page) != 0) {
        return false;
    }
    uint32_t len = (uint32_t)s_page[8] | ((uint32_t)s_page[9] << 8);
    memcpy(buf, &s_page[12], n < len ? n : len);
    return true;
}

static int save(const void *buf, size_t n) {
    if (n > NV_DATA_SIZE) {
        return NV_ERR_SIZE;
    }
    scan_t s;
    scan(&s);

    /* Find the target page: the first blank one after the newest record. */
    uint32_t p = s.newest < 0 ? 0u : ((uint32_t)s.newest + 1u) % PAGES;
    bool found = false;
    for (uint32_t tries = 0; tries < PAGES && !found; tries++, p = (p + 1u) % PAGES) {
        if ((int)p == s.newest) {
            break; /* went all the way round */
        }
        uint32_t sector = p / PAGES_PER_SECTOR;
        uint32_t smask = ((1u << PAGES_PER_SECTOR) - 1u) << (sector * PAGES_PER_SECTOR);
        if (p % PAGES_PER_SECTOR == 0u && (s.blank & smask) != smask) {
            /* Entering a used sector: erase it (it cannot hold the newest
             * record, which is in the other sector). */
            if (s.newest >= 0 && (uint32_t)s.newest / PAGES_PER_SECTOR == sector) {
                break;
            }
            int r = flash_op(CFLASH_OP_VALUE_ERASE, NV_FLASH_OFFSET + sector * SECTOR, SECTOR, NULL);
            if (r != 0) {
                return r;
            }
            s_stats.erases++;
            s.blank |= smask;
        }
        if (s.blank & (1u << p)) {
            found = true;
            break;
        }
    }
    if (!found) {
        return NV_ERR_FULL;
    }

    memset(s_page, 0xFF, PAGE);
    put32(&s_page[0], NV_MAGIC);
    put32(&s_page[4], s.seq + 1u);
    s_page[8] = (uint8_t)n;
    s_page[9] = 0u;
    s_page[10] = (uint8_t)NV_FORMAT;
    s_page[11] = (uint8_t)(NV_FORMAT >> 8);
    memset(&s_page[12], 0, NV_DATA_SIZE);
    memcpy(&s_page[12], buf, n);
    put32(&s_page[REC_CRC_AT], crc32(s_page, REC_CRC_AT));
    int r = flash_op(CFLASH_OP_VALUE_PROGRAM, page_offset(p), PAGE, s_page);
    if (r != 0) {
        return r;
    }

    /* Read back and check. */
    uint32_t seq;
    if (flash_op(CFLASH_OP_VALUE_READ, page_offset(p), PAGE, s_page) != 0 || !page_valid(&seq) ||
        seq != s.seq + 1u || memcmp(&s_page[12], buf, n) != 0) {
        return NV_ERR_VERIFY;
    }
    s_stats.seq = seq;
    return 0;
}

int nv_save(const void *buf, size_t n) {
    uint64_t t0 = time_us_64();
    int r = save(buf, n);
    s_stats.last_us = (uint32_t)(time_us_64() - t0);
    s_stats.last_err = r;
    if (r == 0) {
        s_stats.saves++;
    }
    return r;
}

void nv_stats(nv_stats_t *s) {
    *s = s_stats;
}

/* ---- Settings ------------------------------------------------------ */

static dfc_settings_t s_settings;

bool settings_load(void) {
    return nv_load(&s_settings, sizeof s_settings);
}

const dfc_settings_t *settings_get(void) {
    return &s_settings;
}

int settings_save(const dfc_settings_t *s) {
    if (s_stats.seq != 0u && memcmp(s, &s_settings, sizeof *s) == 0) {
        return 0; /* unchanged: no flash write */
    }
    int r = nv_save(s, sizeof *s);
    if (r == 0) {
        s_settings = *s;
    }
    return r;
}

uint32_t settings_mag_rate_hz(void) {
    uint32_t hz = s_settings.mag_rate_hz;
    return mag_rate_supported(hz) ? hz : 0u;
}

bool settings_mag_offset_apply(void) {
    switch (s_settings.mag_offset) {
    case NV_OFFSET_APPLY:
        return true;
    case NV_OFFSET_RAW:
        return false;
    default:
        return MAG_OFFSET_APPLY_DEFAULT != 0;
    }
}

int settings_save_mag_offset_apply(bool apply) {
    dfc_settings_t s = s_settings;
    s.mag_offset = apply ? NV_OFFSET_APPLY : NV_OFFSET_RAW;
    return settings_save(&s);
}

bool settings_apply_startup(void) {
    bool found = settings_load();
    uint32_t hz = settings_mag_rate_hz();
    if (hz != 0u) {
        (void)mag_set_rate_hz(hz);
    }
    mag_set_offset_apply(settings_mag_offset_apply());
    return found;
}

int settings_save_mag_rate(uint32_t hz) {
    if (!mag_rate_supported(hz)) {
        return NV_ERR_VALUE;
    }
    dfc_settings_t s = s_settings;
    s.mag_rate_hz = (uint16_t)hz;
    return settings_save(&s);
}
