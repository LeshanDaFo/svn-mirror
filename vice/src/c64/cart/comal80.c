/*
 * comal80.c - Cartridge handling, Comal80 cart.
 *
 * Written by
 *  Marco van den Heuvel <blackystardust68@yahoo.com>
 *
 * This file is part of VICE, the Versatile Commodore Emulator.
 * See README for copyright notice.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 *  02111-1307  USA.
 *
 */

/* #define DEBUGCART */

#include "vice.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CARTRIDGE_INCLUDE_SLOTMAIN_API
#include "c64cartsystem.h"
#undef CARTRIDGE_INCLUDE_SLOTMAIN_API
#include "c64mem.h"
#include "cartio.h"
#include "cartridge.h"
#include "cmdline.h"
#include "comal80.h"
#include "export.h"
#include "log.h"
#include "monitor.h"
#include "resources.h"
#include "snapshot.h"
#include "types.h"
#include "util.h"
#include "crt.h"

#ifdef DEBUGCART
#define DBG(x) log_printf x
#else
#define DBG(x)
#endif

/*
    Comal80 Cartridge

    - 64K ROM (32K mapped to $8000 and 32K mapped to $A000)
    - free socket for another 64K eprom

    The cart has 1 (write-only) bank control register which is located at $DE00
    and mirrored throughout the $DE00-$DEFF range.

    Two variants of this Cartridge existed:

    1. Grey Cartridge (Comal Users Group, USA)
       Produced around 1984-1985, likely with UniComal Denmark’s approval.
       Approx. 2,000 units distributed (according to COMAL Today).

    - 4 sockets populated with 16 KB EPROMs (27128)
    - NO additional socket, but it was possible to piggyback a fifth EPROM via
      minor hardware mod

    bit 7   : -
    bit 6   : GAME
    bit 5   : EXROM
    bit 4   : -
    bit 3   : -
    bit 2   : -
    bit 0-1 : selects EPROM (16k bank)

    2. Black Cartridge (Official Commodore Release)
       Used internationally and with a different hardware configuration.

    - Two fixed 32 KB ROMs (Commodore) and one socket for a 32 KB (or with
      hardware modification 64 KB ) EPROM

    bit 7   : -
    bit 6   : GAME + EXROM
    bit 5   : -
    bit 4   : -
    bit 3   : RAM/ROM select (0 = ROM, 1 = RAM)
    bit 2   : switches to the optional third socket (additional 2x16k)
    bit 0-1 : selects EPROM+bank (4x16k bank)

*/

static int currregval = 0;
static int extrarom = 0;

#define VARIANT_GREY    0
#define VARIANT_BLACK   1
static int comal80_variant = VARIANT_BLACK;

// RAM bank select flag and current bank variable
static int currbank_is_ram = 0;
static int currbank = 0;

// RAM banks storage: 8 RAM banks, each bank has LO and HI 16KB blocks
static uint8_t comal80_ram_lo[8][0x2000];  // $8000-$9FFF
static uint8_t comal80_ram_hi[8][0x2000];  // $A000-$BFFF

// The existing ROM banks are assumed to be in roml_banks and romh_banks arrays (already declared elsewhere)

static void comal80_io1_store(uint16_t addr, uint8_t value)
{
    int cmode;

    if (comal80_variant == VARIANT_GREY) {
        static int modes[4] = {
            CMODE_16KGAME, CMODE_ULTIMAX, CMODE_8KGAME, CMODE_RAM
        };
        currregval = value & 0x63;
        cmode = modes[(value & 0x60) >> 5];
        currbank = value & 3;
        currbank_is_ram = 0; // Grey variant does not support RAM bank switching via bit3
    } else {
        // Black variant:
        currregval = value & 0x4F; // mask bits: bit7=0, bit6, bit3, bit2, bits0-1-2-3 plus bit6 retained
        currbank = value & 7;      // bits 0-2 select bank (0-7)
        currbank_is_ram = (value & 8) ? 1 : 0; // bit 3 for RAM/ROM select
        cmode = (value & 0x40) ? CMODE_RAM : CMODE_16KGAME;
    }

#ifdef DEBUGCART
    if (currregval != value) {
        static unsigned int last = 0;
        unsigned int now = value ^ currregval;
        if (last != now) {
            DBG(("using unconnected bits: 0x%02x", now));
            last = now;
        }
    }
#endif

    // Update cartridge memory config according to mode and bank
    if (currbank_is_ram) {
        // RAM mode, access memory mapped to RAM banks
        // Using CMODE_RAM mode (custom mode signifying RAM enabled) plus bank shift
        cart_config_changed_slotmain(0, (CMODE_RAM | (currbank << CMODE_BANK_SHIFT)), CMODE_READ);
    } else {
        // ROM mode, normal ROM banking
        cart_config_changed_slotmain(0, (CMODE_16KGAME | (currbank << CMODE_BANK_SHIFT)), CMODE_READ);
    }
}

