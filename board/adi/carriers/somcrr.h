/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * (C) Copyright 2022 - Analog Devices, Inc.
 *
 * Written and/or maintained by Timesys Corporation
 *
 * Contact: Nathan Barrett-Morrison <nathan.morrison@timesys.com>
 * Contact: Greg Malysa <greg.malysa@timesys.com>
 *
 * U-boot - Functions which are shared between u-boot proper + u-boot SPL
 *
 */

#ifndef ADI_CARRIERS_SOMCRR_H
#define ADI_CARRIERS_SOMCRR_H

void adi_somcrr_init_ethernet(void);
void adi_somcrr_enable_ethernet(void);
void adi_somcrr_disable_ethernet(void);

#endif

