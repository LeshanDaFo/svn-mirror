/* comalramrom.c - Cartridge handling, COMAL RAM/ROM cart. */

#include "vice.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* VICE / C64 Cartridge includes */
#include "cartio.h"       /* io_source_t, io_source_register */
#include "c64cartmem.h"   /* ROML/ROMH callbacks used by memory system */

/* Slot-Main API (CMODE_* + cart_config_changed_slotmain) */
#define CARTRIDGE_INCLUDE_SLOTMAIN_API
#include "c64cartsystem.h"
#undef  CARTRIDGE_INCLUDE_SLOTMAIN_API
#include "c64cart.h"      /* CMODE_* constants, flags */

#include "c64mem.h"
#include "cartridge.h"
#include "log.h"
#include "resources.h"
#include "snapshot.h"
#include "types.h"

#include "comalramrom.h"
#include "lib.h"


/* -----------------------------------------------------------
    Register ($DE00)
    Bits 0–3: ROM-Bank 0..15 (Wert $00–$0F)
    Bit 5   : RAM-Selektor; $20–$3F = RAM-Bank 0..31 (Index = untere 5 Bits)
    Bit 6   : 1 = Modul AUS
----------------------------------------------------------- */
#define COMAL_BANK_MASK_NIBBLE 0x0f
#define COMAL_FLAG_DISABLE     0x40

#define COMAL_BANK_SIZE        0x4000  /* $8000-$BFFF, 16KB (ROML+ROMH je 8KB) */
#define COMAL_LOWER_OFF        0x0000  /* ROML (8000-9FFF) */
#define COMAL_UPPER_OFF        0x2000  /* ROMH (A000-BFFF) */

static uint8_t *comal_rom = NULL;      /* comal_rom_banks * 16KB (ro) */
static uint8_t *comal_ram = NULL;      /* comal_ram_banks * 16KB (rw) */
static unsigned comal_rom_banks = 0;   /* vom CRT-Parser ermittelt (typ. 4/6/8) */
static unsigned comal_ram_banks = 32;   /* Standard: 32 RAM-Bänke verfügbar */
static uint8_t  comal_bank  = 0;       /* roher Bankwert 0..15 aus $DE00 */
static uint8_t  comal_flags = 0x00;    /* nur Disable-Bit relevant */
static int      comal_enabled = 1;

/* Kompatibilität: Bit3 RAM-Selektor (alter Stil) */
static uint8_t  comal_ram_sel_compat = 0;

/* Vorwärtsdeklarationen IO1-Handler */
static void    comalramrom_io1_store(uint16_t addr, uint8_t value);
static uint8_t comalramrom_io1_peek (uint16_t addr);
static int     comalramrom_dump     (void);

/* IO-Source-Handle */
static io_source_list_t *comal_list_item = NULL;

/* ---------------------- CRT helpers (Big Endian) ---------------------- */
static uint16_t be16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}
static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* --------- aktuelle Sicht (ROM/RAM) & Index aus comal_bank ableiten -------- */
static inline int comal_is_ram_window(void)
{
    /* RAM, wenn Bank 8..15 ODER Bit3 gesetzt (alter Stil) */
    return (comal_bank & 0x20) != 0;
}
/* NEU: ROM = 0..15 (4 Bits), RAM = 0..31 (5 Bits) */
static inline unsigned comal_bank_index(void)
{
    if (comal_is_ram_window()) {
        return (unsigned)(comal_bank & 0x1f);  /* $20..$3F → RAM-Bank 0..31 */
    } else {
        return (unsigned)(comal_bank & 0x0f);  /* $00..$0F → ROM-Bank 0..15 */
    }
}

/* -----------------------------------------------------------
   internes Mapping / Config-Schaltung
   ----------------------------------------------------------- */
static void comalramrom_apply_config(void)
{
    /* 16K GAME-Konfiguration (wie Pagefox): beide Sichten = 16KGAME */
    uint8_t mode_a = CMODE_16KGAME;
    uint8_t mode_b = CMODE_16KGAME;

    /* Flags abhängig von ROM/RAM-Fenster: RAM -> Export-RAM aktivieren */
    uint8_t flags  = CMODE_READ;
    if (comal_is_ram_window()) {
        flags |= CMODE_EXPORT_RAM;
    }

    cart_config_changed_slotmain(mode_a, mode_b, flags);
}

static void comalramrom_remap_window(void)
{
    /* Enabled-Status aus Bit 6 */
    comal_enabled = (comal_flags & COMAL_FLAG_DISABLE) ? 0 : 1;

    if (comal_enabled) {
        comalramrom_apply_config();
    } else {
        cart_config_changed_slotmain(CMODE_RAM, CMODE_RAM, 0);
    }
}

