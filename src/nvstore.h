/*
 * Non-volatile settings (Rev 2.28, NV-1..NV-13; offset choice M-4c).
 *
 * A small block of settings (NV_DATA_SIZE = 64 bytes) kept in flash, outside
 * both firmware slots, so it survives power cycles and firmware updates.
 *
 * Layout: two 4 KB sectors of the unpartitioned flash at the end of the chip
 * (NV_FLASH_OFFSET, 0x1FC000-0x1FDFFF). Each save appends one 256-byte
 * record (magic, sequence number, length, data, CRC-32) to the next blank
 * page; the newest record with a good CRC is the current one. When a sector
 * fills up, the other one is erased and used next, so the newest record is
 * never erased. A save cut short by a power loss or reset leaves a record
 * with a bad CRC, which is ignored: the previous settings stay in force.
 *
 * Wear: one erase per 16 saves per sector; ~100k erase cycles gives well
 * over a million saves.
 *
 * Timing: nv_save() blocks: ~1 ms for the page program, plus a sector erase
 * (typically 45 ms, up to 400 ms) every 16th save. Interrupts and DMA keep
 * running (the firmware runs from RAM), but the main loop stops, so 1 ms
 * epochs are missed while it runs. Call it from the main loop only, and only
 * when a setting really changes (settings_save() skips unchanged data).
 *
 * Growing the settings: add fields to the END of dfc_settings_t only, use
 * part of `reserved`, and keep the struct exactly NV_DATA_SIZE bytes. A
 * record saved by older firmware is shorter: the missing bytes read as 0,
 * so give every field the meaning "0 = use the default".
 */
#ifndef NVSTORE_H
#define NVSTORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NV_DATA_SIZE 64u
#define NV_FLASH_OFFSET 0x1FC000u /* physical flash offset of the first sector */
#define NV_SECTORS 2u

/* ---- Raw store ----------------------------------------------------- */

/* Read the current settings bytes into buf (n <= NV_DATA_SIZE). Bytes the
 * stored record does not have are set to 0. Returns false, with buf all 0,
 * if nothing valid is stored (new board, or both sectors erased). */
bool nv_load(void *buf, size_t n);

/* Save n bytes (n <= NV_DATA_SIZE) as the new current settings, and read
 * them back to check. Returns 0 on success, else a negative error
 * (NV_ERR_* or a bootrom error code). Blocks; see above. */
int nv_save(const void *buf, size_t n);

#define NV_ERR_SIZE (-100)   /* n > NV_DATA_SIZE */
#define NV_ERR_FULL (-101)   /* no blank page could be made (should not happen) */
#define NV_ERR_VERIFY (-102) /* read-back did not match */
#define NV_ERR_VALUE (-103)  /* settings_save_mag_rate(): not a supported rate */
#define NV_ERR_GUARD (-104)  /* write outside the settings sectors refused (bug) */

typedef struct {
    uint32_t seq;     /* sequence number of the current record; 0 = none */
    uint32_t saves;   /* nv_save() calls that succeeded since boot */
    uint32_t erases;  /* sector erases since boot */
    int32_t last_err; /* result of the last nv_save() (0 = ok, or none yet) */
    uint32_t last_us; /* how long the last nv_save() took (us) */
} nv_stats_t;

void nv_stats(nv_stats_t *s);

/* ---- Settings ------------------------------------------------------ */

typedef struct {
    uint16_t mag_rate_hz; /* start-up magnetometer rate (M-3b); 0 = default (MAG_RATE_DEFAULT_HZ) */
    uint8_t mag_offset;   /* M-4c: remove the offset in mag_field()? NV_OFFSET_*; 0 = default */
    uint8_t reserved[61]; /* future settings: 0 = default */
} dfc_settings_t;

#define NV_OFFSET_DEFAULT 0u /* MAG_OFFSET_APPLY_DEFAULT (config.h) */
#define NV_OFFSET_APPLY 1u   /* offset removed */
#define NV_OFFSET_RAW 2u     /* offset left in (field as measured) */

_Static_assert(sizeof(dfc_settings_t) == NV_DATA_SIZE, "settings must be NV_DATA_SIZE bytes");

/* Load the stored settings into RAM (call once, from app_init()). Returns
 * false if none are stored: all fields are then 0 (= defaults). */
bool settings_load(void);

/* The settings as loaded or last saved (RAM copy). */
const dfc_settings_t *settings_get(void);

/* Save s (and make it the RAM copy). Skips the flash write, returning 0,
 * if s equals what is already stored. Otherwise as nv_save(). */
int settings_save(const dfc_settings_t *s);

/* Helpers for the start-up magnetometer rate. */
uint32_t settings_mag_rate_hz(void); /* stored rate, or 0 if none / not supported */
int settings_save_mag_rate(uint32_t hz); /* NV_ERR_VALUE if hz is not a supported rate */

/* Helpers for the offset setting (M-4c). settings_mag_offset_apply() gives
 * the stored choice, or MAG_OFFSET_APPLY_DEFAULT if none (or an unknown
 * value) is stored. */
bool settings_mag_offset_apply(void);
int settings_save_mag_offset_apply(bool apply);

/* Load the settings and apply them (rate, offset) to the magnetometer: the
 * whole start-up step, for app_init(). Returns settings_load()'s result. */
bool settings_apply_startup(void);

#endif /* NVSTORE_H */
