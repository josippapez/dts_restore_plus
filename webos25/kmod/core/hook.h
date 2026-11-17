/* SPDX-License-Identifier: GPL-2.0-only */
/* Shared types a target uses to describe its hooks: where, what to check, what to change. */
#ifndef EARC_CORE_HOOK_H
#define EARC_CORE_HOOK_H

typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long u64;
typedef long s64;

#define EARC_BRK_IMMEDIATE 0x4c31U
#define EARC_BRK_INSTRUCTION (0xd4200000U | (EARC_BRK_IMMEDIATE << 5))

enum earc_when {
    EARC_WHEN_EARC_PORT = 1,  /* eARC on, the lease is the main audio, x[reg] == value */
    EARC_WHEN_LEASE,          /* the lease is the main audio */
    EARC_WHEN_LEASE_DEVICE,   /* *(u32 *)(*(u64 *)(x[reg] + a) + b) is the leased renderer */
};
enum earc_action {
    EARC_SET_REG = 1,  /* x[reg] = value; counts overrides */
    EARC_CLEAR_U64,    /* *(u64 *)(x[reg] + a) = 0 if set; counts mix_clears */
    EARC_CARRIER,      /* sp + a holds an SRC format: keep 192k/8ch/16bit; counts src_fixes */
};

#define WHEN_EARC_PORT(r, v) .when = EARC_WHEN_EARC_PORT, .when_reg = r, .when_value = v
#define WHEN_LEASE() .when = EARC_WHEN_LEASE
#define WHEN_LEASE_DEVICE(r, x, y) .when = EARC_WHEN_LEASE_DEVICE, .when_reg = r, .when_a = x, .when_b = y
#define SET_REG(r, v) .action = EARC_SET_REG, .reg = r, .value = v
#define CLEAR_U64(r, x) .action = EARC_CLEAR_U64, .reg = r, .a = x
#define CARRIER(x) .action = EARC_CARRIER, .a = x

/* function is an EF_ index of the target's EARC_FUNCTIONS; the site, its address + offset,
 * holds word (pinned by the function's shape), which then runs from a resume slot. */
struct earc_hook {
    u8 function;
    u32 offset, word;
    u8 when, when_reg, action, reg;
    u32 when_value, when_a, when_b, value, a;
};

/* The replaced instruction runs from a slot in module text, so it must not be PC-relative. */
#define EARC_RELOCATABLE(word) \
    (((word) & 0x1f000000U) != 0x10000000U && ((word) & 0x3b000000U) != 0x18000000U && \
     ((word) & 0x7c000000U) != 0x14000000U && ((word) & 0xff000010U) != 0x54000000U && \
     ((word) & 0x7e000000U) != 0x34000000U && ((word) & 0x7e000000U) != 0x36000000U && \
     ((word) & 0xfe000000U) != 0xd6000000U && ((word) & 0xff000000U) != 0xd4000000U)

#endif
