// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2017-2023 Kalray Inc.
 * Author(s): Clement Leger
 *            Yann Sionneau
 *            Marius Gligor
 *            Guillaume Thouvenin
 */

#include <linux/types.h>
#include <linux/string.h>
#include <linux/atomic.h>
#include <linux/cpumask.h>
#include <linux/uaccess.h>
#include <linux/stop_machine.h>

#include <asm/cacheflush.h>
#include <asm/insns_defs.h>
#include <asm/fixmap.h>

struct insns_patch {
	atomic_t cpu_count;
	u32 *addr;
	u32 *insns;
	unsigned long insns_len;
};

static void *insn_patch_map(void *addr)
{
	unsigned long uintaddr = (uintptr_t) addr;
	bool module = !core_kernel_text(uintaddr);
	struct page *page;

	if (module && IS_ENABLED(CONFIG_STRICT_MODULE_RWX))
		page = vmalloc_to_page(addr);
	else if (!module && IS_ENABLED(CONFIG_STRICT_KERNEL_RWX))
		page = phys_to_page(__pa_symbol(addr));
	else
		return addr;

	BUG_ON(!page);
	return (void *)set_fixmap_offset(FIX_TEXT_PATCH, page_to_phys(page) +
			(uintaddr & ~PAGE_MASK));
}

static void insn_patch_unmap(void)
{
	clear_fixmap(FIX_TEXT_PATCH);
}

int kvx_insns_write_nostop(u32 *insns, u8 insns_len, u32 *insn_addr)
{
	unsigned long current_insn_addr = (unsigned long) insn_addr;
	unsigned long len_remain = insns_len;
	unsigned long next_insn_page, patch_len;
	void *map_patch_addr;
	int ret = 0;

	do {
		/* Compute next upper page boundary */
		next_insn_page = (current_insn_addr + PAGE_SIZE) & PAGE_MASK;

		patch_len = min(next_insn_page - current_insn_addr, len_remain);
		len_remain -= patch_len;

		/* Map & patch patch insns */
		map_patch_addr = insn_patch_map((void *) current_insn_addr);
		ret = copy_to_kernel_nofault(map_patch_addr, insns, patch_len);
		if (ret)
			break;

		insns = (void *) insns + patch_len;
		current_insn_addr = next_insn_page;

	} while (len_remain);

	insn_patch_unmap();

	/*
	 * Flush & invalidate L2 + L1 icache to reload instructions from memory
	 * L2 wbinval is necessary because we write through DEVICE cache policy
	 * mapping which is uncached therefore L2 is bypassed
	 */
	wbinval_icache_range(virt_to_phys(insn_addr), insns_len);

	return ret;
}

/* This function is called by all CPUs in parallel */
static int patch_insns_percpu(void *data)
{
	struct insns_patch *ip = data;
	unsigned long insn_addr = (unsigned long) ip->addr;
	int ret;

	/* Only the first CPU writes the instruction(s) */
	if (atomic_inc_return(&ip->cpu_count) == 1) {

		/*
		 * This will write the instruction(s) through uncached mapping
		 * and then flush/inval L2$ lines which itself will inval all
		 * PE L1d$ lines. It will after Invalid current PE L1i$ lines.
		 * Therefore cache coherency is handled for the calling CPU.
		 */
		ret = kvx_insns_write_nostop(ip->insns, ip->insns_len,
					     insn_addr);
		/*
		 * Increment once more to signal that instructions have been written
		 * to memory.
		 */
		atomic_inc(&ip->cpu_count);

		return ret;
	}

	/*
	 * Synchronization point: remaining CPUs need to wait for the first
	 * CPU to complete patching the instruction(s).
	 * The completion is signaled when the counter reaches
	 * `num_online_cpus + 1`: meaning that every CPU has entered this
	 * function and the first CPU has completed its operations.
	 */
	while (atomic_read(&ip->cpu_count) <= num_online_cpus())
		cpu_relax();

	/*
	 * Other CPUs simply invalidate their L1 I-cache to reload
	 * from L2 or memory. Their L1 D-cache as well as the L2$ have
	 * already been invalidated by the kvx_insns_write_nostop() call.
	 */
	l1_inval_icache_range(insn_addr, insn_addr + ip->insns_len);
	return 0;
}

/**
 * kvx_insns_write() Patch instructions at a specified address
 * @insns: Instructions to be written at @addr
 * @insns_len: Size of instructions to patch
 * @addr: Address of the first instruction to patch
 */
int kvx_insns_write(u32 *insns, unsigned long insns_len, u32 *addr)
{
	struct insns_patch ip = {
		.cpu_count = ATOMIC_INIT(0),
		.addr = addr,
		.insns = insns,
		.insns_len = insns_len
	};

	if (!insns_len)
		return -EINVAL;

	if (!IS_ALIGNED((unsigned long) addr, KVX_INSN_SYLLABLE_WIDTH))
		return -EINVAL;

	/*
	 * Function name is a "bit" misleading. while being named
	 * stop_machine, this function does not stop the machine per se
	 * but execute the provided function on all CPU in a safe state.
	 */
	return stop_machine(patch_insns_percpu, &ip, cpu_online_mask);
}

int kvx_insns_read(u32 *insns, unsigned long insns_len, u32 *addr)
{
	l1_inval_dcache_range((unsigned long)addr, insns_len);
	return copy_from_kernel_nofault(insns, addr, insns_len);
}
