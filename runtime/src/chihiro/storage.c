/*
 * Chihiro baseboard storage and clock: backup RAM, EEPROM, RTC.
 *
 * Host replacements for the SEGA XPP library's JvsBACKUP_Read/Write,
 * JvsEEPROM_Read/Write and JvsRTC_Read (named as sub_XXXXXXXX in
 * src/recomp_manual.c). On hardware each one is a transfer to the Sega
 * baseboard through 0x13CB10 that the library polls until done; here there is
 * no board, so they complete at once against files in the save directory.
 *
 * All five are stdcall with four arguments,
 *     status = f(uint32_t dev_offset, uint32_t size, void *buf, uint32_t flags)
 * so on entry [esp] is the return address, the arguments follow, and the
 * callee pops 4 + 16 bytes. Status >= 0 is success.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "recomp_types.h"
#include "chihiro/chihiro_profile.h"

extern const char *chihiro_save_dir(void);

#define STDCALL_ARG(n)  MEM32(g_esp + 4 + 4 * (n))
#define STDCALL_RET(v)  do { g_eax = (uint32_t)(v); g_esp += 20; } while (0)

/* Raw device offsets: backup RAM is addressed as a 32 KB window at 0x8000 and
 * reads that cross it split, so index the file by the raw offset and let a
 * 64 KB image cover both halves. */
#define BACKUP_SIZE  0x10000u
#define EEPROM_SIZE  0x100u
#define LOG_LIMIT    16

typedef struct {
    const char *name;
    uint32_t size;
    uint8_t *data;
    int loaded;
    int logged;
} store_t;

static uint8_t s_backup_data[BACKUP_SIZE];
static uint8_t s_eeprom_data[EEPROM_SIZE];
static store_t s_backup = { "backup.bin", BACKUP_SIZE, s_backup_data, 0, 0 };
static store_t s_eeprom = { "eeprom.bin", EEPROM_SIZE, s_eeprom_data, 0, 0 };

static void store_path(const store_t *s, char *out, size_t len)
{
    snprintf(out, len, "%s/%s", chihiro_save_dir(), s->name);
}

/* First touch: load the image, or start blank. A blank store makes the game
 * take its own "no valid backup data" path and initialise it, as a new
 * cabinet does. */
static void store_load(store_t *s)
{
    char path[1100];
    FILE *f;

    if (s->loaded)
        return;
    s->loaded = 1;
    memset(s->data, 0, s->size);
    store_path(s, path, sizeof(path));
    f = fopen(path, "rb");
    if (f) {
        size_t n = fread(s->data, 1, s->size, f);
        fclose(f);
        fprintf(stderr, "[CHIHIRO] %s: loaded %zu bytes\n", s->name, n);
    } else {
        fprintf(stderr, "[CHIHIRO] %s: none yet, starting blank\n", s->name);
    }
}

/* Write-through: the images are small and a crash mid-bring-up must not
 * lose the settings the game just saved. */
static void store_flush(const store_t *s)
{
    char path[1100];
    FILE *f;

    store_path(s, path, sizeof(path));
    f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "[CHIHIRO] %s: cannot write %s\n", s->name, path);
        return;
    }
    fwrite(s->data, 1, s->size, f);
    fclose(f);
}

/* Copy between guest memory and the store, clamped to the store. Returns the
 * number of bytes moved. */
static uint32_t store_xfer(store_t *s, int write, const char *op)
{
    uint32_t off = STDCALL_ARG(0);
    uint32_t size = STDCALL_ARG(1);
    uint32_t buf = STDCALL_ARG(2);
    uint32_t flags = STDCALL_ARG(3);
    uint32_t n = 0;

    store_load(s);
    if (off < s->size && buf) {
        n = size < s->size - off ? size : s->size - off;
        if (write)
            memcpy(s->data + off, (const void *)XBOX_PTR(buf), n);
        else
            memcpy((void *)XBOX_PTR(buf), s->data + off, n);
    }
    if (s->logged < LOG_LIMIT || n != size) {
        s->logged++;
        fprintf(stderr, "[CHIHIRO] %s off=0x%04X size=0x%X buf=0x%08X flags=%u%s\n",
                op, off, size, buf, flags, n != size ? " (clamped)" : "");
    }
    if (write && n)
        store_flush(s);
    return n;
}

/* Player 1's gun calibration as GUN ADJUSTMENT stores it: at 0x476 a 0x0010
 * tag, then TOP, LEFT, CENTER, RIGHT, BOTTOM as signed (x, y) pairs in the
 * game's gun coordinates. Returns 1 and fills cal[10] when the block is there
 * and ordered as a calibration must be. */
int chihiro_gun_calibration(int16_t cal[10])
{
    uint32_t at = chihiro_profile()->aim.backup_offset;   /* VC3: 0x476 */
    const uint8_t *p = s_backup_data + at;
    int i;

    if (!at || at + 22 > BACKUP_SIZE)
        return 0;
    store_load(&s_backup);
    if ((p[0] | p[1] << 8) != 0x0010)
        return 0;
    for (i = 0; i < 10; i++)
        cal[i] = (int16_t)(p[2 + i * 2] | p[3 + i * 2] << 8);
    /* left < centre < right across, bottom < centre < top down */
    return cal[2] < cal[4] && cal[4] < cal[6] && cal[9] < cal[5] && cal[5] < cal[1];
}

void chihiro_backup_read(void)
{
    store_xfer(&s_backup, 0, "BACKUP_Read");
    STDCALL_RET(0);
}

void chihiro_backup_write(void)
{
    store_xfer(&s_backup, 1, "BACKUP_Write");
    STDCALL_RET(0);
}

void chihiro_eeprom_read(void)
{
    store_xfer(&s_eeprom, 0, "EEPROM_Read");
    STDCALL_RET(0);
}

void chihiro_eeprom_write(void)
{
    store_xfer(&s_eeprom, 1, "EEPROM_Write");
    STDCALL_RET(0);
}

static uint8_t bcd(int v)
{
    return (uint8_t)(((v / 10) % 10) << 4 | (v % 10));
}

/* The caller (0x105020) reads 16 bytes and unpacks the first eight as BCD.
 * Field order {yy, mm, dd, wday, hh, mi, ss, 0} is inferred from where it
 * stores each byte; the test menu's clock is the check. */
void chihiro_rtc_read(void)
{
    static int logged;
    uint32_t size = STDCALL_ARG(1);
    uint32_t buf = STDCALL_ARG(2);
    uint8_t rtc[16] = { 0 };
    time_t now = time(NULL);
    struct tm tm;

    localtime_r(&now, &tm);
    rtc[0] = bcd(tm.tm_year % 100);
    rtc[1] = bcd(tm.tm_mon + 1);
    rtc[2] = bcd(tm.tm_mday);
    rtc[3] = bcd(tm.tm_wday);
    rtc[4] = bcd(tm.tm_hour);
    rtc[5] = bcd(tm.tm_min);
    rtc[6] = bcd(tm.tm_sec);
    if (buf)
        memcpy((void *)XBOX_PTR(buf), rtc, size < sizeof(rtc) ? size : sizeof(rtc));
    if (logged++ < LOG_LIMIT)
        fprintf(stderr, "[CHIHIRO] RTC_Read size=0x%X -> %02X-%02X-%02X %02X:%02X:%02X\n",
                size, rtc[0], rtc[1], rtc[2], rtc[4], rtc[5], rtc[6]);
    STDCALL_RET(0);
}