static uint8_t comal80_io1_peek(uint16_t addr)
{
    return currregval;
}

// Read function to return data from current bank mapped either to ROM or RAM
static uint8_t comal80_read(uint16_t addr)
{
    uint16_t offset = addr & 0x1FFF; // 8KB offset inside bank
    if (currbank_is_ram) {
        // RAM mode
        if (addr >= 0x8000 && addr < 0xA000) {
            return comal80_ram_lo[currbank][offset];
        } else if (addr >= 0xA000 && addr < 0xC000) {
            return comal80_ram_hi[currbank][offset];
        }
    } else {
        // ROM mode
        if (addr >= 0x8000 && addr < 0xA000) {
            return roml_banks[currbank * 0x2000 + offset];
        } else if (addr >= 0xA000 && addr < 0xC000) {
            return romh_banks[currbank * 0x2000 + offset];
        }
    }
    return 0xFF; // default (open bus)
}

// Write function to write data to the RAM bank (no operation on ROM)
static void comal80_write(uint16_t addr, uint8_t val)
{
    uint16_t offset = addr & 0x1FFF;
    if (currbank_is_ram) {
        if (addr >= 0x8000 && addr < 0xA000) {
            comal80_ram_lo[currbank][offset] = val;
        } else if (addr >= 0xA000 && addr < 0xC000) {
            comal80_ram_hi[currbank][offset] = val;
        }
    }
    // no writes to ROM area
}

static int comal80_dump(void)
{
    mon_out("Cartridge variant: %s", (comal80_variant == VARIANT_GREY) ? "grey" : "black");
    mon_out("Extra eprom is installed: %s\n", extrarom ? "yes" : "no");
    mon_out("Register value: $%02x\n", (unsigned int)currregval);
    mon_out(" bank: %d/%d\n", currbank, extrarom ? 8 : 4);
    mon_out(" mode: %s\n", currbank_is_ram ? "RAM" : "ROM");
    return 0;
}

/* ---------------------------------------------------------------------*/

static io_source_t comal80_device = {
    CARTRIDGE_NAME_COMAL80, /* name of the device */
    IO_DETACH_CART,         /* use cartridge ID to detach the device when involved in a read-collision */
    IO_DETACH_NO_RESOURCE,  /* does not use a resource for detach */
    0xde00, 0xdeff, 0xff,   /* range for the device, address is ignored, reg:$de00, mirrors:$de01-$deff */
    0,                      /* read never valid, device is write only */
    comal80_io1_store,      /* store function */
    comal80_write,          /* poke function for writes to RAM area */
    comal80_read,           /* read function to read from ROM or RAM banks */
    comal80_io1_peek,       /* peek function */
    comal80_dump,           /* device state information dump function */
    CARTRIDGE_COMAL80,      /* cartridge ID */
    IO_PRIO_NORMAL,         /* normal priority, device read needs to be checked for collisions */
    0,                      /* insertion order, gets filled in by the registration function */
    IO_MIRROR_NONE          /* NO mirroring */
};

static io_source_list_t *comal80_list_item = NULL;

static const export_resource_t export_res = {
    CARTRIDGE_NAME_COMAL80, 1, 1, &comal80_device, NULL, CARTRIDGE_COMAL80
};

/* ---------------------------------------------------------------------*/

