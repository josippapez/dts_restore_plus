/* SPDX-License-Identifier: GPL-2.0-only */
/* Derives every LG-private data address the eARC route module needs from the
 * code of LG's own functions, so that one module binary can serve every
 * firmware of a kernel generation. Shared, unchanged, by the kernel module
 * (earc_route.c) and the host test (test/test_decode.c).
 *
 * Freestanding: no libc, no floating point, no allocation. All state is
 * static, so it is not reentrant; the module decodes once, in init.
 *
 * Method. Each analysed function is decoded as a whole, [address, address +
 * size), the size coming from the next kallsyms symbol. A forward dataflow
 * pass over its control-flow graph gives every general register an abstract
 * value: a constant, an incoming argument (scaled and offset), the value
 * loaded from another value plus an offset, a call's return value, a few ALU
 * forms, or unknown. Facts are recorded at every visit: loads and stores
 * (address split into a root value plus an offset, and the base register's
 * value), direct and indirect calls with their first arguments, and compares.
 * A merge of differing values is unknown, so every recorded fact holds on at
 * least one real path from the entry. The derivations at the end require
 * exact patterns in those facts and refuse anything else.
 *
 * Soundness assumptions, each enforced: no branch leaves the function except
 * BL (so no cold partition can re-enter it), no indirect branch (no jump
 * table), and only instructions whose register effects are modelled or can
 * be bounded (an unmodelled data-processing instruction forgets its Rd, an
 * unmodelled load/store forgets Rt, Rt2, Rn and Rs). A BL forgets x0, x1,
 * x16, x17 and x30 only: GCC's -fipa-ra lets the caller keep other
 * caller-saved registers live across a call it knows does not clobber them,
 * and compiled code never reads a register the callee did clobber before
 * redefining it. A later call's argument registers are read by that callee,
 * not by an instruction here, so a register only carried across a call is
 * recorded as an unknown argument. A BLR forgets every caller-saved register. */
#ifndef EARC_DECODE_H
#define EARC_DECODE_H

typedef unsigned long long ed_u64;
typedef long long ed_s64;
typedef unsigned int ed_u32;
typedef unsigned short ed_u16;
typedef unsigned char ed_u8;

/* X(name, analysed). Analysed functions need their code and size, the others
 * only their address. mutex_lock and mutex_unlock are exported kernel
 * functions: the decoder needs their addresses to recognise calls to them. */
#define EARC_FUNCTIONS(X) \
    X(audio_hal_earc_set_output_format, 1) \
    X(audio_hal_earc_set_channel, 1) \
    X(audio_hal_earc_set_audio_mode, 1) \
    X(audio_hal_earc_set_mute, 1) \
    X(bm_connect_block, 1) \
    X(bm_disconnect_block, 1) \
    X(bm_get_block_info_by_index, 1) \
    X(_bm_disconnect_block_locked, 1) \
    X(aud_clock_update_output, 1) \
    X(renderer_get_pcm_bypass, 1) \
    X(renderer_get_uid_pcm_bypass_head, 1) \
    X(output_set_earc_on, 1) \
    X(audio_hal_earc_set_on, 0) \
    X(aimc_send_command, 0) \
    X(aud_hal_set_arc_sample_rate, 0) \
    X(_bm_find_block_by_block_info, 0) \
    X(_bm_get_block_port, 0) \
    X(_bm_connect_block_locked, 0) \
    X(mutex_lock, 0) \
    X(mutex_unlock, 0)

#define EARC_ENUM(name, analysed) EF_##name,
enum earc_function { EARC_FUNCTIONS(EARC_ENUM) EF_COUNT };
#undef EARC_ENUM

/* One function: its address and, if analysed, its instruction words. */
struct earc_code {
    ed_u64 address;
    const ed_u32 *words;
    ed_u32 count;
};

/* Everything derived. Offsets are relative to the base register LG's code
 * uses: HAL_A is the base of the NONPCM store, HAL_B that of the rate store. */
struct earc_layout {
    ed_u64 hal_a, hal_b;
    ed_u64 nonpcm_offset, rate_offset, acmod_offset, control_offset;
    ed_u64 nonpcm, cs0, cs2, mode_select, rate, channel_count, acmod, control;
    ed_u64 bm_mutex;
    ed_u64 clock_table, clock_stride, clock_entry3;
    /* aud_clock_update_output: where the entry's mutex, the object's rate,
     * channel count (only if the function takes one), flag and the callbacks
     * (rate, and channel count if taken) are. */
    ed_u64 clock_mutex_offset, clock_takes_channels, clock_object_rate, clock_object_channels;
    ed_u64 clock_object_flag, clock_callback_offset, clock_channel_callback_offset;
};

/* The largest function decoded, in instructions. */
#define ED_MAX_INSNS 512
#define ED_MAX_NODES 512
#define ED_MAX_FACTS 512

/* Abstract values. Node 0 is unknown and node 1 the constant 0, which is also
 * the root of an absolute address. NK_CARRY marks value a as only carried
 * across a call in a caller-saved register: instructions that read the
 * register see a, a later call's argument record sees unknown. */
enum { NK_UNK, NK_AFF, NK_RAW, NK_RET, NK_LOAD, NK_ADDI, NK_OP, NK_CARRY };
enum { OP_ANDI = 1, OP_ORRI, OP_ORR, OP_CSEL };
#define ED_NOARG 0xff
#define ED_UNK 0
#define ED_ZERO 1

struct ed_node {
    ed_u8 kind;
    ed_u8 width;  /* NK_LOAD: bytes, |0x80 if sign-extended. NK_OP: 32 or 64 */
    ed_u8 op;     /* NK_OP */
    ed_u8 arg;    /* NK_AFF, NK_RAW: argument register, or ED_NOARG */
    ed_u16 a, b;  /* operands; NK_LOAD and NK_ADDI: a is the root */
    ed_u64 c;     /* NK_AFF constant, NK_LOAD/NK_ADDI offset, NK_OP immediate, NK_RET call pc */
    ed_u64 m;     /* NK_AFF: multiplier of the zero-extended 32-bit argument */
};

enum { FK_LOAD = 1, FK_STORE, FK_CALL, FK_ICALL, FK_CMPI, FK_CMPR };

struct ed_fact {
    ed_u8 kind;
    ed_u8 width;     /* access bytes; compare width */
    ed_u16 root;     /* access root; compared value; indirect call target */
    ed_u16 value;    /* value loaded or stored; second compared value */
    ed_u16 base;     /* access: the base register's value */
    ed_u16 args[3];  /* call: x0..x2 */
    ed_u64 off;      /* access offset from root; compare immediate */
    ed_u64 pc;
    ed_u64 target;   /* direct call target */
};

static struct ed_node ed_nodes[ED_MAX_NODES];
static unsigned int ed_node_count;
static struct ed_fact ed_facts[ED_MAX_FACTS];
static unsigned int ed_fact_count;
static ed_u16 ed_state[ED_MAX_INSNS][32];
static ed_u8 ed_reached[ED_MAX_INSNS], ed_dirty[ED_MAX_INSNS];
static const struct earc_code *ed_fn;  /* the function being decoded */
/* The reason for a refusal, and the instruction or function it concerns. */
static const char *ed_error;
static ed_u64 ed_error_pc;

