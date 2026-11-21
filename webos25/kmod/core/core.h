/* SPDX-License-Identifier: GPL-2.0-only */
/* Shared core for BRK targets: policy gates, hook-table dispatch, the shape check, patching
 * and the module entry. Included after a platform and a target. */
#ifndef EARC_CORE_H
#define EARC_CORE_H

#include "module.h"
extern struct target_module __this_module;

#define EARC_HOOK_COUNT (sizeof earc_hooks / sizeof earc_hooks[0])

static u32 earc_enabled = 1, earc_hits, earc_overrides, earc_mix_clears, earc_src_fixes;
static struct earc_code earc_fns[EF_COUNT];
static u64 earc_sites[EARC_HOOK_COUNT];
/* Per hook: the replaced instruction, then B back to the next one; UDF until filled at load. */
#define EARC_SLOT_MAX 8
_Static_assert(sizeof earc_hooks / sizeof earc_hooks[0] <= EARC_SLOT_MAX, "too many hooks");
__asm__(".pushsection .text\n.balign 8\nearc_slots:\n.fill 16, 4, 0\n.popsection\n");
extern u32 earc_slots[EARC_SLOT_MAX][2];
#define EARC_LEASE_COPY_BYTE(to, from) earc_platform_copy_byte(to, from)
#include "lease.h"

static int earc_hook_wanted(const struct earc_hook *hook, const struct target_pt_regs *regs)
{
    u32 leased = APP_ROUTE();
    u64 base;
    if (!__atomic_load_n(&earc_enabled, __ATOMIC_RELAXED) || !leased)
        return 0;
    switch (hook->when) {
    case EARC_WHEN_EARC_PORT:
        return *earc_on && leased - 1 == *earc_main_index
            && (u32)regs->regs[hook->when_reg] == hook->when_value;
    case EARC_WHEN_LEASE:
        return leased - 1 == *earc_main_index;
    case EARC_WHEN_LEASE_DEVICE:
        base = regs->regs[hook->when_reg];
        base = base ? *(u64 *)(base + hook->when_a) : 0;
        return base && *(u32 *)(base + hook->when_b) == leased - 1;
    }
    return 0;
}

static void earc_hook_apply(const struct earc_hook *hook, struct target_pt_regs *regs)
{
    u32 *format;
    u64 *field;
    switch (hook->action) {
    case EARC_SET_REG:
        if ((u32)regs->regs[hook->reg] != hook->value)
            __atomic_add_fetch(&earc_overrides, 1, __ATOMIC_RELAXED);
        regs->regs[hook->reg] = hook->value;
        break;
    case EARC_CLEAR_U64:
        field = regs->regs[hook->reg] ? (u64 *)(regs->regs[hook->reg] + hook->a) : 0;
        if (field && *field) {
            *field = 0;
            __atomic_add_fetch(&earc_mix_clears, 1, __ATOMIC_RELAXED);
        }
        break;
    case EARC_CARRIER:
        format = (u32 *)(regs->sp + hook->a);
        if (format[0] == 192000 && format[1] == 8 && format[2] == 16) {
            format[5] = 192000;
            format[6] = 8;
            format[7] = 16;
            __atomic_add_fetch(&earc_src_fixes, 1, __ATOMIC_RELAXED);
        }
        break;
    }
}

/* BRK exception context: touches the saved registers and LG's own structures only. */
static int earc_break(struct target_pt_regs *regs, unsigned int syndrome)
{
    unsigned int index;
    if (!regs || syndrome != (0xf2000000U | EARC_BRK_IMMEDIATE) || (regs->pstate & 15) != 5)
        return 1;
    for (index = 0; index < EARC_HOOK_COUNT; index++) {
        const struct earc_hook *hook = &earc_hooks[index];
        if (regs->pc != earc_sites[index])
            continue;
        if (earc_hook_wanted(hook, regs))
            earc_hook_apply(hook, regs);
        regs->pc = (u64)earc_slots[index];
        __atomic_add_fetch(&earc_hits, 1, __ATOMIC_RELAXED);
        return 0;
    }
    return 1;
}

/* Every function by name, sized up to the next symbol, of the analysed shape; then the
 * target's data and the resume slots. The first mismatch, or 0. */
static const char *earc_verify(void)
{
    unsigned int index;
    const char *mismatch;
    for (index = 0; index < EF_COUNT; index++) {
        unsigned long size = 0, offset = 1;
        earc_fns[index].address = kallsyms_lookup_name(earc_functions[index].name);
        if (earc_fns[index].address)
            earc_platform_symbol_size(earc_fns[index].address, &size, &offset);
        if (offset || size != 4UL * earc_functions[index].count)
            return earc_functions[index].name;
        earc_fns[index].words = (const u32 *)earc_fns[index].address;
        earc_fns[index].count = earc_functions[index].count;
    }
    mismatch = earc_check(earc_fns);
    if (mismatch || (mismatch = earc_target_derive(earc_fns)))
        return mismatch;
    for (index = 0; index < EARC_HOOK_COUNT; index++) {
        const struct earc_hook *hook = &earc_hooks[index];
        u64 site = earc_fns[hook->function].address + hook->offset, slot = (u64)&earc_slots[index][1];
        s64 back = (s64)(site + 4 - slot);
        if (hook->offset + 4 > 4 * earc_fns[hook->function].count || !EARC_RELOCATABLE(hook->word)
            || back >= (1L << 27) || back < -(1L << 27))
            return earc_functions[hook->function].name;
        earc_sites[index] = site;
    }
    return 0;
}