/* -----------------------------------------------------------
   IO1 ($DE00-$DEFF)
   ----------------------------------------------------------- */
static void comalramrom_io1_store(uint16_t addr, uint8_t value)
{
    (void)addr;

    /* Rohwert behalten (wir erlauben $00..$0F ROM, $20..$3F RAM) */
    comal_bank = value & 0x3f;  /* defensiv auf 0..63 masken */

    /* Kompatibilität: Bit3 = RAM-Fenster (alter Stil) merken */
    comal_ram_sel_compat = 0;

    /* Disable-Bit (Bit6) übernehmen/aktualisieren */
    if (value & COMAL_FLAG_DISABLE) {
        comal_flags |= COMAL_FLAG_DISABLE;
    } else {
        comal_flags &= (uint8_t)~COMAL_FLAG_DISABLE;
    }

    comalramrom_remap_window();
}

static uint8_t comalramrom_io1_peek(uint16_t addr)
{
    (void)addr;
    /* Readback: Bankbits und Disable-Bit berichten (Kompat-Status nicht zwingend nötig) */
    return (uint8_t)((comal_bank & COMAL_BANK_MASK_NIBBLE) | (comal_flags & COMAL_FLAG_DISABLE));
}

static int comalramrom_dump(void)
{
    log_message(LOG_DEFAULT,
        "COMALRAMROM: enabled=%s bankraw=$%02x (idx=%u, %s) flags=$%02x  ROMbanks=%u RAMbanks=%u compatRAM=%u\n",
        comal_enabled ? "yes" : "no",
        (unsigned)comal_bank,
        comal_bank_index(),
        comal_is_ram_window() ? "RAM" : "ROM",
        (unsigned)comal_flags,
        (unsigned)comal_rom_banks,
        (unsigned)comal_ram_banks,
        (unsigned)comal_ram_sel_compat);
    return 0;
}

/* -----------------------------------------------------------
   ROML/ROMH – Read/Write (werden aus c64cartmem.c gerufen)
   ----------------------------------------------------------- */
static inline uint8_t *current_bank_base_for_read(void)
{
    const unsigned idx = comal_bank_index();
    if (comal_is_ram_window()) {
        if (!comal_ram || idx >= comal_ram_banks) return NULL;
        return comal_ram + (size_t)idx * COMAL_BANK_SIZE;
    } else {
        if (!comal_rom || idx >= comal_rom_banks) return NULL;
        return comal_rom + (size_t)idx * COMAL_BANK_SIZE;
    }
}

static inline uint8_t *current_bank_base_for_write(void)
{
    /* Nur RAM beschreibbar */
    const unsigned idx = comal_bank_index();
    if (!comal_is_ram_window()) return NULL;
    if (!comal_ram || idx >= comal_ram_banks) return NULL;
    return comal_ram + (size_t)idx * COMAL_BANK_SIZE;
}

uint8_t comalramrom_roml_read(uint16_t addr)
{
    if (!comal_enabled) return 0xFF;
    uint8_t *bank = current_bank_base_for_read();
    if (!bank) return 0xFF;
    return bank[COMAL_LOWER_OFF + (addr & 0x1FFF)];
}

void comalramrom_roml_store(uint16_t addr, uint8_t value)
{
    if (!comal_enabled) return;
    uint8_t *bank = current_bank_base_for_write();
    if (!bank) return;
    bank[COMAL_LOWER_OFF + (addr & 0x1FFF)] = value;
}

uint8_t comalramrom_romh_read(uint16_t addr)
{
    if (!comal_enabled) return 0xFF;
    uint8_t *bank = current_bank_base_for_read();
    if (!bank) return 0xFF;
    return bank[COMAL_UPPER_OFF + (addr & 0x1FFF)];
}

void comalramrom_romh_store(uint16_t addr, uint8_t value)
{
    if (!comal_enabled) return;
    uint8_t *bank = current_bank_base_for_write();
    if (!bank) return;
    bank[COMAL_UPPER_OFF + (addr & 0x1FFF)] = value;
}

/* -----------------------------------------------------------
   IO-Device-Beschreibung (für IO1 @ $DE00-$DEFF)
   ----------------------------------------------------------- */
