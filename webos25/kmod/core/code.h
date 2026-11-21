/* SPDX-License-Identifier: GPL-2.0-only */
/* The kernel functions a BRK target reads, found by name and accepted by shape. Before
 * including: EARC_FUNCTIONS(X), X(name, count, shape). A shape is FNV-1a over the function's
 * count instructions with every link-time immediate masked (ADRP/ADR, B/BL targets, offsets
 * from an ADRP-derived base): builds of one source share it, while opcodes, registers,
 * struct offsets and constants stay pinned. Shared by the module and test/legacy-audio/check.c. */
#ifndef EARC_CORE_CODE_H
#define EARC_CORE_CODE_H

struct earc_code {
    u64 address;
    const u32 *words;
    u32 count;
};

#define EARC_ENUM(name, count, shape) EF_##name,
enum { EARC_FUNCTIONS(EARC_ENUM) EF_COUNT };
#undef EARC_ENUM

static const struct earc_function {
    const char *name;
    u32 count, shape;
} earc_functions[] = {
#define EARC_ROW(name, count, shape) {#name, count, shape},
    EARC_FUNCTIONS(EARC_ROW)
#undef EARC_ROW
};

struct earc_scan {
    u32 shape, found;  /* found: accesses of the requested width and direction */
    u64 address;       /* the last one, through an ADRP-derived base */
};

/* ponytail: linear, not per path. Addresses: a register holds an ADRP-derived value until
 * overwritten, and targets accept one only if found exactly once and named in kallsyms.
 * Shape: offsets from any register an ADRP or ADD writes anywhere in the function are
 * masked, so a cold block after the epilogue still masks its base. */
static void earc_scan(const struct earc_code *fn, u32 width, int store, struct earc_scan *out)
{
    u64 value[32];
    u32 page = 0, sticky = 0, index, byte;  /* page bit r: x<r> holds value[r]; read only then */
    for (index = 0; index < fn->count; index++) {
        u32 w = fn->words[index];
        if ((w & 0x1f000000U) == 0x10000000U
            || ((w & 0xff800000U) == 0x91000000U && (w & 31) != 31 && ((sticky >> ((w >> 5) & 31)) & 1)))
            sticky |= 1U << (w & 31);
    }
    out->shape = 0x811c9dc5U;
    out->found = 0;
    out->address = 0;
    for (index = 0; index < fn->count; index++) {
        u32 w = fn->words[index], rd = w & 31, rn = (w >> 5) & 31;
        u64 pc = fn->address + 4ULL * index;
        int based = rn < 31 && ((page >> rn) & 1);
        if ((w & 0x1f000000U) == 0x10000000U) {  /* ADR, ADRP */
            u64 imm = (u64)((w >> 5) & 0x7ffff) << 2 | ((w >> 29) & 3);
            imm = (imm ^ 0x100000) - 0x100000;
            value[rd] = w >> 31 ? (pc & ~0xfffULL) + (imm << 12) : pc + imm;
            page |= 1U << rd;
            w &= 0x9f00001fU;
        } else if ((w & 0x7c000000U) == 0x14000000U) {  /* B, BL */
            if (w >> 31)
                page &= ~0x4007ffffU;  /* x0-x18, x30 */
            w &= 0xfc000000U;
        } else if ((w & 0xff800000U) == 0x91000000U && based) {  /* ADD Xd, Xn, #imm */
            value[rd] = value[rn] + ((u64)((w >> 10) & 0xfff) << ((w >> 22) & 1 ? 12 : 0));
            page |= 1U << rd;
        } else if ((w & 0x3b000000U) == 0x39000000U && based) {  /* LDR/STR, unsigned offset */
            u32 size = 1U << (w >> 30), load = (w >> 22) & 3;
            if (size == width && !load == !!store && !((w >> 26) & 1)) {
                out->found++;
                out->address = value[rn] + ((w >> 10) & 0xfff) * size;
            }
            if (load)
                page &= ~(1U << rd);
        } else if ((w & 0x3b200000U) == 0x38000000U && based) {  /* unscaled, pre/post-index */
            if ((w >> 22) & 3)
                page &= ~(1U << rd);
        } else if ((w & 0x0a000000U) == 0x08000000U) {  /* other loads and stores */
            if ((w >> 22) & 1)
                page &= ~(1U << rd | ((w & 0x3a000000U) == 0x28000000U ? 1U << ((w >> 10) & 31) : 0));
        } else {
            page &= ~(1U << rd);
        }
        if (rn < 31 && ((sticky >> rn) & 1)) {
            if ((w & 0xff800000U) == 0x91000000U || (w & 0x3b000000U) == 0x39000000U)
                w &= ~(0xfffU << 10);
            else if ((w & 0x3b200000U) == 0x38000000U)
                w &= ~(0x1ffU << 12);
            else if ((w & 0x3a000000U) == 0x28000000U)
                w &= ~(0x7fU << 15);
        }
        for (byte = 0; byte < 4; byte++) {
            out->shape ^= (w >> (8 * byte)) & 0xff;
            out->shape *= 0x01000193U;
        }
    }
}

/* The first function missing or not of the analysed shape, or 0. */
static const char *earc_check(const struct earc_code *fns)
{
    struct earc_scan scan;
    unsigned int index;
    for (index = 0; index < EF_COUNT; index++) {
        if (!fns[index].address)
            return earc_functions[index].name;
        earc_scan(&fns[index], 0, 0, &scan);
        if (scan.shape != earc_functions[index].shape)
            return earc_functions[index].name;
    }
    return 0;
}

#endif