void comal80_config_init(void)
{
    cart_config_changed_slotmain(CMODE_16KGAME, CMODE_16KGAME, CMODE_READ);
    currregval = 0;
    currbank = 0;
    currbank_is_ram = 0;
}

void comal80_config_setup(uint8_t *rawcart)
{
    memcpy(&roml_banks[0x0000], &rawcart[0x0000], 0x2000);
    memcpy(&romh_banks[0x0000], &rawcart[0x2000], 0x2000);
    memcpy(&roml_banks[0x2000], &rawcart[0x4000], 0x2000);
    memcpy(&romh_banks[0x2000], &rawcart[0x6000], 0x2000);
    memcpy(&roml_banks[0x4000], &rawcart[0x8000], 0x2000);
    memcpy(&romh_banks[0x4000], &rawcart[0xa000], 0x2000);
    memcpy(&roml_banks[0x6000], &rawcart[0xc000], 0x2000);
    memcpy(&romh_banks[0x6000], &rawcart[0xe000], 0x2000);

    memset(&roml_banks[0x8000], 0xff, 0x8000);
    memset(&romh_banks[0x8000], 0xff, 0x8000);

    if (extrarom) {
        memcpy(&roml_banks[0x8000], &rawcart[0x10000], 0x2000);
        memcpy(&romh_banks[0x8000], &rawcart[0x12000], 0x2000);
        memcpy(&roml_banks[0xa000], &rawcart[0x14000], 0x2000);
        memcpy(&romh_banks[0xa000], &rawcart[0x16000], 0x2000);
        memcpy(&roml_banks[0xc000], &rawcart[0x18000], 0x2000);
        memcpy(&romh_banks[0xc000], &rawcart[0x1a000], 0x2000);
        memcpy(&roml_banks[0xe000], &rawcart[0x1c000], 0x2000);
        memcpy(&romh_banks[0xe000], &rawcart[0x1e000], 0x2000);
    }

    // Initialize RAM banks to 0xff or zero if desired
    for (int i = 0; i < 8; i++) {
        memset(comal80_ram_lo[i], 0xFF, 0x2000);
        memset(comal80_ram_hi[i], 0xFF, 0x2000);
    }

    cart_config_changed_slotmain(CMODE_8KGAME, CMODE_8KGAME, CMODE_READ);
}

/* ---------------------------------------------------------------------*/

static int set_comal80_variant(int val, void *param)
{
    comal80_variant = val ? 1 : 0;
    return 0;
}

static const resource_int_t resources_int[] = {
    { "Comal80Revision", VARIANT_BLACK, RES_EVENT_NO, NULL,
      &comal80_variant, set_comal80_variant, NULL },
    RESOURCE_INT_LIST_END
};

int comal80_resources_init(void)
{
    return resources_register_int(resources_int);
}

void comal80_resources_shutdown(void)
{

}

static const cmdline_option_t cmdline_options[] =
{
    { "-comal80rev", SET_RESOURCE, CMDLINE_ATTRIB_NEED_ARGS,
      NULL, NULL, "Comal80Revision", NULL,
      "<Revision>", "Set Comal 80 Revision (0: Grey, 1: Commodore/Black)" },
    CMDLINE_LIST_END
};

int comal80_cmdline_options_init(void)
{
    if (cmdline_register_options(cmdline_options) < 0) {
        return -1;
    }
    return 0;
}

static int comal80_common_attach(void)
{
    DBG(("using comal 80 variant: %s", (comal80_variant == VARIANT_GREY) ? "grey" : "black (commodore)"));
    if (export_add(&export_res) < 0) {
        return -1;
    }
    comal80_list_item = io_source_register(&comal80_device);
    return 0;
}

int comal80_bin_attach(const char *filename, uint8_t *rawcart)
{
    extrarom = 1;
    if (util_file_load(filename, rawcart, 0x20000, UTIL_FILE_LOAD_SKIP_ADDRESS) < 0) {
        extrarom = 0;
        if (util_file_load(filename, rawcart, 0x10000, UTIL_FILE_LOAD_SKIP_ADDRESS) < 0) {
            return -1;
        }
    }
    return comal80_common_attach();
}