static void ed_fail(const char *error, ed_u64 pc) {
    if (!ed_error) { ed_error = error; ed_error_pc = pc; }
}

static ed_u16 ed_node(ed_u8 kind, ed_u8 width, ed_u8 op, ed_u8 arg, ed_u16 a, ed_u16 b, ed_u64 c, ed_u64 m) {
    unsigned int i;
    struct ed_node *n;
    for (i = 1; i < ed_node_count; i++) {
        n = &ed_nodes[i];
        if (n->kind == kind && n->width == width && n->op == op && n->arg == arg && n->a == a &&
            n->b == b && n->c == c && n->m == m)
            return (ed_u16)i;
    }
    if (ed_node_count >= ED_MAX_NODES) {
        ed_fail("value table full", ed_fn->address);
        return ED_UNK;
    }
    n = &ed_nodes[ed_node_count];
    n->kind = kind; n->width = width; n->op = op; n->arg = arg; n->a = a; n->b = b; n->c = c; n->m = m;
    return (ed_u16)ed_node_count++;
}

/* c + m * (the zero-extended low 32 bits of argument register arg). */
static ed_u16 ed_aff(ed_u64 c, ed_u8 arg, ed_u64 m) {
    if (!m || arg == ED_NOARG) { arg = ED_NOARG; m = 0; }
    return ed_node(NK_AFF, 0, 0, arg, 0, 0, c, m);
}

static ed_u16 ed_const(ed_u64 c) { return ed_aff(c, ED_NOARG, 0); }

static int ed_is_const(ed_u16 v, ed_u64 *c) {
    if (ed_nodes[v].kind != NK_AFF || ed_nodes[v].arg != ED_NOARG) return 0;
    *c = ed_nodes[v].c;
    return 1;
}

/* The low 32 bits of argument n. */
static int ed_is_arg(ed_u16 v, unsigned int n) {
    const struct ed_node *x = &ed_nodes[v];
    return (x->kind == NK_RAW && x->arg == n) ||
        (x->kind == NK_AFF && x->arg == n && x->c == 0 && x->m == 1);
}

/* The low 8 bits of argument n, possibly through an AND with 0xff. */
static int ed_is_low8_arg(ed_u16 v, unsigned int n) {
    const struct ed_node *x = &ed_nodes[v];
    return ed_is_arg(v, n) ||
        (x->kind == NK_OP && x->op == OP_ANDI && (x->c & 0xff) == 0xff && ed_is_arg(x->a, n));
}

static ed_u16 ed_add_imm(ed_u16 v, ed_u64 imm) {
    const struct ed_node *x = &ed_nodes[v];
    if (v == ED_UNK) return ED_UNK;
    if (!imm) return v;
    if (x->kind == NK_AFF) return ed_aff(x->c + imm, x->arg, x->m);
    if (x->kind == NK_ADDI) return x->c + imm ? ed_node(NK_ADDI, 0, 0, 0, x->a, 0, x->c + imm, 0) : x->a;
    return ed_node(NK_ADDI, 0, 0, 0, v, 0, imm, 0);
}

/* 64-bit a + b, or a - b. */
static ed_u16 ed_add(ed_u16 a, ed_u16 b, int subtract) {
    const struct ed_node *x = &ed_nodes[a], *y = &ed_nodes[b];
    ed_u64 c;
    if (a == ED_UNK || b == ED_UNK) return ED_UNK;
    if (ed_is_const(b, &c)) return ed_add_imm(a, subtract ? 0 - c : c);
    if (!subtract && ed_is_const(a, &c)) return ed_add_imm(b, c);
    if (x->kind == NK_AFF && y->kind == NK_AFF && (x->arg == y->arg || x->arg == ED_NOARG))
        return subtract ? ed_aff(x->c - y->c, y->arg, x->m - y->m) : ed_aff(x->c + y->c, y->arg, x->m + y->m);
    return ED_UNK;
}

static ed_u16 ed_shl(ed_u16 v, unsigned int s) {
    const struct ed_node *x = &ed_nodes[v];
    if (!s) return v;
    if (s > 63 || x->kind != NK_AFF) return ED_UNK;
    return ed_aff(x->c << s, x->arg, x->m << s);
}

/* What a 32-bit register write leaves in the 64-bit register. */
static ed_u16 ed_zx32(ed_u16 v) {
    const struct ed_node *x = &ed_nodes[v];
    ed_u64 c;
    if (ed_is_const(v, &c)) return ed_const(c & 0xffffffffULL);
    if (x->kind == NK_RAW) return ed_aff(0, x->arg, 1);
    if (x->kind == NK_AFF && x->c == 0 && x->m == 1) return v;
    if (x->kind == NK_LOAD && !(x->width & 0x80) && x->width <= 4) return v;
    if (x->kind == NK_OP && x->width == 32) return v;
    return ED_UNK;
}

static ed_u16 ed_op(ed_u8 op, int w32, ed_u16 a, ed_u16 b, ed_u64 imm) {
    ed_u16 t;
    if (a == ED_UNK || (op >= OP_ORR && b == ED_UNK)) return ED_UNK;
    if (op >= OP_ORR && b < a) { t = a; a = b; b = t; }
    return ed_node(NK_OP, w32 ? 32 : 64, op, 0, a, b, imm, 0);
}

/* Splits base + imm into a root and an offset; ED_UNK if the base is unknown. */
static ed_u16 ed_split(ed_u16 base, ed_u64 imm, ed_u64 *off) {
    const struct ed_node *x = &ed_nodes[base];
    if (base == ED_UNK) return ED_UNK;
    if (x->kind == NK_AFF) {
        *off = x->c + imm;
        return x->arg == ED_NOARG ? ED_ZERO : ed_aff(0, x->arg, x->m);
    }
    if (x->kind == NK_ADDI) {
        *off = x->c + imm;
        return x->a;
    }
    *off = imm;
    return base;
}

static void ed_fact(const struct ed_fact *f) {
    unsigned int i;
    for (i = 0; i < ed_fact_count; i++) {
        const struct ed_fact *g = &ed_facts[i];
        if (g->kind == f->kind && g->width == f->width && g->root == f->root && g->value == f->value &&
            g->base == f->base && g->off == f->off && g->pc == f->pc && g->target == f->target &&
            g->args[0] == f->args[0] && g->args[1] == f->args[1] && g->args[2] == f->args[2])
            return;
    }
    if (ed_fact_count >= ED_MAX_FACTS) {
        ed_fail("fact table full", ed_fn->address);
        return;
    }
    ed_facts[ed_fact_count++] = *f;
}

static void ed_simple_fact(ed_u8 kind, ed_u8 width, ed_u16 root, ed_u16 value, ed_u64 off, ed_u64 pc) {
    struct ed_fact f = {0};
    f.kind = kind; f.width = width; f.root = root; f.value = value; f.off = off; f.pc = pc;
    ed_fact(&f);
}

static ed_s64 ed_sext(ed_u64 value, unsigned int bits) {
    ed_u64 sign = 1ULL << (bits - 1);
    value &= (sign << 1) - 1;
    return (ed_s64)((value ^ sign) - sign);
}

