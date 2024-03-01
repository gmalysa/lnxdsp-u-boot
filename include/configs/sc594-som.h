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

#ifndef __CONFIG_SC594_SOM_H
#define __CONFIG_SC594_SOM_H

#include <linux/sizes.h>

/*
 * Memory Settings
 */
#define MEM_IS43TR16512BL
#define MEM_ISSI_8Gb_DDR3_800MHZ

#define CFG_SYS_SDRAM_BASE	0x82000000
#define CFG_SYS_SDRAM_SIZE	0x3E000000

/*
 * Non-Kconfig option for designware driver
 */
#define CONFIG_DW_AXI_BURST_LEN 16

/* Push button polarties -- used for Falcon boot interrupt */
#define ADI_PB1_POLARITY 1
#define ADI_PB2_POLARITY 1

#endif
