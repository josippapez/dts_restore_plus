/* SPDX-License-Identifier: GPL-2.0-only */
/* struct module, struct kernel_param and .modinfo, shared by every target.
 * Before including: EARC_MODULE_SIZE, EARC_MODULE_INIT, EARC_MODULE_EXIT (sizeof
 * struct module and its init/exit offsets in the generation's stock module) and
 * EARC_VERMAGIC. Each platform header sets these; build.py checks them. */
#ifndef EARC_CORE_MODULE_H
#define EARC_CORE_MODULE_H

struct target_module {
    unsigned char prefix[24];
    char name[56];
    unsigned char before_init[EARC_MODULE_INIT - 80];
    int (*initialise)(void);
    unsigned char before_exit[EARC_MODULE_EXIT - EARC_MODULE_INIT - 8];
    void (*finish)(void);
    unsigned char suffix[EARC_MODULE_SIZE - EARC_MODULE_EXIT - 8];
};
_Static_assert(sizeof(struct target_module) == EARC_MODULE_SIZE, "module size");
_Static_assert(__builtin_offsetof(struct target_module, name) == 24, "module name");
_Static_assert(__builtin_offsetof(struct target_module, initialise) == EARC_MODULE_INIT, "module init");
_Static_assert(__builtin_offsetof(struct target_module, finish) == EARC_MODULE_EXIT, "module exit");

/* Declares __this_module; finish may be 0 (the kernel then refuses rmmod). */
#define EARC_THIS_MODULE(module_name, init, exit)                                                  \
    struct target_module __this_module __attribute__((section(".gnu.linkonce.this_module"),        \
                                                      aligned(64), used)) = {                      \
        .name = module_name, .initialise = init, .finish = exit}

/* moduleparam.h on 4.4, 5.4 and 6.12: ops {flags, set, get, free}; param {name,
 * mod, ops, u16 perm, s8 level, u8 flags, arg}. Permission 0 keeps it out of sysfs. */
extern int param_set_uint(const char *value, const void *parameter);
extern int param_get_uint(char *value, const void *parameter);
extern int param_set_ulong(const char *value, const void *parameter);
extern int param_get_ulong(char *value, const void *parameter);
struct target_parameter_operations {
    unsigned int flags;
    int (*set)(const char *, const void *);
    int (*get)(char *, const void *);
    void (*free)(void *);
};
static const struct target_parameter_operations uint_operations = {0, param_set_uint, param_get_uint, (void *)0};
static const struct target_parameter_operations ulong_operations = {0, param_set_ulong, param_get_ulong, (void *)0};
_Static_assert(sizeof(struct target_parameter_operations) == 32, "parameter operations size");
_Static_assert(__builtin_offsetof(struct target_parameter_operations, get) == 16, "parameter operations get");
struct target_parameter {
    const char *name;
    struct target_module *module;
    const void *operations;
    unsigned short permissions;
    signed char level;
    unsigned char flags;
    void *argument;
};
_Static_assert(sizeof(struct target_parameter) == 40, "parameter size");
#define EARC_PARAM_UINT(param_name, mode, variable) \
    {param_name, &__this_module, &uint_operations, mode, -1, 0, &variable}
#define EARC_PARAM_TABLE \
    static const struct target_parameter parameters[] __attribute__((section("__param"), aligned(8), used))

/* License, vermagic, a target's extra modinfo (description=...), then the empty
 * PLT sections the arm64 loader expects and any extra empty sections the target needs. */
#define EARC_MODINFO(extra_modinfo, extra_sections)                                                \
    static const char module_license[] __attribute__((section(".modinfo"), used)) = "license=GPL"; \
    static const char module_vermagic[] __attribute__((section(".modinfo"), used)) =               \
        "vermagic=" EARC_VERMAGIC;                                                                 \
    extra_modinfo                                                                                  \
    __asm__(".pushsection .plt,\"ax\",%progbits\n.byte 0\n.popsection\n"                           \
            ".pushsection .init.plt,\"a\",%progbits\n.byte 0\n.popsection\n" extra_sections)

#endif