/* DecodeBitMasks for a logical immediate; 0 if the encoding is reserved. */
static int ed_bitmask(unsigned int n, unsigned int immr, unsigned int imms, int sf, ed_u64 *mask) {
    unsigned int combined = (n << 6) | (~imms & 0x3f), len = 6, size, s, r, i;
    ed_u64 element;
    if (!sf && n) return 0;
    while (len && !(combined & (1u << len))) len--;
    if (!len) return 0;
    size = 1u << len;
    s = imms & (size - 1);
    r = immr & (size - 1);
    if (s == size - 1) return 0;
    element = (1ULL << (s + 1)) - 1;
    if (r) element = ((element >> r) | (element << (size - r))) & (size == 64 ? ~0ULL : (1ULL << size) - 1);
    for (i = size; i < 64; i *= 2) element |= element << i;
    *mask = sf ? element : element & 0xffffffffULL;
    return 1;
}

static int ed_inside(ed_u64 target) {
    return target >= ed_fn->address && target < ed_fn->address + 4ULL * ed_fn->count;
}

/* A load or store of width bytes at base + imm through register rt; v holds
 * the values instructions read, r the state written. */
static void ed_access(ed_u16 *r, const ed_u16 *v, int load, int sign, unsigned int width, ed_u16 base,
                      ed_u64 imm, unsigned int rt, ed_u64 pc) {
    struct ed_fact f = {0};
    ed_u64 off = 0;
    ed_u16 root = ed_split(base, imm, &off), value = ED_UNK;
    if (load && root != ED_UNK)
        value = ed_node(NK_LOAD, (ed_u8)(width | (sign ? 0x80 : 0)), 0, 0, root, 0, off, 0);
    if (root != ED_UNK) {
        f.kind = load ? FK_LOAD : FK_STORE;
        f.width = (ed_u8)width;
        f.root = root;
        f.value = load ? value : rt == 31 ? ED_ZERO : v[rt];
        f.base = base;
        f.off = off;
        f.pc = pc;
        ed_fact(&f);
    }
    if (load && rt != 31) r[rt] = value;
}

/* Interprets one instruction on register state r. Sets *target to a branch
 * target inside the function (0 if none) and *fall to whether execution can
 * continue at the next instruction. */
