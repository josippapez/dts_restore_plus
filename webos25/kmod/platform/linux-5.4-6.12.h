/* SPDX-License-Identifier: GPL-2.0-only */
/* 5.4.268 and 6.12.44 arm64 as LG builds them for webOS 25: module, kprobe and
 * break-hook ABI. build.py passes the per-kernel values with -D; the defaults are 5.4. */
#ifndef EARC_PLATFORM_LINUX_5_4_6_12_H
#define EARC_PLATFORM_LINUX_5_4_6_12_H

/* sizeof(struct module) and its init/exit offsets in the stock aspectratiodrv.ko. */
#ifndef EARC_MODULE_SIZE
#define EARC_MODULE_SIZE 0x380
#define EARC_MODULE_INIT 0x150
#define EARC_MODULE_EXIT 0x338
#endif
#ifndef EARC_VERMAGIC
#define EARC_VERMAGIC "5.4.268 SMP preempt mod_unload aarch64"
#endif
#ifndef EARC_PRINTK_SYMBOL
#define EARC_PRINTK_SYMBOL "printk"
#endif
/* sizeof(struct kprobe) and the offset of its last field (flags): 5.4 0x88/0x80;
 * 6.12 0x80/0x78 after fault_handler was dropped. The kernel touches nothing past flags. */
#ifndef EARC_KPROBE_SIZE
#define EARC_KPROBE_SIZE 0x88
#define EARC_KPROBE_FLAGS 0x80
#endif

#include "../core/hook.h"

extern int printk(const char *format, ...) __asm__(EARC_PRINTK_SYMBOL);
extern void *kthread_create_on_node(int (*function)(void *), void *data, int node, const char *name, ...);
extern int wake_up_process(void *task);
extern int kthread_stop(void *task);
extern _Bool kthread_should_stop(void);
extern void msleep(unsigned int milliseconds);
extern void mutex_lock(void *mutex);
extern void mutex_unlock(void *mutex);
/* Exported on every 5.4 and 6.12 kernel measured (build.py proves it).
 * register_kernel_break_hook, unregister_kernel_break_hook and
 * aarch64_insn_patch_text are not exported; targets call them via kallsyms. */
extern int register_kprobe(void *kp);
extern void unregister_kprobe(void *kp);
extern void cpus_read_lock(void);
extern void cpus_read_unlock(void);
/* copy_from_user is inlined on both generations. Returns bytes not copied. */
extern unsigned long __arch_copy_from_user(void *to, const void *from, unsigned long n);
#define copy_from_user __arch_copy_from_user

/* Only addr and pre_handler are set; the kernel fills the rest. */
struct target_kprobe;
typedef int (*prehandler_fn)(struct target_kprobe *, struct target_pt_regs *);
struct target_kprobe {
    u8 before_addr[0x28];
    void *addr;                 /* +0x28 */
    const char *symbol_name;    /* +0x30 */
    u32 offset;                 /* +0x38 */
    u8 pad_3c[4];
    prehandler_fn pre_handler;  /* +0x40 */
    u8 tail[EARC_KPROBE_SIZE - 0x48];
};
_Static_assert(sizeof(struct target_kprobe) == EARC_KPROBE_SIZE, "kprobe size");
_Static_assert(__builtin_offsetof(struct target_kprobe, addr) == 0x28, "kprobe addr");
_Static_assert(__builtin_offsetof(struct target_kprobe, offset) == 0x38, "kprobe offset");
_Static_assert(__builtin_offsetof(struct target_kprobe, pre_handler) == 0x40, "kprobe pre_handler");
_Static_assert(EARC_KPROBE_FLAGS + 4 <= EARC_KPROBE_SIZE, "kprobe flags within size");
/* register_kernel_break_hook's struct: list, fn, u16 imm, u16 mask. */
struct target_break_hook {
    void *next;           /* +0 */
    void *prev;           /* +8 */
    void *fn;             /* +16 */
    unsigned short imm;   /* +24 */
    unsigned short mask;  /* +26 */
    u8 pad[4];
};
_Static_assert(sizeof(struct target_break_hook) == 32, "break_hook size");
_Static_assert(__builtin_offsetof(struct target_break_hook, fn) == 16, "break_hook fn");
_Static_assert(__builtin_offsetof(struct target_break_hook, imm) == 24, "break_hook imm");
_Static_assert(__builtin_offsetof(struct target_break_hook, mask) == 26, "break_hook mask");

#endif