static io_source_t comalramrom_device = {
    CARTRIDGE_NAME_COMALRAMROM, /* name of the device */
    IO_DETACH_CART,
    IO_DETACH_NO_RESOURCE,
    0xde00, 0xdeff, 0xff,       /* IO1 range: $de00..$deff */
    0,                          /* read is never valid, write-only reg */
    comalramrom_io1_store,
    NULL,                       /* poke */
    NULL,                       /* read */
    comalramrom_io1_peek,       /* peek */
    comalramrom_dump,           /* dump */
    CARTRIDGE_COMALRAMROM,      /* cartridge ID */
    IO_PRIO_NORMAL,
    0,                          /* insertion order (filled by register) */
    IO_MIRROR_NONE
};

/* -----------------------------------------------------------
   Common attach
   ----------------------------------------------------------- */
static int comalramrom_common_attach(void)
{
    if (!comal_list_item) {
        comal_list_item = io_source_register(&comalramrom_device);
    }

    /* Defaults */
    comal_bank          = 0x00; /* ROM Bank 0 */
    comal_flags         = 0x00; /* enabled */
    comal_enabled       = 1;
    comal_ram_sel_compat = 0;

    /* 16K GAME sichtbar machen (Flag je nach RAM/ROM kommt aus apply_config) */
    comalramrom_apply_config();
    return 0;
}

/* -----------------------------------------------------------
   CRT-Attach (CHIP-Parser, nur ROM füllen)
   ----------------------------------------------------------- */
int comalramrom_crt_attach(FILE *fd, uint8_t *rawcart)
{
    (void)rawcart;

    if (!fd) {
        return comalramrom_common_attach();
    }

    /* VICE liefert fd oft direkt vor dem ersten CHIP. Header optional. */
    long pos0 = ftell(fd);
    if (pos0 < 0) pos0 = 0;

    /* Wenn am Dateianfang: optional Magic prüfen und ggf. zum CHIP-Start vorspulen */
    if (pos0 == 0) {
        uint8_t hdr[0x40];
        if (fread(hdr, 1, sizeof(hdr), fd) == sizeof(hdr)) {
            static const uint8_t magic[16] = {
                0x43,0x36,0x34,0x20,0x43,0x41,0x52,0x54,0x52,0x49,0x44,0x47,0x45,0x20,0x20,0x20
            };
            if (memcmp(hdr, magic, 16) != 0) {
                /* kein klassischer CRT-Header – einfach ab pos0 als CHIP-Stream lesen */
                fseek(fd, pos0, SEEK_SET);
            }
        } else {
            fseek(fd, pos0, SEEK_SET);
        }
    } else {
        fseek(fd, pos0, SEEK_SET);
    }

    /* 1) CHIP-Header scannen -> höchste ROM-Bank ermitteln */
    size_t max_rom_bankp1 = 0;
    unsigned chips_count = 0;
    long chips_start = ftell(fd);

    for (;;) {
        uint8_t ch[16];
        if (fread(ch, 1, sizeof(ch), fd) != sizeof(ch)) {
            break;
        }
        if (memcmp(ch, "CHIP", 4) != 0) {
            break;
        }
        uint32_t packet_len = be32(ch + 4);
        uint16_t bank       = be16(ch + 10);
        uint16_t img_size   = be16(ch + 14);

        if (bank + 1 > max_rom_bankp1) max_rom_bankp1 = (size_t)bank + 1;
        ++chips_count;

        /* skip payload + evtl. Padding */
        if (fseek(fd, img_size, SEEK_CUR) != 0) break;
        uint32_t consumed = 16u + (uint32_t)img_size;
        if (packet_len > consumed) {
            if (fseek(fd, (long)(packet_len - consumed), SEEK_CUR) != 0) break;
        }
    }

    if (chips_count == 0 || max_rom_bankp1 == 0) {
        log_message(LOG_DEFAULT, "COMALRAMROM: no CHIP packets");
        fseek(fd, chips_start, SEEK_SET);
        return comalramrom_common_attach();
    }

    comal_rom_banks = (unsigned)max_rom_bankp1; /* 1..8 typischerweise */

    /* 2) ROM-Puffer allozieren/initialisieren (0xFF = leer) */
    size_t rom_total = (size_t)comal_rom_banks * COMAL_BANK_SIZE;
    uint8_t *new_rom = (uint8_t*)lib_malloc(rom_total);
    memset(new_rom, 0xFF, rom_total);
    if (comal_rom) lib_free(comal_rom);
    comal_rom = new_rom;

    /* 3) RAM-Puffer bereitstellen (immer 8 Bänke, 0x00 init) */
    size_t ram_total = (size_t)comal_ram_banks * COMAL_BANK_SIZE;
    uint8_t *new_ram = (uint8_t*)lib_malloc(ram_total);
    memset(new_ram, 0x00, ram_total);
    if (comal_ram) lib_free(comal_ram);
    comal_ram = new_ram;

    /* 4) CHIP-Pakete lesen & in ROM ablegen */
    fseek(fd, chips_start, SEEK_SET);
    unsigned loaded = 0;

    for (;;) {
        uint8_t ch[16];
        if (fread(ch, 1, sizeof(ch), fd) != sizeof(ch)) break;
        if (memcmp(ch, "CHIP", 4) != 0) break;

        uint32_t packet_len = be32(ch + 4);
        uint16_t chip_type  = be16(ch + 8);   /* 0=ROML, 1=ROMH */
        uint16_t bank       = be16(ch + 10);
        uint16_t addr       = be16(ch + 12);  /* 0x8000/0xA000 */
        uint16_t img_size   = be16(ch + 14);

        if (img_size > COMAL_BANK_SIZE) {
            /* Unsinnig -> Daten überspringen */
            if (fseek(fd, img_size, SEEK_CUR) != 0) break;
            uint32_t consumed = 16u + (uint32_t)img_size;
            if (packet_len > consumed) {
                if (fseek(fd, (long)(packet_len - consumed), SEEK_CUR) != 0) break;
            }
            continue;
        }

        if ((size_t)bank < max_rom_bankp1) {
            size_t base   = (size_t)bank * COMAL_BANK_SIZE;
            size_t dst_off = (chip_type == 0 || addr == 0x8000)
                           ? (base + COMAL_LOWER_OFF)
                           : (base + COMAL_UPPER_OFF);
            if (fread(comal_rom + dst_off, 1, img_size, fd) != img_size) break;
        } else {
            /* Bank außerhalb – überspringen */
            if (fseek(fd, img_size, SEEK_CUR) != 0) break;
        }

        /* Rest (Padding) skippen */
        uint32_t consumed = 16u + (uint32_t)img_size;
        if (packet_len > consumed) {
            if (fseek(fd, (long)(packet_len - consumed), SEEK_CUR) != 0) break;
        }
        ++loaded;
    }

    log_message(LOG_DEFAULT,
        "COMALRAMROM: CRT loaded: chips=%u, ROMbanks=%u, RAMbanks=%u",
        loaded, (unsigned)comal_rom_banks, (unsigned)comal_ram_banks);

    return comalramrom_common_attach();
}