static void ed_step(ed_u32 op, ed_u64 pc, ed_u16 *r, ed_u64 *target, int *fall) {
    unsigned int rd = op & 31, rn = (op >> 5) & 31, rm = (op >> 16) & 31, i;
    int sf = (int)(op >> 31);
    ed_u16 a, b, res = ED_UNK, v[32];
    ed_u64 c, imm, t;
    *target = 0;
    *fall = 1;
    for (i = 0; i < 32; i++) v[i] = ed_nodes[r[i]].kind == NK_CARRY ? ed_nodes[r[i]].a : r[i];

    if ((op >> 16) == 0) { *fall = 0; return; }  /* UDF */

    if ((op & 0x1c000000) == 0x10000000) {  /* data processing, immediate */
        if ((op & 0x1f000000) == 0x10000000) {  /* ADR, ADRP */
            ed_s64 offset = ed_sext(((op >> 5) & 0x7ffff) << 2 | ((op >> 29) & 3), 21);
            res = ed_const(sf ? (pc & ~0xfffULL) + ((ed_u64)offset << 12) : pc + (ed_u64)offset);
        } else if ((op & 0x1f800000) == 0x11000000) {  /* ADD/SUB immediate */
            int sub = (op >> 30) & 1, flags = (op >> 29) & 1;
            imm = (ed_u64)((op >> 10) & 0xfff) << (((op >> 22) & 1) * 12);
            a = rn == 31 ? ED_UNK : v[rn];
            if (sf) res = ed_add_imm(a, sub ? 0 - imm : imm);
            else if (ed_is_const(a, &c)) res = ed_const((sub ? c - imm : c + imm) & 0xffffffffULL);
            if (flags && rd == 31) {
                if (sub) ed_simple_fact(FK_CMPI, sf ? 8 : 4, a, 0, imm, pc);
                return;
            }
        } else if ((op & 0x1f800000) == 0x12000000) {  /* logical immediate */
            unsigned int opc = (op >> 29) & 3;
            a = rn == 31 ? ED_ZERO : v[rn];
            if (ed_bitmask((op >> 22) & 1, (op >> 16) & 0x3f, (op >> 10) & 0x3f, sf, &imm)) {
                if (ed_is_const(a, &c)) {
                    c = opc == 1 ? c | imm : opc == 2 ? c ^ imm : c & imm;
                    res = ed_const(sf ? c : c & 0xffffffffULL);
                } else if (opc == 0 || opc == 3) {
                    res = ed_op(OP_ANDI, !sf, a, 0, imm);
                } else if (opc == 1) {
                    res = ed_op(OP_ORRI, !sf, a, 0, imm);
                }
            }
        } else if ((op & 0x1f800000) == 0x12800000) {  /* move wide */
            unsigned int opc = (op >> 29) & 3, shift = ((op >> 21) & 3) * 16;
            ed_u64 width_mask = sf ? ~0ULL : 0xffffffffULL;
            imm = (ed_u64)((op >> 5) & 0xffff) << shift;
            if (opc != 1 && (sf || shift < 32)) {
                if (opc == 0) res = ed_const(~imm & width_mask);
                else if (opc == 2) res = ed_const(imm);
                else if (rd != 31 && ed_is_const(v[rd], &c))
                    res = ed_const(((c & ~(0xffffULL << shift)) | imm) & width_mask);
            }
        } else if ((op & 0x1f800000) == 0x13000000) {  /* bitfield */
            unsigned int immr = (op >> 16) & 0x3f, imms = (op >> 10) & 0x3f;
            /* UBFM Xd, Xn, #(-s mod 64), #(63-s) is LSL Xd, Xn, #s. */
            if (sf && ((op >> 29) & 3) == 2 && ((op >> 22) & 1) && imms != 63 && imms + 1 == immr)
                res = ed_shl(rn == 31 ? ED_ZERO : v[rn], 63 - imms);
            /* UBFIZ Xd, Xn, #lsb, #32 is UBFM with imms 31 < immr: the zero-
             * extended low word of Xn shifted left by 64 - immr. Other widths
             * stay unknown. */
            else if (sf && ((op >> 29) & 3) == 2 && ((op >> 22) & 1) && imms < immr && imms == 31)
                res = ed_shl(rn == 31 ? ED_ZERO : ed_zx32(v[rn]), 64 - immr);
        }
        if (rd != 31) r[rd] = res;  /* rd 31 is SP or ZR here; SP is never tracked */
        return;
    }

    if ((op & 0x1c000000) == 0x14000000) {  /* branches, exceptions, system */
        if ((op & 0x7c000000) == 0x14000000) {  /* B, BL */
            t = pc + ((ed_u64)ed_sext(op & 0x3ffffff, 26) << 2);
            if (op >> 31) {
                struct ed_fact f = {0};
                f.kind = FK_CALL; f.pc = pc; f.target = t;
                for (i = 0; i < 3; i++) f.args[i] = r[i] == v[i] ? r[i] : ED_UNK;
                ed_fact(&f);
                r[0] = ed_node(NK_RET, 0, 0, 0, 0, 0, pc, 0);
                r[1] = r[16] = r[17] = r[30] = ED_UNK;
                for (i = 2; i <= 18; i++)
                    if (i != 16 && i != 17 && r[i] != ED_UNK && r[i] == v[i]) r[i] = ed_node(NK_CARRY, 0, 0, 0, r[i], 0, 0, 0);
                return;
            }
            if (!ed_inside(t)) ed_fail("branch leaves the function", pc);
            *target = t;
            *fall = 0;
        } else if ((op & 0xff000010) == 0x54000000) {  /* B.cond */
            t = pc + ((ed_u64)ed_sext(op >> 5, 19) << 2);
            if (!ed_inside(t)) ed_fail("conditional branch leaves the function", pc);
            *target = t;
            *fall = (op & 15) < 14;
        } else if ((op & 0x7e000000) == 0x34000000) {  /* CBZ, CBNZ */
            t = pc + ((ed_u64)ed_sext(op >> 5, 19) << 2);
            if (!ed_inside(t)) ed_fail("compare-and-branch leaves the function", pc);
            *target = t;
        } else if ((op & 0x7e000000) == 0x36000000) {  /* TBZ, TBNZ */
            t = pc + ((ed_u64)ed_sext(op >> 5, 14) << 2);
            if (!ed_inside(t)) ed_fail("test-and-branch leaves the function", pc);
            *target = t;
        } else if ((op & 0xfffffc1f) == 0xd65f0000) {  /* RET */
            *fall = 0;
        } else if ((op & 0xfffffc1f) == 0xd63f0000) {  /* BLR */
            struct ed_fact f = {0};
            f.kind = FK_ICALL; f.root = v[rn]; f.pc = pc;
            for (i = 0; i < 3; i++) f.args[i] = r[i] == v[i] ? r[i] : ED_UNK;
            ed_fact(&f);
            for (i = 0; i <= 18; i++) r[i] = ED_UNK;
            r[30] = ED_UNK;
        } else if ((op & 0xffe0001f) == 0xd4200000) {  /* BRK: BUG() */
            *fall = 0;
        } else if ((op & 0xffc00000) == 0xd5000000) {  /* system: only MRS and SYSL write a register */
            if ((op & 0xfff00000) == 0xd5300000 || (op & 0xfff80000) == 0xd5280000) r[rd] = ED_UNK;
        } else {
            ed_fail("indirect branch or unsupported branch/system instruction", pc);
        }
        return;
    }

    if ((op & 0x0a000000) == 0x08000000) {  /* loads and stores */
        unsigned int rt = rd, rt2 = (op >> 10) & 31, simd = (op >> 26) & 1;
        ed_u16 base = rn == 31 ? ED_UNK : v[rn];
        if ((op & 0x3b000000) == 0x39000000) {  /* unsigned immediate */
            unsigned int size = op >> 30, opc = (op >> 22) & 3;
            imm = (ed_u64)((op >> 10) & 0xfff) << size;
            if (simd) return;
            if (opc == 0) ed_access(r, v, 0, 0, 1u << size, base, imm, rt, pc);
            else if (opc == 1) ed_access(r, v, 1, 0, 1u << size, base, imm, rt, pc);
            else if (size < 2 || (size == 2 && opc == 2)) ed_access(r, v, 1, 1, 1u << size, base, imm, rt, pc);
            else if (opc == 3 && rt != 31) r[rt] = ED_UNK;  /* opc 2 size 3 is PRFM */
        } else if ((op & 0x3b200000) == 0x38000000) {  /* unscaled, post-index, pre-index, unprivileged */
            unsigned int size = op >> 30, opc = (op >> 22) & 3, mode = (op >> 10) & 3;
            ed_u64 offset = (ed_u64)ed_sext(op >> 12, 9);
            int known = !simd && mode != 2 && (opc < 2 || (opc == 2 && size < 3) || (opc == 3 && size < 2));
            if (known) ed_access(r, v, opc != 0, opc >= 2, 1u << size, base, mode == 1 ? 0 : offset, rt, pc);
            else if (!simd && rt != 31 && !(opc == 2 && size == 3)) r[rt] = ED_UNK;
            if ((mode == 1 || mode == 3) && rn != 31)
                r[rn] = (!simd && opc != 0 && rt == rn) ? ED_UNK : ed_add_imm(base, offset);
        } else if ((op & 0x3a000000) == 0x28000000) {  /* pair */
            unsigned int opc = op >> 30, mode = (op >> 23) & 3, load = (op >> 22) & 1;
            unsigned int size = simd ? 4u << opc : opc == 2 ? 8 : 4;
            ed_u64 offset = (ed_u64)ed_sext(op >> 15, 7) * size, at = mode == 1 ? 0 : offset;
            if (!simd && (opc == 3 || (opc == 1 && !load))) {
                if (rt != 31) r[rt] = ED_UNK;
                if (rt2 != 31) r[rt2] = ED_UNK;
            } else if (!simd) {
                ed_access(r, v, load, load && opc == 1, size, base, at, rt, pc);
                ed_access(r, v, load, load && opc == 1, size, base, at + size, rt2, pc);
            }
            if ((mode == 1 || mode == 3) && rn != 31)
                r[rn] = (!simd && load && (rt == rn || rt2 == rn)) ? ED_UNK : ed_add_imm(base, offset);
        } else if ((op & 0x3b000000) == 0x18000000) {  /* literal */
            unsigned int opc = op >> 30;
            if (!simd && opc < 3)
                ed_access(r, v, 1, opc == 2, opc == 1 ? 8 : 4, ED_ZERO,
                          pc + ((ed_u64)ed_sext(op >> 5, 19) << 2), rt, pc);
        } else {  /* register offset, atomics, exclusives, acquire/release */
            if (rt != 31) r[rt] = ED_UNK;
            if (rt2 != 31) r[rt2] = ED_UNK;
            if (rn != 31) r[rn] = ED_UNK;
            if (rm != 31) r[rm] = ED_UNK;
        }
        return;
    }

    if ((op & 0x0e000000) == 0x0a000000) {  /* data processing, register */
        a = rn == 31 ? ED_ZERO : v[rn];
        b = rm == 31 ? ED_ZERO : v[rm];
        if ((op & 0x1f000000) == 0x0a000000) {  /* logical, shifted register */
            if (((op >> 29) & 3) == 1 && !((op >> 21) & 1) && !((op >> 10) & 0x3f))  /* ORR, no shift */
                res = rn == 31 ? (sf ? b : ed_zx32(b)) : ed_op(OP_ORR, !sf, a, b, 0);
        } else if ((op & 0x1f200000) == 0x0b000000) {  /* add/sub, shifted register */
            unsigned int shift = (op >> 22) & 3, amount = (op >> 10) & 0x3f;
            int sub = (op >> 30) & 1, flags = (op >> 29) & 1;
            if (flags && rd == 31) {
                if (sub && !shift && !amount) ed_simple_fact(FK_CMPR, sf ? 8 : 4, a, b, 0, pc);
                return;
            }
            if (sf && !shift) res = ed_add(a, ed_shl(b, amount), sub);
        } else if ((op & 0x1f200000) == 0x0b200000) {  /* add/sub, extended register */
            unsigned int option = (op >> 13) & 7, amount = (op >> 10) & 7;
            a = rn == 31 ? ED_UNK : v[rn];
            if (sf && !((op >> 22) & 3) && amount <= 4 && (option == 2 || option == 3 || option == 7))
                res = ed_add(a, ed_shl(option == 2 ? ed_zx32(b) : b, amount), (op >> 30) & 1);
        } else if ((op & 0x1fe00000) == 0x1a800000) {  /* conditional select */
            if (!((op >> 29) & 3) && !((op >> 10) & 3)) res = ed_op(OP_CSEL, !sf, a, b, 0);
        }
        if (rd != 31) r[rd] = res;  /* rd 31: ZR, or SP for extended ADD/SUB; neither tracked */
        return;
    }

    if ((op & 0x0e000000) == 0x0e000000) {  /* SIMD and floating point */
        r[rd] = ED_UNK;  /* a move to a general register writes Rd */
        return;
    }

    ed_fail("unsupported instruction class", pc);
}

