/*
 * SHIX 2.0 board description
 *
 * Copyright (c) 2005 Samuel Tardieu
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
/*
   Shix 2.0 board by Alexis Polti, described at
   http://perso.enst.fr/~polti/realisations/shix20/

   More information in target-sh4/README.sh4
*/
#include "hw/hw.h"
#include "hw/sh4/sh.h"
#include "sysemu/sysemu.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "exec/address-spaces.h"

#define BIOS_FILENAME "shix_bios.bin"
#define BIOS_ADDRESS 0xA0000000

static void shix_init(QEMUMachineInitArgs *args)
{
    const char *cpu_model = args->cpu_model;
    int ret;
    CPUSH4State *env;
    struct SH7750State *s;
    MemoryRegion *sysmem = get_system_memory();
    MemoryRegion *rom = g_new(MemoryRegion, 1);
    MemoryRegion *sdram = g_new(MemoryRegion, 2);
    
    if (!cpu_model)
        cpu_model = "any";

    printf("Initializing CPU\n");
    env = cpu_init(cpu_model);

    /* Allocate memory space */
    printf("Allocating ROM\n");
    memory_region_init_ram(rom, "shix.rom", 0x4000);
    vmstate_register_ram_global(rom);
    memory_region_set_readonly(rom, true);
    memory_region_add_subregion(sysmem, 0x00000000, rom);
    printf("Allocating SDRAM 1\n");
    memory_region_init_ram(&sdram[0], "shix.sdram1", 0x01000000);
    vmstate_register_ram_global(&sdram[0]);
    memory_region_add_subregion(sysmem, 0x08000000, &sdram[0]);
    printf("Allocating SDRAM 2\n");
    memory_region_init_ram(&sdram[1], "shix.sdram2", 0x01000000);
    vmstate_register_ram_global(&sdram[1]);
    memory_region_add_subregion(sysmem, 0x0c000000, &sdram[1]);

    /* Load BIOS in 0 (and access it through P2, 0xA0000000) */
    if (bios_name == NULL)
        bios_name = BIOS_FILENAME;
    printf("%s: load BIOS '%s'\n", __func__, bios_name);
    ret = load_image_targphys(bios_name, 0, 0x4000);
    if (ret < 0) {		/* Check bios size */
	fprintf(stderr, "ret=%d\n", ret);
	fprintf(stderr, "qemu: could not load SHIX bios '%s'\n",
		bios_name);
	exit(1);
    }

    /* Register peripherals */
    s = sh7750_init(env, sysmem);
    /* XXXXX Check success */
    tc58128_init(s, "shix_linux_nand.bin", NULL);
    fprintf(stderr, "initialization terminated\n");
}

static QEMUMachine shix_machine = {
    .name = "shix",
    .desc = "shix card",
    .init = shix_init,
    .is_default = 1,
    DEFAULT_MACHINE_OPTIONS,
};

static void shix_machine_init(void)
{
    qemu_register_machine(&shix_machine);
}

machine_init(shix_machine_init);
