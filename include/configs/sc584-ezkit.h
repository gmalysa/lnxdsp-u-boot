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

#ifndef __CONFIG_SC584_EZKIT_H
#define __CONFIG_SC584_EZKIT_H

#include <linux/sizes.h>

/*
 * Memory Settings
 */
#define MEM_MT47H128M16RT

#define CFG_SYS_SDRAM_BASE	0x89000000
#define CFG_SYS_SDRAM_SIZE	0x7000000

#endif