/* The value of a register at a join: equal values stay, a value carried
 * across a call on either path stays carried, anything else is unknown. */
static ed_u16 ed_merge(ed_u16 x, ed_u16 y) {
    ed_u16 a = ed_nodes[x].kind == NK_CARRY ? ed_nodes[x].a : x;
    ed_u16 b = ed_nodes[y].kind == NK_CARRY ? ed_nodes[y].a : y;
    if (x == y) return x;
    if (a != b || a == ED_UNK) return ED_UNK;
    return ed_node(NK_CARRY, 0, 0, 0, a, 0, 0, 0);
}

/* Decodes one function and leaves its facts in ed_facts. */
static int ed_analyse(const struct earc_code *fn) {
    unsigned int i, r, pass, changed;
    ed_u16 state[32];
    ed_u64 target;
    int fall;
    ed_fn = fn;
    ed_node_count = 0;
    ed_fact_count = 0;
    if (!fn->words || !fn->count || fn->count > ED_MAX_INSNS || (fn->address & 3)) {
        ed_fail("function size outside the decoder's range", fn->address);
        return -1;
    }
    ed_node(NK_UNK, 0, 0, 0, 0, 0, 0, 0);
    ed_const(0);
    for (i = 0; i < fn->count; i++) ed_reached[i] = ed_dirty[i] = 0;
    for (r = 0; r < 32; r++) ed_state[0][r] = r < 8 ? ed_node(NK_RAW, 0, 0, (ed_u8)r, 0, 0, 0, 0) : ED_UNK;
    ed_reached[0] = ed_dirty[0] = 1;
    for (pass = 0, changed = 1; changed && !ed_error; pass++) {
        if (pass >= 64) { ed_fail("dataflow did not converge", fn->address); break; }
        changed = 0;
        for (i = 0; i < fn->count && !ed_error; i++) {
            unsigned int successor[2], count = 0, s;
            if (!ed_dirty[i]) continue;
            ed_dirty[i] = 0;
            for (r = 0; r < 32; r++) state[r] = ed_state[i][r];
            ed_step(fn->words[i], fn->address + 4ULL * i, state, &target, &fall);
            if (ed_error) break;  /* target may lie outside the function */
            if (fall) {
                if (i + 1 >= fn->count) { ed_fail("execution runs off the end of the function", fn->address + 4ULL * i); break; }
                successor[count++] = i + 1;
            }
            if (target) successor[count++] = (unsigned int)((target - fn->address) / 4);
            for (s = 0; s < count; s++) {
                unsigned int j = successor[s];
                if (!ed_reached[j]) {
                    for (r = 0; r < 32; r++) ed_state[j][r] = state[r];
                    ed_reached[j] = ed_dirty[j] = 1;
                    changed = 1;
                    continue;
                }
                for (r = 0; r < 32; r++) {
                    ed_u16 merged = ed_merge(ed_state[j][r], state[r]);
                    if (merged != ed_state[j][r]) {
                        ed_state[j][r] = merged;
                        ed_dirty[j] = 1;
                        changed = 1;
                    }
                }
            }
        }
    }
    return ed_error ? -1 : 0;
}

/* ---- Queries over the facts of the function just analysed ---- */

static const struct ed_fact *ed_find(ed_u8 kind, ed_u8 width, ed_u16 root, ed_u64 off) {
    unsigned int i;
    for (i = 0; i < ed_fact_count; i++)
        if (ed_facts[i].kind == kind && ed_facts[i].width == width && ed_facts[i].root == root &&
            ed_facts[i].off == off)
            return &ed_facts[i];
    return 0;
}

static ed_u16 ed_loaded(ed_u8 width, ed_u16 root, ed_u64 off) {
    const struct ed_fact *f = root == ED_UNK ? 0 : ed_find(FK_LOAD, width, root, off);
    return f ? f->value : ED_UNK;
}

/* A 32-bit load or store at absolute address whose base register holds base. */
static int ed_word_access_from(ed_u64 address, ed_u64 base) {
    unsigned int i;
    ed_u64 c;
    for (i = 0; i < ed_fact_count; i++) {
        const struct ed_fact *f = &ed_facts[i];
        if ((f->kind == FK_LOAD || f->kind == FK_STORE) && f->width == 4 && f->root == ED_ZERO &&
            f->off == address && ed_is_const(f->base, &c) && c == base)
            return 1;
    }
    return 0;
}

/* The one static address argument n is stored to as a 32-bit word, and the
 * base register value of that store. 0 if none, or more than one. */
static int ed_arg_store(unsigned int n, ed_u64 *address, ed_u64 *base) {
    unsigned int i, found = 0;
    ed_u64 c;
    for (i = 0; i < ed_fact_count; i++) {
        const struct ed_fact *f = &ed_facts[i];
        if (f->kind != FK_STORE || f->width != 4 || f->root != ED_ZERO || !ed_is_arg(f->value, n)) continue;
        if (!ed_is_const(f->base, &c)) return 0;
        if (found && (f->off != *address || c != *base)) return 0;
        *address = f->off;
        *base = c;
        found = 1;
    }
    return found;
}

/* The single call site of target: 1 if exactly one, else 0. */
static int ed_call_site(ed_u64 target, ed_u64 *pc) {
    unsigned int i, found = 0;
    for (i = 0; i < ed_fact_count; i++) {
        if (ed_facts[i].kind != FK_CALL || ed_facts[i].target != target) continue;
        if (found && ed_facts[i].pc != *pc) return 0;
        *pc = ed_facts[i].pc;
        found = 1;
    }
    return found;
}

/* Argument n at the call at pc, if every recorded visit agrees. */
static ed_u16 ed_call_arg(ed_u64 pc, unsigned int n) {
    unsigned int i;
    ed_u16 value = ED_UNK;
    for (i = 0; i < ed_fact_count; i++) {
        if (ed_facts[i].kind != FK_CALL || ed_facts[i].pc != pc) continue;
        if (value != ED_UNK && ed_facts[i].args[n] != value) return ED_UNK;
        value = ed_facts[i].args[n];
    }
    return value;
}

/* A call of target whose argument n is value. An unknown value matches
 * nothing: ed_loaded returns ED_UNK for a missing load, and an argument the
 * callee gets from an unmodelled instruction is ED_UNK too, so the two would
 * otherwise agree about nothing. */
