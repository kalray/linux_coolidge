/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2017-2023 Kalray Inc.
 * Author(s): Yann Sionneau
 *            Clement Leger
 */

#ifndef _ASM_KVX_CLOCKSOURCE_H
#define _ASM_KVX_CLOCKSOURCE_H

#include <linux/compiler.h>

struct arch_clocksource_data {
	void __iomem *regs;
};

#endif /* _ASM_KVX_CLOCKSOURCE_H */
