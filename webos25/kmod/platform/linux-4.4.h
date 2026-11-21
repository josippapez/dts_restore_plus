/* SPDX-License-Identifier: GPL-2.0-only */
/* Linux 4.4.84 arm64 as LG builds it for webOS 6: module and hook ABI.
 * Layouts measured from the stock si2151.ko and the kernel's own code. */
#ifndef EARC_PLATFORM_LINUX_4_4_H
#define EARC_PLATFORM_LINUX_4_4_H

#include "../core/hook.h"

#define EARC_MODULE_SIZE 0x300
#define EARC_MODULE_INIT 0x158
#define EARC_MODULE_EXIT 0x2d8
#define EARC_VERMAGIC "4.4.84 SMP preempt mod_unload aarch64"

extern int printk(const char *format, ...);
extern unsigned long kallsyms_lookup_name(const char *name);
extern int misc_register(void *device);
extern void misc_deregister(void *device);
extern unsigned long __copy_from_user(void *to, const void *from, unsigned long length);
extern void get_online_cpus(void);
extern void put_online_cpus(void);

/* struct break_hook {list_head; u32 esr; u32 esr_mask; fn}, read by brk_handler +0x58/+0x68. */
struct target_break_hook {
    void *next, *prev;
    u32 esr, esr_mask;
    int (*fn)(struct target_pt_regs *, unsigned int);
};
_Static_assert(sizeof(struct target_break_hook) == 32, "break_hook size");

/* struct file_operations slots from __vfs_write, misc_open and __fput. */
#define EARC_PLATFORM_FOPS_WRITE 0x18
#define EARC_PLATFORM_FOPS_OPEN 0x58
#define EARC_PLATFORM_FOPS_RELEASE 0x68

/* 4.4's brk_handler calls hooks outside RCU, so a hook cannot be safely removed
 * while LG code runs: modules on this platform have no exit. */
static int earc_break(struct target_pt_regs *regs, unsigned int syndrome);
static struct target_break_hook earc_platform_hook = {
    .esr = 0xf2000000U | EARC_BRK_IMMEDIATE, .esr_mask = ~0U, .fn = earc_break};

static void earc_platform_hook_register(void)
{
    ((void (*)(void *))kallsyms_lookup_name("register_break_hook"))(&earc_platform_hook);
}

static void earc_platform_hook_unregister(void)
{
    ((void (*)(void *))kallsyms_lookup_name("unregister_break_hook"))(&earc_platform_hook);
}

static void earc_platform_symbol_size(u64 address, unsigned long *size, unsigned long *offset)
{
    ((int (*)(u64, unsigned long *, unsigned long *))kallsyms_lookup_name("kallsyms_lookup_size_offset"))(
        address, size, offset);
}

static int earc_platform_patch(void **addresses, u32 *words, int count)
{
    int result;
    get_online_cpus();
    result = ((int (*)(void **, u32 *, int))kallsyms_lookup_name("aarch64_insn_patch_text"))(addresses, words, count);
    put_online_cpus();
    return result;
}

/* thread_info.addr_limit sits at +8 of the 16 KiB stack base on 4.4. */
static int earc_platform_copy_byte(char *value, const char *user)
{
    u64 stack, limit;
    __asm__("mov %0, sp" : "=r"(stack));
    limit = *(u64 *)((stack & ~16383UL) + 8);
    return (u64)user > limit || __copy_from_user(value, user, 1);
}

#endif
