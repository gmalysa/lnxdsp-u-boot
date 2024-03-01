/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * (C) Copyright 2022 - Analog Devices, Inc.
 *
 * Written and/or maintained by Timesys Corporation
 *
 * Contact: Nathan Barrett-Morrison <nathan.morrison@timesys.com>
 * Contact: Greg Malysa <greg.malysa@timesys.com>
 *
 */

#ifndef __CONFIG_SC598_SOM_H
#define __CONFIG_SC598_SOM_H

#include <linux/sizes.h>
#include <linux/kconfig.h>

/* GIC */
#define GICD_BASE 0x31200000
#define GICR_BASE 0x31240000

/*
 * Memory Settings
 */
#define MEM_IS43TR16512BL
#define MEM_ISSI_4Gb_DDR3_800MHZ

#define CFG_SYS_SDRAM_BASE	0x90000000
#define CFG_SYS_SDRAM_SIZE	0x0e000000

/* Push button polarties -- used for Falcon boot interrupt */
#define ADI_PB1_POLARITY 1
#define ADI_PB2_POLARITY 1

#endif
