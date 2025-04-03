// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * (C) Copyright 2022 - Analog Devices, Inc.
 *
 * Written and/or maintained by Timesys Corporation
 *
 * Contact: Nathan Barrett-Morrison <nathan.morrison@timesys.com>
 * Contact: Greg Malysa <greg.malysa@timesys.com>
 */

#include <phy.h>
#include <asm/u-boot.h>
#include <asm/arch-adi/sc5xx/sc5xx.h>
#include <asm/arch-adi/sc5xx/soc.h>

int board_phy_config(struct phy_device *phydev)
{
	fixup_dp83867_phy(phydev);
	return 0;
}

int board_init(void)
{
	sc5xx_enable_rgmii();
	return 0;
}

