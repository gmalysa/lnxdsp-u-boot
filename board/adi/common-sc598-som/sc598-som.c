// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * (C) Copyright 2022 - Analog Devices, Inc.
 *
 * Written and/or maintained by Timesys Corporation
 *
 * Contact: Nathan Barrett-Morrison <nathan.morrison@timesys.com>
 * Contact: Greg Malysa <greg.malysa@timesys.com>
 */

#include <config.h>
#include <phy.h>
#include <asm/u-boot.h>
#include <asm/arch-adi/sc5xx/sc5xx.h>
#include <asm/arch-adi/sc5xx/soc.h>
#include <asm/armv8/mmu.h>

#include "../carriers/somcrr.h"

static struct mm_region sc598_mem_map[] = {
	{
		/* Peripherals */
		.virt = 0x0UL,
		.phys = 0x0UL,
		.size = 0x80000000UL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* DDR */
		.virt = 0x80000000UL,
		.phys = 0x80000000UL,
		.size = 0x40000000UL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL) |
			 PTE_BLOCK_INNER_SHARE
	}, {
		/* List terminator */
		0,
	}
};

struct mm_region *mem_map = sc598_mem_map;

int board_phy_config(struct phy_device *phydev)
{
	if (IS_ENABLED(CONFIG_ADI_CARRIER_SOMCRR_EZKIT))
		fixup_dp83867_phy(phydev);
	return 0;
}

int board_init(void)
{
	sc59x_remap_ospi();

	if (IS_ENABLED(CONFIG_ADI_CARRIER_SOMCRR_EZKIT) ||
	    IS_ENABLED(CONFIG_ADI_CARRIER_SOMCRR_EZLITE)) {
		adi_somcrr_init_ethernet();
	}

	sc5xx_enable_rgmii();

	return 0;
}

