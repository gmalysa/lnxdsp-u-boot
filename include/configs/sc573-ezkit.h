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

#ifndef __CONFIG_SC573_EZKIT_H
#define __CONFIG_SC573_EZKIT_H

#include <linux/sizes.h>

/*
 * Memory Settings
 */
#define MEM_MT41K128M16JT

#define CFG_SYS_SDRAM_BASE	0x82000000
#define CFG_SYS_SDRAM_SIZE	0xe000000

/*
 * Non-Kconfig option for designware driver
 */
#define CONFIG_DW_AXI_BURST_LEN 16

#endif