static int ed_any_call_with_arg(ed_u64 target, unsigned int n, ed_u16 value) {
    unsigned int i;
    if (value == ED_UNK) return 0;
    for (i = 0; i < ed_fact_count; i++)
        if (ed_facts[i].kind == FK_CALL && ed_facts[i].target == target && ed_facts[i].args[n] == value)
            return 1;
    return 0;
}

/* A compare of value with imm immediately followed by B.cond (cond, or any
 * condition if cond is negative). */
static int ed_has_compare(ed_u16 value, ed_u64 imm, int cond) {
    unsigned int i;
    for (i = 0; i < ed_fact_count; i++) {
        const struct ed_fact *f = &ed_facts[i];
        ed_u64 next = (f->pc - ed_fn->address) / 4 + 1;
        ed_u32 insn;
        if (f->kind != FK_CMPI || f->root != value || f->off != imm || value == ED_UNK) continue;
        if (next >= ed_fn->count) continue;
        insn = ed_fn->words[next];
        if ((insn & 0xff000010) == 0x54000000 && (cond < 0 || (insn & 15) == (ed_u32)cond)) return 1;
    }
    return 0;
}

static ed_u16 ed_raw(unsigned int n) { return ed_node(NK_RAW, 0, 0, (ed_u8)n, 0, 0, 0, 0); }

#define ED_CHECK(condition, message) \
    do { if (!(condition)) { ed_fail(message, ed_fn->address); return -1; } } while (0)

/* audio_hal_earc_set_output_format(nonpcm, rate): NONPCM is where argument 0
 * is stored and the rate where argument 1 is; their base registers are HAL_A
 * and HAL_B. CS0 and CS2 must be words accessed through the HAL_A base at
 * NONPCM-12 and NONPCM-4, and the mode select a word loaded from HAL_A+0. */
static int ed_output_format(const struct earc_code *fns, struct earc_layout *out) {
    unsigned int i, mode = 0;
    if (ed_analyse(&fns[EF_audio_hal_earc_set_output_format])) return -1;
    ED_CHECK(ed_arg_store(0, &out->nonpcm, &out->hal_a),
             "set_output_format: no single 32-bit store of argument 0 to a static address (NONPCM)");
    ED_CHECK(ed_arg_store(1, &out->rate, &out->hal_b),
             "set_output_format: no single 32-bit store of argument 1 to a static address (rate)");
    out->nonpcm_offset = out->nonpcm - out->hal_a;
    out->rate_offset = out->rate - out->hal_b;
    ED_CHECK(out->nonpcm_offset >= 12 && out->nonpcm_offset < 0x1000 && out->rate_offset < 0x1000 &&
             out->hal_a != out->hal_b, "set_output_format: NONPCM or rate base offset out of range");
    out->cs0 = out->nonpcm - 12;
    out->cs2 = out->nonpcm - 4;
    ED_CHECK(ed_word_access_from(out->cs0, out->hal_a),
             "set_output_format: no 32-bit access to NONPCM-12 through the HAL_A base (CS0)");
    ED_CHECK(ed_word_access_from(out->cs2, out->hal_a),
             "set_output_format: no 32-bit access to NONPCM-4 through the HAL_A base (CS2)");
    for (i = 0; i < ed_fact_count; i++)
        if (ed_facts[i].kind == FK_LOAD && ed_facts[i].width == 4 && ed_facts[i].root == ED_ZERO &&
            ed_facts[i].off == out->hal_a)
            mode = 1;
    ED_CHECK(mode, "set_output_format: no 32-bit load of HAL_A+0 (mode select)");
    out->mode_select = out->hal_a;
    return 0;
}

/* audio_hal_earc_set_channel(channels, acmod): the channel count is stored at
 * HAL_B itself and acmod through the HAL_B base. It reads back the rate and
 * NONPCM words found above. */
static int ed_channel(const struct earc_code *fns, struct earc_layout *out) {
    ed_u64 base;
    if (ed_analyse(&fns[EF_audio_hal_earc_set_channel])) return -1;
    ED_CHECK(ed_arg_store(0, &out->channel_count, &base) && out->channel_count == out->hal_b,
             "set_channel: argument 0 is not stored once, at HAL_B (channel count)");
    ED_CHECK(ed_arg_store(1, &out->acmod, &base) && base == out->hal_b && out->acmod - out->hal_b < 0x1000,
             "set_channel: argument 1 is not stored once through the HAL_B base (acmod)");
    out->acmod_offset = out->acmod - out->hal_b;
    ED_CHECK(ed_find(FK_LOAD, 4, ED_ZERO, out->rate), "set_channel: does not read the rate word");
    ED_CHECK(ed_find(FK_LOAD, 4, ED_ZERO, out->nonpcm), "set_channel: does not read the NONPCM word");
    return 0;
}

/* audio_hal_earc_set_audio_mode(mode): control = (control & ~7) | mode,
 * through the HAL_B base. */
static int ed_audio_mode(const struct earc_code *fns, struct earc_layout *out) {
    unsigned int i, found = 0, other = 0;
    ed_u64 base;
    if (ed_analyse(&fns[EF_audio_hal_earc_set_audio_mode])) return -1;
    for (i = 0; i < ed_fact_count; i++) {
        const struct ed_fact *f = &ed_facts[i];
        const struct ed_node *v = &ed_nodes[f->value], *masked;
        if (f->kind != FK_STORE || f->width != 4 || f->root != ED_ZERO) continue;
        if (v->kind != NK_OP || v->op != OP_ORR || v->width != 32) continue;
        if (ed_is_arg(v->b, 0)) masked = &ed_nodes[v->a];
        else if (ed_is_arg(v->a, 0)) masked = &ed_nodes[v->b];
        else continue;
        if (masked->kind != NK_OP || masked->op != OP_ANDI || masked->width != 32 ||
            masked->c != 0xfffffff8ULL || masked->a != ed_loaded(4, ED_ZERO, f->off))
            continue;
        if (found && f->off != out->control) other = 1;
        if (!ed_is_const(f->base, &base) || base != out->hal_b) other = 1;
        out->control = f->off;
        found = 1;
    }
    ED_CHECK(found && !other, "set_audio_mode: no single (word & ~7) | argument 0 store through the HAL_B base (control)");
    out->control_offset = out->control - out->hal_b;
    ED_CHECK(out->control_offset < 0x1000, "set_audio_mode: control offset out of range");
    return 0;
}

/* audio_hal_earc_set_mute(on): CS2 = on ? CS2 | 1 << 18 : CS2 & ~(1 << 18). */
static int ed_mute(const struct earc_code *fns, const struct earc_layout *out) {
    unsigned int i, found = 0;
    ed_u16 word;
    if (ed_analyse(&fns[EF_audio_hal_earc_set_mute])) return -1;
    word = ed_loaded(4, ED_ZERO, out->cs2);
    for (i = 0; i < ed_fact_count && word != ED_UNK; i++) {
        const struct ed_fact *f = &ed_facts[i];
        const struct ed_node *v = &ed_nodes[f->value], *x, *y;
        if (f->kind != FK_STORE || f->width != 4 || f->root != ED_ZERO || f->off != out->cs2) continue;
        if (v->kind != NK_OP || v->op != OP_CSEL) continue;
        x = &ed_nodes[v->a];
        y = &ed_nodes[v->b];
        if (x->op == OP_ORRI) { const struct ed_node *s = x; x = y; y = s; }
        if (x->kind == NK_OP && x->op == OP_ANDI && x->c == 0xfffbffffULL && x->a == word &&
            y->kind == NK_OP && y->op == OP_ORRI && y->c == 0x40000ULL && y->a == word)
            found = 1;
    }
    ED_CHECK(found, "set_mute: no select of CS2 with bit 18 set or cleared stored back to CS2");
    return 0;
}