static int earc_sites_hold(int brk)
{
    unsigned int index;
    for (index = 0; index < EARC_HOOK_COUNT; index++)
        if (*(volatile u32 *)earc_sites[index] != (brk ? EARC_BRK_INSTRUCTION : earc_hooks[index].word))
            return 0;
    return 1;
}

static int earc_patch_sites(int brk)
{
    void *addresses[EARC_HOOK_COUNT];
    u32 words[EARC_HOOK_COUNT];
    unsigned int index;
    for (index = 0; index < EARC_HOOK_COUNT; index++) {
        addresses[index] = (void *)earc_sites[index];
        words[index] = brk ? EARC_BRK_INSTRUCTION : earc_hooks[index].word;
    }
    return earc_platform_patch(addresses, words, EARC_HOOK_COUNT);
}

/* Module text is read-only by now: the slots go through the kernel's own text patcher. */
static int earc_fill_slots(void)
{
    void *addresses[2 * EARC_HOOK_COUNT];
    u32 words[2 * EARC_HOOK_COUNT];
    unsigned int index;
    for (index = 0; index < EARC_HOOK_COUNT; index++) {
        u64 slot = (u64)earc_slots[index];
        addresses[2 * index] = (void *)slot;
        words[2 * index] = earc_hooks[index].word;
        addresses[2 * index + 1] = (void *)(slot + 4);
        words[2 * index + 1] = 0x14000000U | (u32)(((earc_sites[index] - slot) >> 2) & 0x3ffffffU);
    }
    if (earc_platform_patch(addresses, words, 2 * EARC_HOOK_COUNT))
        return -1;
    for (index = 0; index < 2 * EARC_HOOK_COUNT; index++)
        if (*(volatile u32 *)addresses[index] != words[index])
            return -1;
    return 0;
}

int init_module(void)
{
    const char *mismatch = earc_verify();
    int result;
    if (mismatch) {
        printk("\0013" EARC_NAME ": refused; not the analysed code (%s)\n", mismatch);
        return -19;
    }
    if (!earc_sites_hold(0)) {
        printk("\0013" EARC_NAME ": refused; a hook site is already patched\n");
        return -16;
    }
    if (earc_fill_slots()) {
        printk("\0013" EARC_NAME ": refused; resume slots not written\n");
        return -5;
    }
    lease_prepare(&__this_module, EARC_PLATFORM_FOPS_WRITE, EARC_PLATFORM_FOPS_OPEN, EARC_PLATFORM_FOPS_RELEASE);
    result = misc_register(&lease_device);
    if (result) {
        printk("\0013" EARC_NAME ": refused; /dev/earc_route unavailable (%d)\n", result);
        return result;
    }
    earc_platform_hook_register();
    result = earc_patch_sites(1);
    if (earc_sites_hold(1)) {
        printk("\0016" EARC_NAME ": armed; %u hooks, lease /dev/earc_route minor %d\n",
               (unsigned int)EARC_HOOK_COUNT, lease_device.minor);
        return 0;
    }
    if (earc_sites_hold(0)) {
        earc_platform_hook_unregister();
        misc_deregister(&lease_device);
        printk("\0013" EARC_NAME ": refused; text patch failed (%d)\n", result);
        return result ? result : -5;
    }
    /* Partly patched: the BRK handler must stay, so stay loaded with the policy off. */
    __atomic_store_n(&earc_enabled, 0, __ATOMIC_RELAXED);
    printk("\0013" EARC_NAME ": partial patch (%d); staying loaded with enabled=0\n", result);
    return 0;
}

/* No exit function: the kernel then refuses rmmod (see the platform for why). */
EARC_THIS_MODULE(EARC_NAME, init_module, 0);
EARC_PARAM_TABLE = {
    EARC_PARAM_UINT("enabled", 0644, earc_enabled),
    EARC_PARAM_UINT("hits", 0444, earc_hits),
    EARC_PARAM_UINT("lease", 0444, app_route),
    EARC_PARAM_UINT("mix_clears", 0444, earc_mix_clears),
    EARC_PARAM_UINT("overrides", 0444, earc_overrides),
    EARC_PARAM_UINT("src_fixes", 0444, earc_src_fixes),
};
EARC_MODINFO(, "");

#endif
