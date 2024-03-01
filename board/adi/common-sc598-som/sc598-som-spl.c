// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * (C) Copyright 2022 - Analog Devices, Inc.
 *
 * Written and/or maintained by Timesys Corporation
 *
 * Contact: Nathan Barrett-Morrison <nathan.morrison@timesys.com>
 * Contact: Greg Malysa <greg.malysa@timesys.com>
 *
 * U-boot - SPL management
 *
 */

#include <asm/io.h>
#include <asm/arch-adi/sc5xx/sc5xx.h>
#include <asm/arch-adi/sc5xx/spl.h>
#include <dm/ofnode.h>
#include <mmc.h>

#define pRCU_MSG		((void __iomem *)0x3108C06C)

// Table 43-14 in SC598 hardware reference manual
const struct adi_boot_args adi_rom_boot_args[] = {
	// JTAG/no boot
	[0] = {0, 0, 0},
	// SPI master, used for qspi as well
	[1] = {0x60040000, 0x00040000, 0x00000207},
	// SPI slave
	[2] = {0, 0, 0x00000212},
	// UART slave
	[3] = {0, 0, 0x00000013},
	// Linkport slave
	[4] = {0, 0, 0x00000014},
	// OSPI master
	[5] = {0x60040000, 0, 0x00000008},
	// eMMC
	[6] = {0x201, 0, 0x86009},
	// reserved, also no boot
	[7] = {0, 0, 0}
};

unsigned long spl_mmc_get_uboot_raw_sector(struct mmc *mmc,
					   unsigned long raw_sect)
{
	ulong mmc_sector_offs = 0;

	if (IS_ENABLED(CONFIG_OF_CONTROL) && !IS_ENABLED(CONFIG_OF_PLATDATA))
		mmc_sector_offs = ofnode_conf_read_int("u-boot,spl-mmc-sector-offset",
						       mmc_sector_offs);

	return mmc_sector_offs;
}

struct ADI_ROM_BOOT_CONFIG {
	void *src;
	void *dest;
	s32 byte_count;
	s32 flags;
	u32 block_count;
	u32 block_current;
	void *next_dxe;
	u32 byte_address;
	u32 *control_register;
	s32 control_value;
	u32 *peripheral_base;
	u32 *aux_control_register;
	u32 *aux_peripheral_base;
	u32 *sec_control_register;
	void *dma_base_register;
	s32 load_type;
	struct {
		u32 operation;
		u32 id;
		void *src;
		void *destination;
		u32 byte_count;
		s32 done_detect;
		u32 crc_ctl;
		u32 fill_value;
		u32 crc_poly;
		u32 crc_compare;
	} mdma_cfg;
	u16 data_width;
	u16 src_modify_mult;
	u16 dst_modify_mult;
	u16 usr_short;
	s32 user_long;
	s32 reserved0;
	void *mode_data;
	s32 boot_command;
	void *boot_header;
	void *temp_buffer;
	void *reserved1;
	s32 temp_byte_count;
	void *temp_src;
	s32 page_byte_count;
	struct {
		struct {
			u8 *buffer;
			u32 size;
			u32 page_size;
		} buffer[2];
		u32 state;
		void *src;
		void *dma;
	} boot_buffers;
	struct {
		void *init;
		void *config;
		void *load;
		void *cleanup;
		void *reserved0;
		s32 reserved1;
	} registry;
	void (*error)(struct ADI_ROM_BOOT_CONFIG *config);
	// @todo there are more fields but this should be enough for now
};

static void adi_rom_boot_error_handler(struct ADI_ROM_BOOT_CONFIG *config)
{
	printf("bootrom failed, rcu msg = 0x%x\n", readl(pRCU_MSG));
	while (1)
		;
}

int32_t adi_rom_boot_hook(struct ADI_ROM_BOOT_CONFIG *config, int32_t cause)
{
	config->error = &adi_rom_boot_error_handler;
	return 0;
}