/* bm_connect_block / bm_disconnect_block: lock M, call the _locked variant
 * with the arguments unchanged, unlock M. */
static int ed_locked_wrapper(const struct earc_code *fns, enum earc_function wrapper,
                             enum earc_function locked, unsigned int arguments, ed_u64 *mutex) {
    ed_u64 lock = 0, call = 0, unlock = 0, m;
    unsigned int n;
    if (ed_analyse(&fns[wrapper])) return -1;
    ED_CHECK(ed_call_site(fns[EF_mutex_lock].address, &lock) && ed_call_site(fns[locked].address, &call) &&
             ed_call_site(fns[EF_mutex_unlock].address, &unlock) && lock < call && call < unlock,
             "block-manager wrapper: not one mutex_lock, _locked call, mutex_unlock in that order");
    ED_CHECK(ed_is_const(ed_call_arg(lock, 0), &m) && m && ed_call_arg(unlock, 0) == ed_call_arg(lock, 0),
             "block-manager wrapper: mutex_lock and mutex_unlock do not take one static mutex");
    for (n = 0; n < arguments; n++)
        ED_CHECK(ed_is_arg(ed_call_arg(call, n), n), "block-manager wrapper: _locked call does not pass the arguments through");
    ED_CHECK(!*mutex || *mutex == m, "block-manager wrappers lock different mutexes");
    *mutex = m;
    return 0;
}

/* The one offset of a store of width bytes into root whose value satisfies
 * match(value, argument), counted by distinct offset. 0 if none or several. */
static int ed_unique_store(ed_u8 width, ed_u16 root, unsigned int argument, int low8, ed_u64 *off) {
    unsigned int i, found = 0;
    for (i = 0; i < ed_fact_count; i++) {
        const struct ed_fact *f = &ed_facts[i];
        if (f->kind != FK_STORE || f->width != width || f->root != root) continue;
        if (!(low8 ? ed_is_low8_arg(f->value, argument) : ed_is_arg(f->value, argument))) continue;
        if (found && f->off != *off) return 0;
        *off = f->off;
        found = 1;
    }
    return found;
}

/* The one offset X such that a 64-bit load from root at X is the target of an
 * indirect call whose first argument is argument. 0 if none or several. */
static int ed_unique_callback(ed_u16 root, unsigned int argument, ed_u64 *off) {
    unsigned int i, j, found = 0;
    for (i = 0; i < ed_fact_count; i++) {
        const struct ed_fact *f = &ed_facts[i];
        int called = 0;
        if (f->kind != FK_LOAD || f->width != 8 || f->root != root) continue;
        for (j = 0; j < ed_fact_count; j++)
            if (ed_facts[j].kind == FK_ICALL && ed_facts[j].root == f->value && ed_is_arg(ed_facts[j].args[0], argument))
                called = 1;
        if (!called) continue;
        if (found && f->off != *off) return 0;
        *off = f->off;
        found = 1;
    }
    return found;
}

/* aud_clock_update_output(index, rate, [channels,] flag), index <= 3: entry =
 * table + index * stride; reads entry[o] and entry[o+1], locks the entry's
 * mutex, reads the object at entry[o+8], stores rate (and channels) and flag
 * into the object, and calls through the object's +128 control. Nothing is
 * assumed: the stride and the mutex come from the lock's argument, the table
 * from the one pair of byte loads followed by a pointer load, the object
 * offsets from the stores of the arguments, the callbacks from the indirect
 * calls that pass them. */
static int ed_clock(const struct earc_code *fns, struct earc_layout *out) {
    ed_u64 lock = 0, o = 0, flag_arg, channels_off = 0;
    ed_u16 mutex, entry, object, control;
    const struct ed_node *m;
    unsigned int i, pairs = 0;
    if (ed_analyse(&fns[EF_aud_clock_update_output])) return -1;
    ED_CHECK(ed_call_site(fns[EF_mutex_lock].address, &lock), "aud_clock_update_output: not one mutex_lock call");
    mutex = ed_call_arg(lock, 0);
    m = &ed_nodes[mutex];
    ED_CHECK(m->kind == NK_AFF && m->arg == 0 && m->m && m->m <= 0x1000 && !(m->m & 7),
             "aud_clock_update_output: mutex is not table + index * stride + constant");
    ED_CHECK(ed_any_call_with_arg(fns[EF_mutex_unlock].address, 0, mutex),
             "aud_clock_update_output: mutex_unlock does not take the entry mutex");
    out->clock_stride = m->m;
    entry = ed_aff(0, 0, m->m);
    for (i = 0; i < ed_fact_count; i++) {
        const struct ed_fact *f = &ed_facts[i];
        if (f->kind != FK_LOAD || f->width != 1 || f->root != entry) continue;
        if (!ed_find(FK_LOAD, 1, entry, f->off + 1) || !ed_find(FK_LOAD, 8, entry, f->off + 8)) continue;
        if (pairs && f->off != o) { pairs = 2; break; }
        o = f->off;
        pairs = 1;
    }
    ED_CHECK(pairs == 1 && o >= 8,
             "aud_clock_update_output: not exactly one pair of entry+8/entry+9 flag byte loads followed by an 8-byte load at entry+16");
    out->clock_table = o - 8;
    ED_CHECK(m->c >= out->clock_table && m->c - out->clock_table >= 0x18 && !((m->c - out->clock_table) & 7) &&
             m->c - out->clock_table + 0x20 <= m->m,
             "aud_clock_update_output: entry mutex offset is not an aligned 32-byte field inside the entry after +0x18");
    out->clock_mutex_offset = m->c - out->clock_table;
    object = ed_loaded(8, entry, out->clock_table + 16);
    ED_CHECK(object, "aud_clock_update_output: no clock object pointer at entry+16");
    ED_CHECK(ed_has_compare(ed_raw(0), 3, 8) || ed_has_compare(ed_aff(0, 0, 1), 3, 8),
             "aud_clock_update_output: no index <= 3 bound check");
    ED_CHECK(ed_unique_store(4, object, 1, 0, &out->clock_object_rate),
             "aud_clock_update_output: rate (argument 1) not stored once as a word into the object");
    out->clock_takes_channels = ed_unique_store(4, object, 2, 0, &channels_off);
    if (out->clock_takes_channels) out->clock_object_channels = channels_off;
    flag_arg = out->clock_takes_channels ? 3 : 2;
    ED_CHECK(ed_unique_store(1, object, (unsigned int)flag_arg, 1, &out->clock_object_flag),
             "aud_clock_update_output: flag not stored once as a byte into the object");
    ED_CHECK(out->clock_object_rate < 0x80 && out->clock_object_flag < 0x80 &&
             out->clock_object_rate != out->clock_object_channels && out->clock_object_rate != out->clock_object_flag &&
             out->clock_object_channels != out->clock_object_flag,
             "aud_clock_update_output: object field offsets overlap or lie beyond +0x80");
    control = ed_loaded(8, object, 128);
    ED_CHECK(control, "aud_clock_update_output: no control pointer at object+128");
    ED_CHECK(ed_unique_callback(control, 1, &out->clock_callback_offset),
             "aud_clock_update_output: not exactly one call through control+X with the rate as argument 0");
    if (out->clock_takes_channels)
        ED_CHECK(ed_unique_callback(control, 2, &out->clock_channel_callback_offset) &&
                 out->clock_channel_callback_offset != out->clock_callback_offset,
                 "aud_clock_update_output: not exactly one distinct call through control+Y with the channels as argument 0");
    out->clock_entry3 = out->clock_table + 3 * out->clock_stride;
    return 0;
}