/* -----------------------------------------------------------
   BIN-Attach (derzeit nur Defaults; echte BIN-Lader später)
   ----------------------------------------------------------- */
int comalramrom_bin_attach(const char *filename, uint8_t *rawcart)
{
    (void)filename; (void)rawcart;
    if (comal_rom_banks == 0) comal_rom_banks = 4;  /* Default */
    size_t rom_total = (size_t)comal_rom_banks * COMAL_BANK_SIZE;
    if (!comal_rom) comal_rom = (uint8_t*)lib_malloc(rom_total);
    memset(comal_rom, 0xFF, rom_total);

    size_t ram_total = (size_t)comal_ram_banks * COMAL_BANK_SIZE;
    if (!comal_ram) comal_ram = (uint8_t*)lib_malloc(ram_total);
    memset(comal_ram, 0x00, ram_total);

    return comalramrom_common_attach();
}

void comalramrom_detach(void)
{
    if (comal_list_item) {
        io_source_unregister(comal_list_item);
        comal_list_item = NULL;
    }
    if (comal_rom) { lib_free(comal_rom); comal_rom = NULL; }
    if (comal_ram) { lib_free(comal_ram); comal_ram = NULL; }

    /* Fenster ausblenden */
    cart_config_changed_slotmain(CMODE_RAM, CMODE_RAM, 0);
}

void comalramrom_powerup(void)
{
    /* Optional: RAM-Init wie pagefox_powerup() */
}

void comalramrom_config_init(void)
{
    /* Kaltstart-Defaults */
    comal_bank           = 0x00;  /* ROM Bank 0 */
    comal_flags          = 0x00;  /* enabled */
    comal_enabled        = 1;
    comal_ram_sel_compat = 0;
    comalramrom_apply_config();
}

void comalramrom_config_setup(uint8_t *rawcart)
{
    (void)rawcart;
    comalramrom_remap_window();
}

/* -----------------------------------------------------------
   Snapshot-Stubs (später füllen)
   ----------------------------------------------------------- */
int comalramrom_snapshot_write_module(struct snapshot_s *s) { (void)s; return 0; }
int comalramrom_snapshot_read_module (struct snapshot_s *s) { (void)s; return comalramrom_common_attach(); }