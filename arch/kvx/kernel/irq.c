// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2017-2023 Kalray Inc.
 * Author(s): Clement Leger
 */

#include <linux/irqdomain.h>
#include <linux/irqflags.h>
#include <linux/irqchip.h>
#include <linux/bitops.h>
#include <linux/init.h>

#define IT_MASK(__it) (KVX_SFR_ILL_ ## __it ## _MASK)
#define IT_LEVEL(__it, __level) \
	(__level##ULL << KVX_SFR_ILL_ ## __it ## _SHIFT)

/*
 * Early Hardware specific Interrupt setup
 * - Called very early (start_kernel -> setup_arch -> setup_processor)
 * - Needed for each CPU
 */
void kvx_init_core_irq(void)
{
	/*
	 * On KVX, the kernel only cares about the following ITs:
	 * - IT0: Timer 0
	 * - IT2: Watchdog
	 * - IT4: APIC IT 1
	 * - IT24: IPI
	 */
	uint64_t mask = IT_MASK(IT0) | IT_MASK(IT2) | IT_MASK(IT4) |
			IT_MASK(IT24);

	/*
	 * Specific priorities for ITs:
	 * - Watchdog has the highest priority: 3
	 * - Timer has priority: 2
	 * - APIC entries have the lowest priority: 1
	 */
	uint64_t value = IT_LEVEL(IT0, 0x2) | IT_LEVEL(IT2, 0x3) |
			IT_LEVEL(IT4, 0x1) | IT_LEVEL(IT24, 0x1);

	kvx_sfr_set_mask(ILL, mask, value);

	/* Set core level to 0 */
	kvx_sfr_set_field(PS, IL, 0);
}

void __init init_IRQ(void)
{
	irqchip_init();
}