/* _bm_disconnect_block_locked(info, port): node = _bm_find_block_by_block_info
 * (info); p = _bm_get_block_port(node, port, 1); walks the list at p+48 whose
 * entries hold the peer port at +16; the peer's owner is at +32, its port id
 * at +40; the owner's public block-info at +88. */
static int ed_disconnect(const struct earc_code *fns) {
    ed_u64 find = 0, get = 0;
    ed_u16 port, first, peer, owner;
    ed_u64 one;
    unsigned int i, empty = 0;
    if (ed_analyse(&fns[EF__bm_disconnect_block_locked])) return -1;
    ED_CHECK(ed_call_site(fns[EF__bm_find_block_by_block_info].address, &find) &&
             ed_is_arg(ed_call_arg(find, 0), 0) && ed_call_site(fns[EF__bm_get_block_port].address, &get) &&
             ed_call_arg(get, 0) == ed_node(NK_RET, 0, 0, 0, 0, 0, find, 0) && ed_is_arg(ed_call_arg(get, 1), 1) &&
             ed_is_const(ed_call_arg(get, 2), &one) && one == 1,
             "_bm_disconnect_block_locked: no _bm_get_block_port(_bm_find_block_by_block_info(info), port, 1)");
    port = ed_node(NK_RET, 0, 0, 0, 0, 0, get, 0);
    first = ed_loaded(8, port, 48);
    for (i = 0; i < ed_fact_count && first; i++)
        if (ed_facts[i].kind == FK_CMPR && ((ed_facts[i].root == ed_add_imm(port, 48) && ed_facts[i].value == first) ||
                                            (ed_facts[i].value == ed_add_imm(port, 48) && ed_facts[i].root == first)))
            empty = 1;
    ED_CHECK(empty, "_bm_disconnect_block_locked: no empty test of the connection list at port+48");
    peer = ed_loaded(8, first, 16);
    owner = ed_loaded(8, peer, 32);
    ED_CHECK(peer && owner && ed_loaded(4, peer, 40) && ed_loaded(8, owner, 88),
             "_bm_disconnect_block_locked: connection entry layout (+16 peer, +32 owner, +40 id, +88 info) differs");
    return 0;
}

/* bm_get_block_info_by_index(type, index) returns the found block's +88. */
static int ed_block_info(const struct earc_code *fns) {
    unsigned int i, found = 0;
    if (ed_analyse(&fns[EF_bm_get_block_info_by_index])) return -1;
    for (i = 0; i < ed_fact_count; i++) {
        const struct ed_fact *f = &ed_facts[i];
        if (f->kind == FK_CALL && ed_is_arg(f->args[0], 0) && ed_is_arg(f->args[1], 1) &&
            ed_loaded(8, ed_node(NK_RET, 0, 0, 0, 0, 0, f->pc, 0), 88))
            found = 1;
    }
    ED_CHECK(found, "bm_get_block_info_by_index: does not read +88 of the block it looks up");
    return 0;
}

/* The renderer's +76 (bypass pipeline) and +77 (PCM bypass) bytes. */
static int ed_renderer(const struct earc_code *fns) {
    if (ed_analyse(&fns[EF_renderer_get_pcm_bypass])) return -1;
    ED_CHECK(ed_loaded(1, ed_raw(0), 77), "renderer_get_pcm_bypass: does not read renderer+77");
    if (ed_analyse(&fns[EF_renderer_get_uid_pcm_bypass_head])) return -1;
    ED_CHECK(ed_loaded(1, ed_raw(0), 76) && ed_loaded(1, ed_raw(0), 77),
             "renderer_get_uid_pcm_bypass_head: does not test renderer+76 and +77");
    return 0;
}

/* output_set_earc_on(output, on): output+44 is compared with 3, on is stored
 * at +129, aud_clock_update_output gets the rate at +76 (the flag comes from
 * +84), then audio_hal_earc_set_on(on). */
static int ed_earc_on(const struct earc_code *fns, const struct earc_layout *layout) {
    const struct ed_fact *on;
    ed_u16 output;
    if (ed_analyse(&fns[EF_output_set_earc_on])) return -1;
    output = ed_raw(0);
    on = ed_find(FK_STORE, 1, output, 129);
    ED_CHECK(ed_has_compare(ed_loaded(4, output, 44), 3, -1),
             "output_set_earc_on: output+44 is not compared with 3");
    ED_CHECK(on && ed_is_low8_arg(on->value, 1), "output_set_earc_on: argument 1 not stored at output+129");
    ED_CHECK(ed_loaded(4, output, 84) &&
             ed_any_call_with_arg(fns[EF_aud_clock_update_output].address, 1, ed_loaded(4, output, 76)),
             "output_set_earc_on: aud_clock_update_output does not get the rate at output+76");
    ED_CHECK(!layout->clock_takes_channels ||
             ed_any_call_with_arg(fns[EF_aud_clock_update_output].address, 2, ed_loaded(4, output, 72)),
             "output_set_earc_on: aud_clock_update_output does not get the channel count at output+72");
    ED_CHECK(ed_any_call_with_arg(fns[EF_audio_hal_earc_set_on].address, 0, on->value),
             "output_set_earc_on: does not call audio_hal_earc_set_on(on)");
    return 0;
}

/* Returns 0 and fills out, or the reason for refusing (ed_error_pc names the
 * instruction or function). */
static const char *earc_decode(const struct earc_code *fns, struct earc_layout *out) {
    unsigned int i;
    ed_error = 0;
    ed_error_pc = 0;
    for (i = 0; i < sizeof(*out) / sizeof(ed_u64); i++) ((ed_u64 *)out)[i] = 0;
    if (ed_output_format(fns, out) || ed_channel(fns, out) || ed_audio_mode(fns, out) || ed_mute(fns, out) ||
        ed_locked_wrapper(fns, EF_bm_connect_block, EF__bm_connect_block_locked, 3, &out->bm_mutex) ||
        ed_locked_wrapper(fns, EF_bm_disconnect_block, EF__bm_disconnect_block_locked, 2, &out->bm_mutex) ||
        ed_clock(fns, out) || ed_disconnect(fns) || ed_block_info(fns) || ed_renderer(fns) || ed_earc_on(fns, out))
        return ed_error ? ed_error : "decoder failed";
    return 0;
}

#endif