int comal80_crt_attach(FILE *fd, uint8_t *rawcart, int variant)
{
    crt_chip_header_t chip;

    extrarom = 0;
    /* NOTE: in the CRT file we use 0 for the "black" variant, 1 for "grey" */
    comal80_variant = (variant == 1) ? VARIANT_GREY : VARIANT_BLACK;

    while (1) {
        if (crt_read_chip_header(&chip, fd)) {
            break;
        }

        if (chip.start != 0x8000 || chip.size != 0x4000 || chip.bank > 7) {
            return -1;
        }

        if (crt_read_chip(rawcart, chip.bank << 14, &chip, fd)) {
            return -1;
        }

        if (chip.bank > 3) {
            extrarom = 1;
        }
    }
    return comal80_common_attach();
}

void comal80_detach(void)
{
    export_remove(&export_res);
    io_source_unregister(comal80_list_item);
    comal80_list_item = NULL;
    comal80_variant = VARIANT_BLACK;
}

/* ---------------------------------------------------------------------*/

/* CARTCOMAL snapshot module format:

   type  | name     | description
   ------------------------------
   BYTE  | register | control register
   BYTE  | extra rom| image contains extra eprom
   BYTE  | revision | hardware variant
   ARRAY | ROML     | 32768 or 65536 BYTES of ROML data
   ARRAY | ROMH     | 32768 or 65536 BYTES of ROMH data
   ARRAY | RAMLO    | 65536 BYTES of RAMLO data (8 x 8KB)
   ARRAY | RAMHI    | 65536 BYTES of RAMHI data (8 x 8KB)
 */

static const char snap_module_name[] = "CARTCOMAL";
#define SNAP_MAJOR   1
#define SNAP_MINOR   0

int comal80_snapshot_write_module(snapshot_t *s)
{
    snapshot_module_t *m;

    m = snapshot_module_create(s, snap_module_name, SNAP_MAJOR, SNAP_MINOR);

    if (m == NULL) {
        return -1;
    }

    if (0
        || (SMW_B(m, (uint8_t)currregval) < 0)
        || (SMW_B(m, (uint8_t)extrarom) < 0)
        || (SMW_B(m, (uint8_t)comal80_variant) < 0)
        || (SMW_BA(m, roml_banks, extrarom ? 0x10000 : 0x8000) < 0)
        || (SMW_BA(m, romh_banks, extrarom ? 0x10000 : 0x8000) < 0)
        || (SMW_BA(m, comal80_ram_lo, 0x10000) < 0)
        || (SMW_BA(m, comal80_ram_hi, 0x10000) < 0)) {
        snapshot_module_close(m);
        return -1;
    }

    return snapshot_module_close(m);
}

int comal80_snapshot_read_module(snapshot_t *s)
{
    uint8_t vmajor, vminor;
    snapshot_module_t *m;

    m = snapshot_module_open(s, snap_module_name, &vmajor, &vminor);

    if (m == NULL) {
        return -1;
    }

    /* Do not accept versions higher than current */
    if (snapshot_version_is_bigger(vmajor, vminor, SNAP_MAJOR, SNAP_MINOR)) {
        snapshot_set_error(SNAPSHOT_MODULE_HIGHER_VERSION);
        goto fail;
    }

    if (0
        || (SMR_B_INT(m, &currregval) < 0)
        || (SMR_B_INT(m, &extrarom) < 0)
        || (SMR_B_INT(m, &comal80_variant) < 0)
        || (SMR_BA(m, roml_banks, extrarom ? 0x10000 : 0x8000) < 0)
        || (SMR_BA(m, romh_banks, extrarom ? 0x10000 : 0x8000) < 0)
        || (SMR_BA(m, comal80_ram_lo, 0x10000) < 0)
        || (SMR_BA(m, comal80_ram_hi, 0x10000) < 0)) {
        goto fail;
    }

    snapshot_module_close(m);

    currbank = currregval & 7;
    currbank_is_ram = (currregval & 8) ? 1 : 0;

    return comal80_common_attach();

fail:
    snapshot_module_close(m);
    return -1;
}
