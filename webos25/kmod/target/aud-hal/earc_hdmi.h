/* SPDX-License-Identifier: GPL-2.0-only */
/* Derives code facts for the audit and active session from LG's kernel.
 *
 * Two analyses. _cb_output_status goes through earc_decode.h's dataflow engine,
 * used as it is. _set_output (636 instructions on 5.4, above ED_MAX_INSNS), the
 * ADEC table (its index is a value loaded from a control struct, which the
 * engine's argument-scaled value cannot hold) and the two break-hook functions
 * go through the small tracker below. That tracker is sound per basic block:
 * every register is forgotten at each branch target and after each instruction
 * that does not fall through, any branch leaving the function or indirect
 * branch declines the function, and every instruction class forgets the
 * registers it can write. A register value carries the place it was defined, so
 * two reads are known to see the same value only inside one block.
 *
 * This header derives code facts only. Data reads the audit needs (ADEC 0's and
 * 1's live codec/open/started, the codec-name table, the break-hook list) are done
 * by the caller through the addresses derived here; the list walk is here too,
 * with the memory read passed in, so the module and the test run the same code.
 * Freestanding, static state, decode-once. */
#ifndef EARC_HDMI_H
#define EARC_HDMI_H

#include "earc_decode.h"

/* Loader symbols; analysed functions also need sz_<name>.
 * kretprobe_breakpoint_handler exists on 6.12 only. */
#define EARC_HDMI_FUNCTIONS(X) \
    X(aud_hal_dts_supported, 1) \
    X(_set_output, 1) \
    X(_cb_output_status, 1) \
    X(alsa_decoder_codec, 1) \
    X(alsa_decoder_start, 1) \
    X(input_get_hdmi_codec, 1) \
    X(input_set_hdmi, 1) \
    X(aimc_set_property, 1) \
    X(KDRV_HDMI21_HAL_Rx_GetAudioInfo, 0) \
    X(input_drop_data, 1) \
    X(aimc_register_named_event, 0) \
    X(aimc_unregister_named_event, 0) \
    X(register_kernel_break_hook, 1) \
    X(call_break_hook, 1) \
    X(unregister_kernel_break_hook, 0) \
    X(kprobe_breakpoint_handler, 0) \
    X(kprobe_breakpoint_ss_handler, 0) \
    X(kretprobe_breakpoint_handler, 0) \
    X(aarch64_insn_patch_text, 0) \
    X(register_kprobe, 0) \
    X(unregister_kprobe, 0) \
    X(cpus_read_lock, 0) \
    X(cpus_read_unlock, 0) \
    X(misc_open, 1) \
    X(__fput, 1) \
    X(__vfs_write, 1) \
    X(vfs_write, 1) \
    X(misc_register, 0) \
    X(misc_deregister, 0) \
    X(sndout_earc_output_type, 1) \
    X(sndout_mute_input, 1)

#define EARC_HDMI_ENUM(name, analysed) HF_##name,
enum earc_hdmi_function { EARC_HDMI_FUNCTIONS(EARC_HDMI_ENUM) HF_COUNT };
#undef EARC_HDMI_ENUM

/* The largest function the tracker scans, in instructions (16 KiB of code). */
#define HF_MAX_INSNS 4096

/* The one validity rule for an HDMI-set function as the loader passes it, used
 * by the module and by the test: aligned, inside [stext, etext), and, if it is
 * analysed, a non-zero size that is a multiple of 4, does not pass etext and
 * fits the tracker. The engine's ED_MAX_INSNS is not part of it. */
static int earc_hdmi_valid(ed_u64 address, ed_u64 size, int analysed, ed_u64 stext, ed_u64 etext) {
    if (!address || (address & 3) || address < stext || address >= etext) return 0;
    if (!analysed) return 1;
    return size && !(size & 3) && size <= 4ULL * HF_MAX_INSNS && size <= etext - address;
}

enum { GATE_STOCK, GATE_SWITCHED, GATE_BREAKPOINT, GATE_UNKNOWN };

struct earc_hdmi_layout {
    /* 1. DTS support gate. */
    ed_u64 gate;
    ed_u32 gate_words[2];
    int gate_state;
    /* 3. _set_output: the entry a kprobe would sit on, and the eARC output mask. */
    ed_u64 set_output;
    ed_u32 set_output_first_word;
    int set_output_first_brk;
    ed_u64 mask_offset;
    const char *set_output_reason;
    /* 4. ADEC public-state table: slot = base + index*stride; the three field
     * offsets inside a slot; ADEC 0's (slot 0 = base) and ADEC 1's three
     * absolute addresses. */
    ed_u64 adec_base, adec_stride, adec_open_off, adec_started_off, adec_codec_off;
    ed_u64 adec0_codec, adec0_open, adec0_started;
    ed_u64 adec1_codec, adec1_open, adec1_started;
    const char *adec_reason;
    /* 5. _cb_output_status DTS-HD sites. hd_shape 1: `cmp codec,#bound; b.ls`
     * to the supported-codec store. 2: `cmp codec,#bound; b.ne` falling through
     * into it. hd_brk_*: kprobe breakpoints (brk #4) found in the function.
     * hd_output_reg: the register holding argument 2 (the output) at the
     * compare, 32 if not derived. */
    ed_u64 cb_output_status;
    ed_u64 hd_compare, hd_compare_bound, hd_supported_branch, hd_name_load, name_table;
    ed_u32 hd_compare_word, hd_supported_word, hd_name_word;
    int hd_shape;
    unsigned int hd_output_reg;
    ed_u64 hd_brk_offset;
    unsigned int hd_brk_count;
    const char *cb_reason;
    /* 6. kprobe break handlers: the kernel's break-hook list head, the hook's
     * fn field, the 32-bit word after it (immediate and mask, if the dispatcher
     * shows them), and the handlers by name. */
    ed_u64 break_list_head, break_fn_offset, break_word_offset, break_imm_offset, break_mask_offset;
    int break_fields_known;
    ed_u64 break_fn, step_fn, kret_fn, break_register, break_unregister;
    const char *break_reason;
    /* 2. service symbols, resolved by name. */
    ed_u64 patch_text, kprobe_register, kprobe_unregister, cpus_read_lock, cpus_read_unlock;
    /* 7. cached eARC non-PCM word: virtual (= struct earc_layout.nonpcm) and
     * physical. */
    ed_u64 nonpcm_virtual, nonpcm_physical;
    ed_u64 input_compare;
    ed_u64 input_table, decoder_lock, decoder_lock_call;
    unsigned int input_uid_offset, input_codec_offset;
    const char *format_reason;
    unsigned int input_index_reg;
    const char *input_reason;
    ed_u64 input_set_begin, input_set_end, property_entry;
    ed_u64 input_formats[32];
    unsigned int input_format_count, input_type_offset, input_frame_size, input_forced_reg, input_arg_reg;
    const char *property_reason;
    /* struct file_operations' write, open and release slots, from the kernel's own calls. */
    unsigned int fops_write, fops_open, fops_release;
    const char *fops_reason;
    /* sndout_earc_output_type: the mix flag LG writes, at request + mix_offset. */
    ed_u64 mix_entry;
    unsigned int mix_offset;
    const char *mix_reason;
    ed_u64 mute_entry;
    unsigned int mute_offset;
    const char *mute_reason;
};

/* ---- block-sound register tracker over one function ---- */
enum { HK_UNK, HK_CONST, HK_ARG, HK_LOAD, HK_SCALED, HK_SLOT };
#define HF_BLOCK 0x8000  /* def: live into the block that starts at that instruction */
struct hval {
    ed_u8 kind;
    ed_u8 arg;    /* HK_ARG: argument; HK_LOAD: the argument its base was a copy of, else 0xff; HK_SCALED, HK_SLOT: index register */
    ed_u8 breg;   /* HK_LOAD: base register */
    ed_u16 def;   /* where this value was defined: an instruction index, or HF_BLOCK | index */
    ed_u16 bdef;  /* HK_LOAD: def of the base register; HK_SCALED, HK_SLOT: def of the index register */
    ed_u64 c;     /* HK_CONST: value; HK_LOAD: offset; HK_SLOT: base */
    ed_u64 m;     /* HK_LOAD: width, 0 if sign-extended; HK_SCALED: multiplier; HK_SLOT: stride */
};

static const struct earc_code *hf_fn;
static struct hval hf_reg[32];
static ed_u8 hf_target[HF_MAX_INSNS];

static struct hval hv_unk(void) {
    struct hval v;
    v.kind = HK_UNK; v.arg = 0xff; v.breg = 31; v.def = 0; v.bdef = 0; v.c = 0; v.m = 0;
    return v;
}
static struct hval hv_const(ed_u64 c) { struct hval v = hv_unk(); v.kind = HK_CONST; v.c = c; return v; }
static void hf_set(unsigned int r, struct hval v, unsigned int i) {
    if (r == 31) return;  /* SP or ZR: never tracked */
    v.def = (ed_u16)i;
    hf_reg[r] = v;
}
static ed_u64 hf_pc(unsigned int i) { return hf_fn->address + 4ULL * i; }
static int hf_is_brk(ed_u32 op) { return (op & 0xffe0001f) == 0xd4200000; }
#define HF_KPROBE_BRK 0xd4200080u  /* brk #4, what arm64 kprobes plant */

/* A kprobe can be placed unless the instruction is PC-relative (ADR/ADRP,
 * literal load) or exception-generating, which the arm64 kprobe decoder rejects. */
static int hf_kprobe_placeable(ed_u32 op) {
    if ((op & 0x1f000000) == 0x10000000) return 0;
    if ((op & 0x3b000000) == 0x18000000) return 0;
    if ((op & 0xff000000) == 0xd4000000) return 0;
    return 1;
}

/* Checks that fn can be tracked and marks its branch targets. Returns the
 * reason it cannot: too large, an instruction class or branch form the tracker
 * does not bound, an indirect branch, or a branch that leaves the function (a
 * cold partition could re-enter it at an unmarked place). */
static const char *hf_prepare(const struct earc_code *fn) {
    unsigned int i;
    if (!fn->words || !fn->count) return "no code";
    if (fn->count > HF_MAX_INSNS) return "function larger than the tracker's range";
    for (i = 0; i < fn->count; i++) hf_target[i] = 0;
    for (i = 0; i < fn->count; i++) {
        ed_u32 op = fn->words[i];
        ed_u64 pc = fn->address + 4ULL * i, target;
        if ((op >> 16) == 0) continue;  /* UDF */
        if ((op & 0x1c000000) != 0x14000000) {
            if ((op & 0x1c000000) == 0x10000000 || (op & 0x0a000000) == 0x08000000 || (op & 0x0e000000) == 0x0a000000 ||
                (op & 0x0e000000) == 0x0e000000)
                continue;
            return "unsupported instruction class";
        }
        if ((op & 0xfc000000) == 0x14000000) target = pc + ((ed_u64)ed_sext(op & 0x3ffffff, 26) << 2);  /* B */
        else if ((op & 0xff000010) == 0x54000000 || (op & 0x7e000000) == 0x34000000)                     /* B.cond, CBZ, CBNZ */
            target = pc + ((ed_u64)ed_sext(op >> 5, 19) << 2);
        else if ((op & 0x7e000000) == 0x36000000) target = pc + ((ed_u64)ed_sext(op >> 5, 14) << 2);     /* TBZ, TBNZ */
        else if ((op & 0xfc000000) == 0x94000000 || (op & 0xfffffc1f) == 0xd63f0000 ||                   /* BL, BLR */
                 (op & 0xfffffc1f) == 0xd65f0000 || hf_is_brk(op) || (op & 0xffc00000) == 0xd5000000)    /* RET, BRK, system */
            continue;
        else return "indirect branch or unsupported branch/system instruction";
        if (target < fn->address || target >= fn->address + 4ULL * fn->count) return "a branch leaves the function";
        hf_target[(target - fn->address) / 4] = 1;
    }
    return 0;
}

/* Every register unknown, as live into the block starting at instruction i. */
static void hf_block(unsigned int i) {
    unsigned int r;
    for (r = 0; r < 32; r++) { hf_reg[r] = hv_unk(); hf_reg[r].def = (ed_u16)(HF_BLOCK | i); }
}

/* The entry state. Call only after hf_prepare(fn) returned 0. */
static void hf_enter(const struct earc_code *fn) {
    unsigned int r;
    hf_fn = fn;
    hf_block(0);
    for (r = 0; r < 8; r++) { hf_reg[r].kind = HK_ARG; hf_reg[r].arg = (ed_u8)r; }
}

/* A load of width bytes (0: sign-extended) at [rn + off] into rt. */
static void hf_load(unsigned int rt, unsigned int rn, ed_u64 off, ed_u64 width, unsigned int i) {
    struct hval v = hv_unk();
    v.kind = HK_LOAD;
    v.breg = (ed_u8)rn;
    if (rn != 31) {
        v.arg = hf_reg[rn].kind == HK_ARG ? hf_reg[rn].arg : 0xff;
        v.bdef = hf_reg[rn].def;
    } else {
        v.bdef = 0xffff;  /* SP: never equal to a register's def */
    }
    v.c = off;
    v.m = width;
    hf_set(rt, v, i);
}

/* Applies instruction i to hf_reg and, if the next instruction starts a block,
 * forgets everything. Models a handful of forms; every other instruction
 * forgets each register its class can write. */
static void hf_step(unsigned int i) {
    ed_u32 op = hf_fn->words[i];
    unsigned int rd = op & 31, rn = (op >> 5) & 31, rm = (op >> 16) & 31, r;
    struct hval v = hv_unk();
    int fall = 1;
    if ((op >> 16) == 0) {  /* UDF */
        fall = 0;
    } else if ((op & 0x1c000000) == 0x10000000) {  /* data processing, immediate: writes Rd */
        if ((op & 0x9f000000) == 0x90000000) {  /* ADRP */
            v = hv_const((hf_pc(i) & ~0xfffULL) + ((ed_u64)ed_sext(((op >> 5) & 0x7ffff) << 2 | ((op >> 29) & 3), 21) << 12));
        } else if ((op & 0xff800000) == 0x91000000 && rn != 31 && hf_reg[rn].kind == HK_CONST) {  /* ADD Xd, Xn, #imm */
            v = hv_const(hf_reg[rn].c + ((ed_u64)((op >> 10) & 0xfff) << (((op >> 22) & 1) * 12)));
        } else if ((op & 0xfffffc00) == 0x937f7c00 && rn != 31) {  /* SBFIZ Xd, Xn, #1, #32: index * 2 */
            v.kind = HK_SCALED; v.arg = (ed_u8)rn; v.bdef = hf_reg[rn].def; v.m = 2;
        }
        hf_set(rd, v, i);
    } else if ((op & 0x1c000000) == 0x14000000) {  /* branches, exceptions, system */
        if ((op & 0xfc000000) == 0x94000000 || (op & 0xfffffc1f) == 0xd63f0000) {  /* BL, BLR: caller-saved registers */
            for (r = 0; r <= 18; r++) hf_set(r, v, i);
            hf_set(30, v, i);
        } else if ((op & 0xfc000000) == 0x14000000 || (op & 0xfffffc1f) == 0xd65f0000 || hf_is_brk(op)) {  /* B, RET, BRK */
            fall = 0;
        } else if ((op & 0xff000010) == 0x54000000) {  /* B.cond */
            fall = (op & 15) < 14;
        } else if ((op & 0xfff00000) == 0xd5300000 || (op & 0xfff80000) == 0xd5280000) {  /* MRS, SYSL write Rt */
            hf_set(rd, v, i);
        }  /* CBZ, CBNZ, TBZ, TBNZ and the other system instructions write no general register */
    } else if ((op & 0x0a000000) == 0x08000000) {  /* loads and stores */
        unsigned int rt2 = (op >> 10) & 31, simd = (op >> 26) & 1, size = op >> 30, opc = (op >> 22) & 3;
        if ((op & 0x3b000000) == 0x39000000) {  /* unsigned immediate */
            if (!simd && opc != 0 && !(size == 3 && opc == 2))  /* a load; size 3 opc 2 is PRFM */
                hf_load(rd, rn, (ed_u64)((op >> 10) & 0xfff) << size, opc == 1 ? 1u << size : 0, i);
        } else if ((op & 0x3b200000) == 0x38000000) {  /* unscaled, post-index, pre-index, unprivileged */
            unsigned int mode = (op >> 10) & 3;
            ed_u64 offset = (ed_u64)ed_sext(op >> 12, 9);
            struct hval base = rn == 31 ? hv_unk() : hf_reg[rn];
            if (!simd && opc != 0 && !(size == 3 && opc == 2))
                hf_load(rd, rn, mode == 1 ? 0 : offset, opc == 1 ? 1u << size : 0, i);
            if (mode == 1 || mode == 3) hf_set(rn, base.kind == HK_CONST ? hv_const(base.c + offset) : v, i);  /* writeback */
        } else if ((op & 0x3a000000) == 0x28000000) {  /* pair */
            unsigned int mode = (op >> 23) & 3, width = size == 2 ? 8 : 4;
            ed_u64 offset = (ed_u64)ed_sext(op >> 15, 7) * width, at = mode == 1 ? 0 : offset;
            struct hval base = rn == 31 ? hv_unk() : hf_reg[rn];
            if (!simd && ((op >> 22) & 1)) {  /* load pair; size 1 is LDPSW */
                hf_load(rd, rn, at, size == 1 ? 0 : width, i);
                hf_load(rt2, rn, at + width, size == 1 ? 0 : width, i);
            }
            if (mode == 1 || mode == 3) hf_set(rn, base.kind == HK_CONST ? hv_const(base.c + offset) : v, i);
        } else if ((op & 0x3b000000) == 0x18000000 || (op & 0x3b200c00) == 0x38200800) {  /* literal, register offset */
            if (!simd) hf_set(rd, v, i);
        } else {  /* atomics, exclusives, acquire/release: every register named */
            hf_set(rd, v, i); hf_set(rt2, v, i); hf_set(rn, v, i); hf_set(rm, v, i);
        }
    } else if ((op & 0x0e000000) == 0x0a000000) {  /* data processing, register: writes Rd */
        if ((op & 0xffe0ffe0) == 0xaa0003e0 && rm != 31) {  /* MOV Xd, Xm */
            v = hf_reg[rm];
        } else if ((op & 0xffe0fc00) == 0x8b20c000 && rn != 31 && rm != 31 && hf_reg[rn].kind == HK_SCALED &&
                   hf_reg[rn].arg == rm && hf_reg[rn].bdef == hf_reg[rm].def) {  /* ADD Xd, Xn, Wm, sxtw: + the same index */
            v = hf_reg[rn];
            v.m++;
        } else if ((op & 0xffe0fc00) == 0x8b001400 && rn != 31 && rm != 31 && hf_reg[rn].kind == HK_CONST &&
                   hf_reg[rm].kind == HK_SCALED) {  /* ADD Xd, Xn, Xm, lsl #5: base + index * stride */
            v = hf_reg[rm];
            v.kind = HK_SLOT; v.c = hf_reg[rn].c; v.m = hf_reg[rm].m << 5;
        }
        hf_set(rd, v, i);
    } else {  /* SIMD and floating point: a move to a general register writes Rd */
        hf_set(rd, v, i);
    }
    if (i + 1 < hf_fn->count && (!fall || hf_target[i + 1])) hf_block(i + 1);
}

/* ---- the derivations ---- */

static void hd_gate(const struct earc_code *fns, struct earc_hdmi_layout *out) {
    const struct earc_code *g = &fns[HF_aud_hal_dts_supported];
    out->gate = g->address;
    if (!g->words || g->count < 2) { out->gate_state = GATE_UNKNOWN; return; }
    out->gate_words[0] = g->words[0];
    out->gate_words[1] = g->words[1];
    if (hf_is_brk(g->words[0])) out->gate_state = GATE_BREAKPOINT;
    else if (g->words[0] == 0x52800000u && g->words[1] == 0xd65f03c0u) out->gate_state = GATE_STOCK;
    else if (g->words[0] == 0x52800020u && g->words[1] == 0xd65f03c0u) out->gate_state = GATE_SWITCHED;
    else out->gate_state = GATE_UNKNOWN;
}

/* _set_output(index, descriptor, ...): the mask offset. Exact: in the whole
 * function exactly one 32-bit load whose base is a tracked copy of argument 1
 * has its loaded register compared with an immediate. */
static void hd_set_output(const struct earc_code *fns, struct earc_hdmi_layout *out) {
    const struct earc_code *f = &fns[HF__set_output];
    const char *error;
    unsigned int i, found = 0, several = 0;
    ed_u16 load = 0;
    out->set_output = f->address;
    if (!f->words || !f->count) { out->set_output_reason = "no code"; return; }
    out->set_output_first_word = f->words[0];
    out->set_output_first_brk = hf_is_brk(f->words[0]);
    if (out->set_output_first_brk) { out->set_output_reason = "entry holds a breakpoint (another tool is probing it)"; return; }
    if (!hf_kprobe_placeable(f->words[0])) { out->set_output_reason = "entry is not a kprobe-placeable instruction"; return; }
    error = hf_prepare(f);
    if (error) { out->set_output_reason = error; return; }
    hf_enter(f);
    for (i = 0; i < f->count; i++) {
        ed_u32 op = f->words[i];
        unsigned int rn = (op >> 5) & 31;
        if ((op & 0xff80001f) == 0x7100001f && rn != 31 && hf_reg[rn].kind == HK_LOAD && hf_reg[rn].arg == 1 &&
            hf_reg[rn].m == 4) {  /* CMP Wn, #imm, Wn a word loaded from argument 1 */
            if (found && hf_reg[rn].def != load) several = 1;
            found = 1;
            load = hf_reg[rn].def;
            out->mask_offset = hf_reg[rn].c;
        }
        hf_step(i);
    }
    out->set_output_reason = !found ? "no 32-bit load from argument 1 that is then compared with an immediate (mask offset)" :
        several ? "more than one argument-1 word is compared with an immediate (mask offset ambiguous)" : 0;
}

/* What one function does through the ADEC slot. */
struct hd_slot_scan {
    const char *error;
    unsigned int slots, conflict, bound, first_access;
    ed_u64 base, stride;
    unsigned int index_reg;
    ed_u16 index_def;
    unsigned int stores32, loads8;  /* 32-bit store instructions; distinct byte-load offsets (3: more than two) */
    ed_u64 store32_off, load8_off[2];
};

/* Scans f for the slot idiom `sbfiz xA,xI,#1,#32; add xA,xA,wI,sxtw; add
 * xS,<constant>,xA,lsl #5` (slot = constant + index * 96) and the accesses made
 * while a register still holds that slot. Exact: every such access must be
 * through one slot value (one base, stride and index), and that index must be
 * the value checked by a `cmp wI,#1; b.hi` earlier in the same block. The
 * 5.4.268-329.ptl4tv.2 kernel (33.22.x) compares signed, `b.gt`: it too
 * accepts 0 and 1, the only slots read, and rejects every index above 1. */
static void hd_slot_scan(const struct earc_code *f, struct hd_slot_scan *s) {
    unsigned int i, j, bounds = 0, bound_reg[8], bound_at[8];
    ed_u16 bound_def[8];
    s->slots = s->conflict = s->bound = s->first_access = s->stores32 = s->loads8 = 0;
    s->base = s->stride = s->store32_off = s->load8_off[0] = s->load8_off[1] = 0;
    s->index_reg = 31;
    s->index_def = 0;
    s->error = hf_prepare(f);
    if (s->error) return;
    hf_enter(f);
    for (i = 0; i < f->count; i++) {
        ed_u32 op = f->words[i];
        unsigned int rn = (op >> 5) & 31;
        if ((op & 0xfffffc1f) == 0x7100041f && rn != 31 && i + 1 < f->count &&
            ((f->words[i + 1] & 0xff00001f) == 0x54000008 || (f->words[i + 1] & 0xff00001f) == 0x5400000c) &&
            bounds < 8) {  /* cmp wN,#1 ; b.hi or b.gt */
            bound_reg[bounds] = rn; bound_def[bounds] = hf_reg[rn].def; bound_at[bounds++] = i;
        }
        if ((op & 0x3b000000) == 0x39000000 && !((op >> 26) & 1) && rn != 31 && hf_reg[rn].kind == HK_SLOT) {
            const struct hval *slot = &hf_reg[rn];
            unsigned int size = op >> 30, opc = (op >> 22) & 3;
            ed_u64 off = (ed_u64)((op >> 10) & 0xfff) << size;
            if (!s->slots) {
                s->slots = 1; s->base = slot->c; s->stride = slot->m; s->index_reg = slot->arg; s->index_def = slot->bdef;
                s->first_access = i;
            } else if (s->base != slot->c || s->stride != slot->m || s->index_reg != slot->arg || s->index_def != slot->bdef) {
                s->conflict = 1;
            }
            if (size == 2 && opc == 0) {  /* str wT, [slot, #off] */
                s->stores32++;
                s->store32_off = off;
            } else if (size == 0 && opc == 1) {  /* ldrb wT, [slot, #off] */
                for (j = 0; j < s->loads8 && j < 2; j++) if (s->load8_off[j] == off) break;
                if (j == s->loads8) { if (j < 2) s->load8_off[j] = off; s->loads8++; }
            }
        }
        hf_step(i);
    }
    for (j = 0; j < bounds && s->slots; j++)
        if (bound_reg[j] == s->index_reg && bound_def[j] == s->index_def && bound_at[j] < s->first_access) s->bound = 1;
}

/* The ADEC public-state table, from alsa_decoder_codec (base, stride, codec =
 * the one 32-bit store through the slot, open = the one byte offset loaded
 * through it) and alsa_decoder_start (same base and stride; exactly two byte
 * offsets loaded through the slot, open and started). */
static void hd_adec(const struct earc_code *fns, struct earc_hdmi_layout *out) {
    struct hd_slot_scan codec, start;
    ed_u64 slot1;
    hd_slot_scan(&fns[HF_alsa_decoder_codec], &codec);
    if (codec.error) { out->adec_reason = codec.error; return; }
    if (!codec.slots) { out->adec_reason = "slot-index idiom (constant + index*stride: sbfiz, add sxtw, add lsl #5) not found in alsa_decoder_codec"; return; }
    if (codec.conflict) { out->adec_reason = "more than one slot value in alsa_decoder_codec"; return; }
    if (!codec.bound) { out->adec_reason = "no decoder-index <= 1 bound check on the slot index in alsa_decoder_codec"; return; }
    if (!codec.stores32) { out->adec_reason = "codec store (a 32-bit store through the slot) not found"; return; }
    if (codec.stores32 > 1) { out->adec_reason = "more than one 32-bit store through the slot (codec ambiguous)"; return; }
    if (codec.loads8 != 1) { out->adec_reason = "not exactly one byte offset loaded through the slot in alsa_decoder_codec (open)"; return; }
    hd_slot_scan(&fns[HF_alsa_decoder_start], &start);
    if (start.error) { out->adec_reason = start.error; return; }
    if (!start.slots) { out->adec_reason = "slot-index idiom (constant + index*stride: sbfiz, add sxtw, add lsl #5) not found in alsa_decoder_start"; return; }
    if (start.conflict) { out->adec_reason = "more than one slot value in alsa_decoder_start"; return; }
    if (!start.bound) { out->adec_reason = "no decoder-index <= 1 bound check on the slot index in alsa_decoder_start"; return; }
    if (start.base != codec.base || start.stride != codec.stride) {
        out->adec_reason = "alsa_decoder_start and alsa_decoder_codec index different tables"; return;
    }
    if (start.loads8 != 2 || (start.load8_off[0] != codec.load8_off[0] && start.load8_off[1] != codec.load8_off[0])) {
        out->adec_reason = "alsa_decoder_start does not load exactly two slot bytes, open and one other (started)"; return;
    }
    out->adec_base = codec.base;
    out->adec_stride = codec.stride;
    out->adec_codec_off = codec.store32_off;
    out->adec_open_off = codec.load8_off[0];
    out->adec_started_off = start.load8_off[0] != codec.load8_off[0] ? start.load8_off[0] : start.load8_off[1];
    if (out->adec_codec_off + 4 > codec.stride || out->adec_open_off >= codec.stride || out->adec_started_off >= codec.stride) {
        out->adec_reason = "a field offset lies outside the slot"; return;
    }
    out->adec0_codec = codec.base + out->adec_codec_off;
    out->adec0_open = codec.base + out->adec_open_off;
    out->adec0_started = codec.base + out->adec_started_off;
    slot1 = codec.base + codec.stride;
    out->adec1_codec = slot1 + out->adec_codec_off;
    out->adec1_open = slot1 + out->adec_open_off;
    out->adec1_started = slot1 + out->adec_started_off;
    out->adec_reason = 0;
}

/* The value every visit of the engine recorded for a 32-bit CMP immediate at
 * pc, and its immediate; ED_UNK if none or the visits differ. */
static ed_u16 hd_compared_at(ed_u64 pc, ed_u64 *imm) {
    unsigned int i, seen = 0;
    ed_u16 value = ED_UNK;
    for (i = 0; i < ed_fact_count; i++) {
        const struct ed_fact *f = &ed_facts[i];
        if (f->kind != FK_CMPI || f->pc != pc) continue;
        if (f->width != 4 || (seen && f->root != value)) return ED_UNK;
        value = f->root;
        *imm = f->off;
        seen = 1;
    }
    return value;
}

/* Is the instruction at pc, on every visit, a 32-bit store of value? */
static int hd_stores_at(ed_u64 pc, ed_u16 value) {
    unsigned int i, seen = 0;
    for (i = 0; i < ed_fact_count; i++) {
        const struct ed_fact *f = &ed_facts[i];
        if (f->kind != FK_STORE || f->pc != pc) continue;
        if (f->width != 4 || f->value != value) return 0;
        seen = 1;
    }
    return seen;
}

/* _cb_output_status(event, ?, output): the DTS-HD sites, through the engine.
 * The event codec is the 32-bit word loaded from argument 0 at +0. Exact:
 * exactly one compare of it with an immediate is either followed by `b.ls` to
 * a 32-bit store of the codec (shape 1) or by `b.ne` with that store as the
 * next instruction (shape 2); and exactly one `ldr x,[constant, codec, lsl #3]`
 * is the name load. A kprobe breakpoint anywhere in the function is reported
 * instead, before any pattern is judged. */
static void hd_cb(const struct earc_code *fns, struct earc_hdmi_layout *out) {
    const struct earc_code *f = &fns[HF__cb_output_status];
    unsigned int i, compares = 0, names = 0;
    ed_u16 codec;
    out->cb_output_status = f->address;
    if (!f->words || !f->count) { out->cb_reason = "no code"; return; }
    for (i = 0; i < f->count; i++)
        if (f->words[i] == HF_KPROBE_BRK && !out->hd_brk_count++) out->hd_brk_offset = 4ULL * i;
    if (out->hd_brk_count) { out->cb_reason = "holds a breakpoint (another tool is probing it)"; return; }
    ed_error = 0;
    ed_error_pc = 0;
    if (ed_analyse(f)) { out->cb_reason = ed_error ? ed_error : "decoder failed"; return; }
    codec = ed_loaded(4, ed_raw(0), 0);
    if (codec == ED_UNK) { out->cb_reason = "event codec is not a 32-bit load from argument 0"; return; }
    for (i = 0; i + 2 < f->count; i++) {
        ed_u64 pc = f->address + 4ULL * i, imm = 0, target, supported;
        ed_u32 branch = f->words[i + 1];
        int shape;
        if (!ed_reached[i] || (branch & 0xff000010) != 0x54000000 || hd_compared_at(pc, &imm) != codec) continue;
        target = pc + 4 + ((ed_u64)ed_sext(branch >> 5, 19) << 2);
        if ((branch & 15) == 9 && hd_stores_at(target, codec)) { shape = 1; supported = target; }      /* b.ls */
        else if ((branch & 15) == 1 && hd_stores_at(pc + 8, codec)) { shape = 2; supported = pc + 8; }  /* b.ne */
        else continue;
        compares++;
        out->hd_shape = shape;
        out->hd_compare = pc;
        out->hd_compare_word = f->words[i];
        out->hd_compare_bound = imm;
        out->hd_supported_branch = supported;
        out->hd_supported_word = f->words[(supported - f->address) / 4];
        /* The supported store writes the codec through the output: its base
         * register is the output register if it holds argument 2 here too. */
        if (ed_state[i][(out->hd_supported_word >> 5) & 31] == ed_raw(2)) out->hd_output_reg = (out->hd_supported_word >> 5) & 31;
    }
    if (compares != 1) {
        out->cb_reason = compares ? "more than one event-codec acceptance compare" :
            "no event-codec acceptance compare (cmp codec,#bound; b.ls to, or b.ne falling into, the codec store)";
        return;
    }
    for (i = 0; i < f->count; i++) {
        ed_u32 op = f->words[i];
        unsigned int rn = (op >> 5) & 31, rm = (op >> 16) & 31;
        ed_u16 base, index;
        ed_u64 table;
        if (!ed_reached[i] || (op & 0xffe0fc00) != 0xf8607800 || rn == 31 || rm == 31) continue;  /* ldr xT,[xN,xM,lsl #3] */
        base = ed_state[i][rn];
        index = ed_state[i][rm];
        if (ed_nodes[base].kind == NK_CARRY) base = ed_nodes[base].a;
        if (ed_nodes[index].kind == NK_CARRY) index = ed_nodes[index].a;
        if (!ed_is_const(base, &table) || index != codec) continue;
        names++;
        out->hd_name_load = f->address + 4ULL * i;
        out->hd_name_word = op;
        out->name_table = table;
    }
    out->cb_reason = names == 1 ? 0 : names ? "more than one codec-name table load" :
        "codec-name table load (ldr x,[constant, codec, lsl #3]) not found";
}

/* The kernel's break-hook list and struct break_hook's fields. Exact:
 * register_kernel_break_hook has exactly one pre-indexed 64-bit load from a
 * constant base (the list head, as list_add_rcu reads head->next), and
 * call_break_hook, which must compute that same head, has exactly one BLR,
 * exactly one `ldr xN,[xN]` (the node advance) and exactly one 64-bit load into
 * the BLR's register, from the node (fn). The immediate and mask are derived
 * only if the node has exactly two 16-bit loads and the one `cmp x, w, uxth`
 * compares one of them: that one is the immediate, the other the mask. */
static void hd_break(const struct earc_code *fns, struct earc_hdmi_layout *out) {
    const struct earc_code *reg = &fns[HF_register_kernel_break_hook], *call = &fns[HF_call_break_hook];
    const char *error;
    unsigned int i, heads = 0, blrs = 0, advances = 0, fn_loads = 0, halves = 0, compares = 0, head_seen = 0;
    unsigned int fn_reg = 31, node = 31, fn_base = 31, cmp_base = 31;
    ed_u16 fn_bdef = 0, half_bdef[2] = {0, 0}, cmp_bdef = 0;
    ed_u64 fn_off = 0, half_off[2] = {0, 0}, cmp_off = 0;
    out->break_register = reg->address;
    out->break_unregister = fns[HF_unregister_kernel_break_hook].address;
    out->break_fn = fns[HF_kprobe_breakpoint_handler].address;
    out->step_fn = fns[HF_kprobe_breakpoint_ss_handler].address;
    out->kret_fn = fns[HF_kretprobe_breakpoint_handler].address;
    if (!out->break_fn || !out->step_fn) { out->break_reason = "kprobe breakpoint handlers not found by name"; return; }
    error = hf_prepare(reg);
    if (error) { out->break_reason = error; return; }
    hf_enter(reg);
    for (i = 0; i < reg->count; i++) {
        ed_u32 op = reg->words[i];
        unsigned int rn = (op >> 5) & 31;
        if ((op & 0xffe00c00) == 0xf8400c00 && rn != 31 && hf_reg[rn].kind == HK_CONST) {  /* ldr xT,[xN,#imm]! */
            heads++;
            out->break_list_head = hf_reg[rn].c + (ed_u64)ed_sext(op >> 12, 9);
        }
        hf_step(i);
    }
    if (heads != 1) {
        out->break_list_head = 0;
        out->break_reason = "not exactly one list-head load (pre-indexed, constant base) in register_kernel_break_hook";
        return;
    }
    error = hf_prepare(call);
    if (error) { out->break_reason = error; return; }
    for (i = 0; i < call->count; i++) {  /* the BLR's register and the node register first */
        ed_u32 op = call->words[i];
        if ((op & 0xfffffc1f) == 0xd63f0000) { blrs++; fn_reg = (op >> 5) & 31; }
        if ((op & 0xfffffc00) == 0xf9400000 && (op & 31) == ((op >> 5) & 31)) { advances++; node = op & 31; }  /* ldr xN,[xN] */
    }
    if (blrs != 1 || advances != 1 || node == 31 || fn_reg == 31 || fn_reg == node) {
        out->break_reason = "call_break_hook: not exactly one BLR and one node advance (ldr xN,[xN])"; return;
    }
    hf_enter(call);
    for (i = 0; i < call->count; i++) {
        ed_u32 op = call->words[i];
        unsigned int rd = op & 31, rn = (op >> 5) & 31;
        if ((op & 0xffc00000) == 0xf9400000 && rd == fn_reg && rd != rn) {  /* ldr xF,[xB,#off] */
            fn_loads++; fn_base = rn; fn_bdef = rn == 31 ? 0xffff : hf_reg[rn].def; fn_off = (ed_u64)((op >> 10) & 0xfff) << 3;
        }
        if ((op & 0xffc00000) == 0x79400000 && rn == node) {  /* ldrh wT,[node,#off] */
            if (halves < 2) { half_bdef[halves] = hf_reg[rn].def; half_off[halves] = (ed_u64)((op >> 10) & 0xfff) << 1; }
            halves++;
        }
        if ((op & 0x7fe0fc1f) == 0x6b20201f) {  /* cmp Rn, Wm, uxth */
            compares++;
            if (rn != 31 && hf_reg[rn].kind == HK_LOAD && hf_reg[rn].m == 2) {
                cmp_base = hf_reg[rn].breg; cmp_bdef = hf_reg[rn].bdef; cmp_off = hf_reg[rn].c;
            }
        }
        hf_step(i);
        if (rd != 31 && hf_reg[rd].kind == HK_CONST && hf_reg[rd].c == out->break_list_head) head_seen = 1;
    }
    if (!head_seen) { out->break_reason = "call_break_hook does not use the list head register_kernel_break_hook adds to"; return; }
    if (fn_loads != 1 || fn_base != node || !fn_off || (fn_off & 7)) {
        out->break_reason = "call_break_hook: not exactly one 64-bit load from the node into the BLR's register (fn)"; return;
    }
    out->break_fn_offset = fn_off;
    out->break_word_offset = fn_off + 8;
    if (halves == 2 && compares == 1 && half_off[0] != half_off[1] && half_bdef[0] == fn_bdef && half_bdef[1] == fn_bdef &&
        cmp_base == node && cmp_bdef == fn_bdef && (cmp_off == half_off[0] || cmp_off == half_off[1])) {
        out->break_fields_known = 1;
        out->break_imm_offset = cmp_off;
        out->break_mask_offset = cmp_off == half_off[0] ? half_off[1] : half_off[0];
    }
    out->break_reason = 0;
}

/* May op write general register r? Unknown forms answer yes, which only refuses. */
static int hf_writes(ed_u32 op, unsigned int r) {
    unsigned int rt = op & 31, rn = (op >> 5) & 31, rt2 = (op >> 10) & 31, mode;
    if ((op & 0x1c000000u) == 0x14000000u) {  /* branches, exceptions, system */
        if ((op & 0xfc000000u) == 0x94000000u || (op & 0xfffffc1fu) == 0xd63f0000u) return r <= 18 || r == 30;
        if ((op & 0xfff00000u) == 0xd5300000u || (op & 0xfff80000u) == 0xd5280000u) return rt == r;
        return 0;
    }
    if ((op & 0x0a000000u) == 0x08000000u) {  /* loads and stores */
        if ((op & 0x3a000000u) == 0x28000000u) {  /* pair */
            mode = (op >> 23) & 3;
            return (((op >> 22) & 1) && (r == rt || r == rt2)) || ((mode == 1 || mode == 3) && r == rn);
        }
        if ((op & 0x3b000000u) == 0x39000000u) return ((op >> 22) & 3) && r == rt;  /* unsigned offset */
        if ((op & 0x3b200000u) == 0x38000000u) {  /* unscaled, post-index, unprivileged, pre-index */
            mode = (op >> 10) & 3;
            return (((op >> 22) & 3) && r == rt) || ((mode == 1 || mode == 3) && r == rn);
        }
        if ((op & 0x3b200c00u) == 0x38200800u) return ((op >> 22) & 3) && r == rt;  /* register offset */
        if ((op & 0x3b000000u) == 0x18000000u) return r == rt;                        /* literal */
        return r == rt || r == rn || r == rt2 || r == ((op >> 16) & 31);
    }
    return rt == r;  /* data processing and SIMD: Rd */
}

/* A value as constant + sum of coef[i] * register reg[i]. */
struct hf_linear {
    unsigned int count, reg[4];
    ed_u64 coef[4], constant;
};
static int hf_linear_add(struct hf_linear *v, unsigned int reg, ed_u64 coef) {
    unsigned int i;
    if (reg == 31) return 0;
    for (i = 0; i < v->count; i++)
        if (v->reg[i] == reg) { v->coef[i] += coef; return 1; }
    if (v->count == 4) return 0;
    v->reg[v->count] = reg;
    v->coef[v->count++] = coef;
    return 1;
}

/* Register reg just before instruction at, in terms of the registers at
 * instruction stop, substituted back through ADD (immediate or LSL-shifted
 * register), MOV and LSL. Returns 0 for any other definition. */
static int hf_linear_at(const struct earc_code *fn, unsigned int at, unsigned int reg, unsigned int stop,
                        struct hf_linear *v) {
    unsigned int i, t, hit, rd, rn, rm;
    v->count = 0;
    v->constant = 0;
    if (!hf_linear_add(v, reg, 1)) return 0;
    for (i = at; i-- > stop;) {
        ed_u32 op = fn->words[i];
        ed_u64 k;
        for (t = 0, hit = 4; t < v->count; t++)
            if (hf_writes(op, v->reg[t])) { if (hit != 4) return 0; hit = t; }
        if (hit == 4) continue;
        k = v->coef[hit];
        rd = op & 31; rn = (op >> 5) & 31; rm = (op >> 16) & 31;
        if (rd != v->reg[hit]) return 0;
        v->reg[hit] = v->reg[--v->count];
        v->coef[hit] = v->coef[v->count];
        if ((op & 0xff800000u) == 0x91000000u) {  /* ADD Xd, Xn, #imm{, lsl #12} */
            if (!hf_linear_add(v, rn, k)) return 0;
            v->constant += k * ((ed_u64)((op >> 10) & 0xfff) << (((op >> 22) & 1) * 12));
        } else if ((op & 0xffe0ffe0u) == 0xaa0003e0u) {  /* MOV Xd, Xm */
            if (!hf_linear_add(v, rm, k)) return 0;
        } else if ((op & 0xffe00000u) == 0x8b000000u) {  /* ADD Xd, Xn, Xm, lsl #s */
            if (!hf_linear_add(v, rn, k) || !hf_linear_add(v, rm, k << ((op >> 10) & 63))) return 0;
        } else if ((op & 0xffc00000u) == 0xd3400000u && ((op >> 10) & 63) != 63 &&
                   ((((op >> 10) & 63) + 1) & 63) == ((op >> 16) & 63)) {  /* LSL Xd, Xn, #s */
            if (!hf_linear_add(v, rn, k << (63 - ((op >> 10) & 63)))) return 0;
        } else {
            return 0;
        }
    }
    return 1;
}

/* The address of the 64-bit load at instruction at, from the start of its block. */
static int hf_load_address(const struct earc_code *fn, unsigned int at, struct hf_linear *v) {
    ed_u32 op = fn->words[at];
    unsigned int stop = at, rn = (op >> 5) & 31, i;
    struct hf_linear index;
    while (stop > 0 && !hf_target[stop]) stop--;
    if ((op & 0xffc00000u) == 0xf9400000u) {  /* LDR Xt, [Xn, #imm] */
        if (!hf_linear_at(fn, at, rn, stop, v)) return 0;
        v->constant += ((op >> 10) & 0xfff) * 8;
        return 1;
    }
    if ((op & 0xffe0fc00u) == 0xf8606800u) {  /* LDR Xt, [Xn, Xm] */
        if (!hf_linear_at(fn, at, rn, stop, v) || !hf_linear_at(fn, at, (op >> 16) & 31, stop, &index)) return 0;
        for (i = 0; i < index.count; i++)
            if (!hf_linear_add(v, index.reg[i], index.coef[i])) return 0;
        v->constant += index.constant;
        return 1;
    }
    return 0;
}

/* A 16-byte-entry table lookup: base register + 16 * index register + offset. */
static int hf_table_lookup(const struct hf_linear *v, unsigned int *base, unsigned int *index) {
    if (v->count != 2 || v->constant >= 0x1000 || (v->constant & 7)) return 0;
    if (v->coef[0] == 1 && v->coef[1] == 16) { *base = v->reg[0]; *index = v->reg[1]; return 1; }
    if (v->coef[1] == 1 && v->coef[0] == 16) { *base = v->reg[1]; *index = v->reg[0]; return 1; }
    return 0;
}

/* alsa_decoder_start calls input_get_hdmi_codec(table[decoder].input) and
 * compares the result with 1. The table address is derived from the data flow
 * into that call, whatever instructions compute it, and must match every other
 * 16-byte-stride load the function makes through the same registers. */
static void hd_input_start(const struct earc_code *fns, struct earc_hdmi_layout *out) {
    const struct earc_code *start = &fns[HF_alsa_decoder_start];
    const struct earc_code *get = &fns[HF_input_get_hdmi_codec];
    unsigned int index, found = 0, table_reg = 31, tables = 0, uses = 0;
    ed_u64 table_offset = 0;
    if (!get->words || get->count < 2 || (get->words[0] & 0xffc003ffu) != 0xb9400000u ||
        get->words[1] != 0xd65f03c0u) {
        out->input_reason = "HDMI codec getter is not a word load followed by ret";
        return;
    }
    out->input_reason = hf_prepare(start);
    if (out->input_reason) return;
    for (index = 1; index + 2 < start->count; index++) {
        ed_u32 call = start->words[index], cset = start->words[index + 2];
        ed_u64 target = start->address + 4ULL * index + ((ed_u64)ed_sext(call & 0x3ffffff, 26) << 2);
        struct hf_linear argument;
        unsigned int writer = index, base, decoder;
        if ((call & 0xfc000000u) != 0x94000000u || target != get->address) continue;
        if (start->words[index + 1] != 0x7100041fu || (cset & 0xffffffe0u) != 0x1a9f17e0u) continue;
        while (writer > 0 && !hf_target[writer] && !hf_writes(start->words[writer - 1], 0)) writer--;
        if (writer == 0 || hf_target[writer]) continue;
        if (!hf_load_address(start, writer - 1, &argument) ||
            !hf_table_lookup(&argument, &base, &decoder) || decoder < 19 || decoder > 28) continue;
        out->input_index_reg = decoder;
        table_reg = base;
        table_offset = argument.constant;
        out->input_compare = start->address + 4ULL * (index + 1);
        found++;
    }
    out->input_reason = found == 1 ? 0 : found ? "HDMI start compare is ambiguous" : "HDMI start compare not found";
    if (out->input_reason) return;
    for (index = 0; index < start->count; index++) {
        struct hf_linear address;
        unsigned int base, decoder;
        if (!hf_load_address(start, index, &address) || !hf_table_lookup(&address, &base, &decoder) ||
            base != table_reg || decoder != out->input_index_reg) continue;
        if (address.constant != table_offset) {
            out->input_reason = "HDMI input table offsets disagree";
            return;
        }
        uses++;
    }
    if (uses < 2) { out->input_reason = "HDMI input table used only by the start compare"; return; }
    out->input_codec_offset = ((get->words[0] >> 10) & 0xfff) * 4;
    {
        /* The first BL with x0 = xB + imm, where xB = adrp + add earlier in the
         * function: straight-line constants a branch target cannot clobber. */
        ed_u64 page[32], constant[32];
        for (index = 0; index < 32; index++) ((volatile ed_u64 *)page)[index] = ((volatile ed_u64 *)constant)[index] = 0;
        for (index = 0; index + 1 < start->count && !out->decoder_lock; index++) {
            ed_u32 op = start->words[index], next = start->words[index + 1];
            unsigned int rd = op & 31, rn = (op >> 5) & 31;
            if ((op & 0x9f000000u) == 0x90000000u)
                page[rd] = (start->address + 4ULL * index) & ~0xfffULL,
                page[rd] += (ed_u64)ed_sext(((op >> 5) & 0x7ffff) << 2 | ((op >> 29) & 3), 21) << 12;
            else if ((op & 0xffc00000u) == 0x91000000u && rd != 31 && rn != 31 && page[rn] && rd != rn)
                constant[rd] = page[rn] + ((op >> 10) & 0xfff);
            else if ((op & 0xffc0001fu) == 0x91000000u && rn != 31 && constant[rn]) {
                unsigned int at;
                for (at = index + 1; at < start->count && at <= index + 4; at++) {
                    next = start->words[at];
                    if ((next & 0xfc000000u) == 0x94000000u) {
                        out->decoder_lock = constant[rn] + ((op >> 10) & 0xfff);
                        out->decoder_lock_call = start->address + 4ULL * at + ((ed_u64)ed_sext(next & 0x3ffffff, 26) << 2);
                        break;
                    }
                    if (hf_writes(next, 0)) break;
                }
            }
        }
    }
    hf_enter(start);
    for (index = 0; index < start->count; index++) {
        struct hval previous = hf_reg[table_reg];
        hf_step(index);
        /* The table base sits in the driver struct that also holds the ADEC slots. */
        if (hf_reg[table_reg].kind == HK_CONST && hf_reg[table_reg].c != previous.c &&
            hf_reg[table_reg].c <= out->adec_base && out->adec_base - hf_reg[table_reg].c <= 0x100 &&
            hf_reg[table_reg].c + table_offset != out->input_table) {
            out->input_table = hf_reg[table_reg].c + table_offset;
            tables++;
        }
    }
    out->input_reason = tables == 1 ? 0 : "HDMI input table is ambiguous";
}

static void hd_input_properties(const struct earc_code *fns, struct earc_hdmi_layout *out) {
    const struct earc_code *input = &fns[HF_input_set_hdmi];
    const struct earc_code *property = &fns[HF_aimc_set_property];
    ed_u64 info = fns[HF_KDRV_HDMI21_HAL_Rx_GetAudioInfo].address;
    unsigned int index, frames = 0, infos = 0, forced = 0, inputs = 0;
    out->property_reason = "HDMI property layout not derived";
    if (!input->words || !property->words || !property->count || !info) return;
    if (hf_is_brk(property->words[0]) || !hf_kprobe_placeable(property->words[0])) return;
    out->property_reason = hf_prepare(input);
    if (out->property_reason) return;
    hf_enter(input);
    for (index = 0; index < input->count; index++) {
        ed_u32 op = input->words[index];
        if (index < 16 && (op & 0xffc003ffu) == 0xd10003ffu) {
            out->input_frame_size = (op >> 10) & 0xfff;
            frames++;
        }
        if (index < 20 && (op & 0xffffffe0u) == 0x12001c20u) {
            out->input_forced_reg = op & 31;
            forced++;
        }
        if (index < 20 && (op & 0xffffffe0u) == 0xaa0003e0u && (op & 31) >= 19 && (op & 31) <= 28) {
            out->input_arg_reg = op & 31;
            inputs++;
        }
        if ((op & 0xfc000000u) == 0x94000000u) {
            ed_u64 target = input->address + 4ULL * index + ((ed_u64)ed_sext(op & 0x3ffffff, 26) << 2);
            if (target == info && index >= 3 &&
                (input->words[index - 3] & 0xffc003ffu) == 0x910003e1u) {
                out->input_type_offset = ((input->words[index - 3] >> 10) & 0xfff) + 4;
                infos++;
            }
            if (target == property->address && hf_reg[1].kind == HK_CONST) {
                unsigned int existing;
                for (existing = 0; existing < out->input_format_count; existing++)
                    if (out->input_formats[existing] == hf_reg[1].c) break;
                if (existing == out->input_format_count) {
                    if (existing == 32) { out->property_reason = "too many HDMI property formats"; return; }
                    out->input_formats[out->input_format_count++] = hf_reg[1].c;
                }
            }
        }
        hf_step(index);
    }
    if (frames != 1 || infos != 1 || forced != 1 || inputs != 1 || out->input_forced_reg < 19 || out->input_forced_reg > 28 ||
        !out->input_frame_size || out->input_frame_size > 4096 ||
        out->input_type_offset + 8 > out->input_frame_size || !out->input_format_count) {
        out->property_reason = "HDMI stack frame, audio-info or forced-PCM flag is ambiguous";
        return;
    }
    out->input_set_begin = input->address;
    out->input_set_end = input->address + 4ULL * input->count;
    out->property_entry = property->address;
    out->property_reason = 0;
}

/* input_drop_data(input, bool) loads its uid with `ldr w0,[x0,#uid]`. */
static void hd_input_format(const struct earc_code *fns, struct earc_hdmi_layout *out) {
    const struct earc_code *drop = &fns[HF_input_drop_data];
    unsigned int index, moves = 0, masks = 0, uids = 0;
    out->format_reason = "HDMI format-change helpers not found";
    if (!drop->words || !fns[HF_aimc_register_named_event].address || !fns[HF_aimc_unregister_named_event].address ||
        !out->decoder_lock)
        return;
    for (index = 0; index < drop->count && index < 16; index++) {
        ed_u32 op = drop->words[index];
        if (op == 0xaa0003f3u) moves++;
        if (op == 0x12001c34u) masks++;
        if ((op & 0xffc003ffu) == 0xb9400000u) { out->input_uid_offset = ((op >> 10) & 0xfff) * 4; uids++; }
    }
    if (moves != 1 || masks != 1 || uids != 1 || !out->input_uid_offset || out->input_uid_offset >= out->input_codec_offset) {
        out->format_reason = "input_drop_data does not have the expected shape";
        return;
    }
    out->format_reason = 0;
}

/* The one call through a file_operations slot in fn, in the shape every measured
 * kernel uses: `ldr xF,[xFile,#f_op]` (or a str of xF there), `ldr xS,[xF,#slot]`,
 * `cbz xS`, then `blr xS` within 8 instructions. Returns slot and sets *fileops
 * to the f_op offset, or 0 unless exactly one such call exists (or, with want
 * set, exactly one at that slot). */
static unsigned int fops_slot(const struct earc_code *fn, unsigned int *fileops, unsigned int want) {
    unsigned int i, k, found = 0, slot = 0;
    if (!fn->words) return 0;
    for (i = 1; i + 1 < fn->count; i++) {
        ed_u32 load = fn->words[i], reg, base, off = 0, call = 0, k2;
        if ((load & 0xffc00000u) != 0xf9400000u) continue;  /* ldr xS,[xF,#imm] */
        reg = load & 31;
        if ((fn->words[i + 1] & 0xff00001fu) != (0xb4000000u | reg)) continue;  /* cbz xS */
        for (k2 = i + 2; k2 < fn->count && k2 <= i + 8; k2++)
            if (fn->words[k2] == (0xd63f0000u | (reg << 5))) { call = 1; break; }  /* blr xS */
        if (!call) continue;
        base = (load >> 5) & 31;
        for (k = i; k-- > 0 && i - k <= 8;) {
            ed_u32 w = fn->words[k];
            /* ldr xF,[x,#imm] or str xF,[x,#imm]: the f_op field. */
            if ((w & 0xffc0001fu) == (0xf9400000u | base) || (w & 0xffc0001fu) == (0xf9000000u | base)) {
                off = ((w >> 10) & 0xfff) * 8;
                break;
            }
        }
        if (!off || (want && ((load >> 10) & 0xfff) * 8 != want)) continue;
        found++;
        *fileops = off;
        slot = ((load >> 10) & 0xfff) * 8;
    }
    return found == 1 ? slot : 0;
}

static void hd_fops(const struct earc_code *fns, struct earc_hdmi_layout *out) {
    unsigned int open_fileops = 0, release_fileops = 0, write_fileops = 0;
    /* 5.4 calls ->write from __vfs_write; 6.12 has none and calls it from vfs_write. */
    const struct earc_code *write = fns[HF___vfs_write].words ? &fns[HF___vfs_write] : &fns[HF_vfs_write];
    out->fops_reason = "misc_open/__fput/vfs_write not given";
    if (!fns[HF_misc_open].words || !fns[HF___fput].words || !write->words || !fns[HF_misc_register].address ||
        !fns[HF_misc_deregister].address)
        return;
    out->fops_open = fops_slot(&fns[HF_misc_open], &open_fileops, 0);
    /* __fput also calls fasync (two slots later); release follows open. */
    out->fops_release = out->fops_open ? fops_slot(&fns[HF___fput], &release_fileops, out->fops_open + 16) : 0;
    out->fops_write = fops_slot(write, &write_fileops, 0);
    /* All three read f_op from the same struct file field; write precedes open,
     * release follows it, and owner (+0) comes first. */
    if (!out->fops_open || !out->fops_release || !out->fops_write || open_fileops != release_fileops ||
        open_fileops != write_fileops || out->fops_release != out->fops_open + 16 ||
        out->fops_write < 8 || out->fops_write >= out->fops_open || out->fops_open > 0x100) {
        out->fops_reason = "file_operations write/open/release slots not derived";
        out->fops_write = out->fops_open = out->fops_release = 0;
        return;
    }
    out->fops_reason = 0;
}

/* LG's "Sndout EARC Outputtype" put: five words from the ALSA request (x1); the
 * fifth, the mix flag, is the one checked with `ccmp wM,#1` (0 or 1 only) and
 * stored once. Measured: `ldr x24,[x19,#0x68]` after `mov x19,x1` on 5.4 and 6.12. */
static void hd_mix(const struct earc_code *fns, struct earc_hdmi_layout *out) {
    const struct earc_code *f = &fns[HF_sndout_earc_output_type];
    unsigned int i, j, request = 32, reg = 32, loads = 0, checks = 0;
    out->mix_reason = "sndout_earc_output_type not given";
    if (!f->words || f->count < 8) return;
    out->mix_entry = f->address;
    if (hf_is_brk(f->words[0]) || !hf_kprobe_placeable(f->words[0])) {
        out->mix_reason = "sndout_earc_output_type entry is not kprobe-placeable";
        return;
    }
    for (i = 0; i < f->count && i < 16; i++)
        if ((f->words[i] & 0xffffffe0u) == 0xaa0103e0u) { request = f->words[i] & 31; break; }  /* mov xR, x1 */
    for (i = 0; request < 31 && i < f->count; i++) {
        ed_u32 op = f->words[i];
        if ((op & 0xffc003e0u) != (0xf9400000u | (request << 5))) continue;  /* ldr xM,[xR,#off] */
        for (j = i + 1; j < f->count && j <= i + 64; j++)
            /* ccmp wM,#1,#nzcv,cond: opcode, imm5 = 1 and Rn; cond and nzcv vary. */
            if ((f->words[j] & 0xffff0fe0u) == (0x7a410800u | ((op & 31) << 5))) {
                reg = op & 31;
                out->mix_offset = ((op >> 10) & 0xfff) * 8;
                loads++;
                break;
            }
    }
    for (i = 0; reg < 31 && i < f->count; i++)
        if ((f->words[i] & 0xffc0001fu) == (0xb9000000u | reg)) checks++;  /* str wM,[x,#imm] */
    if (loads != 1 || !checks) {
        out->mix_reason = "the mix flag's load and check were not found exactly once";
        out->mix_offset = 0;
        return;
    }
    out->mix_reason = 0;
}

static void hd_mute(const struct earc_code *fns, struct earc_hdmi_layout *out) {
    const struct earc_code *f = &fns[HF_sndout_mute_input];
    unsigned int index, request = 32, moves = 0, matches = 0;
    out->mute_reason = "sndout_mute_input request not derived";
    if (!f->words || !f->count || hf_is_brk(f->words[0]) || !hf_kprobe_placeable(f->words[0])) return;
    if (hf_prepare(f)) return;
    for (index = 0; index < f->count && index < 16; index++) {
        ed_u32 op = f->words[index];
        if ((op & 0xffffffe0u) == 0xaa0103e0u && (op & 31) >= 19 && (op & 31) <= 28) {
            request = op & 31;
            moves++;
        }
    }
    for (index = 0; moves == 1 && index + 3 < f->count; index++) {
        ed_u32 load = f->words[index], pair = f->words[index + 1];
        unsigned int mute = load & 31, offset = ((load >> 10) & 0xfff) * 8;
        int pair_offset = (int)ed_sext(pair >> 15, 7) * 8;
        if ((load & 0xffc003e0u) != (0xf9400000u | (request << 5)) || mute == 31 ||
            (pair & 0xffc003e0u) != (0xa9400000u | (request << 5)) ||
            pair_offset < 8 || pair_offset > 232 || offset != (unsigned int)pair_offset + 16 ||
            (pair & 31) == mute || ((pair >> 10) & 31) == mute ||
            f->words[index + 2] != (0x7100041fu | (mute << 5)) ||
            (f->words[index + 3] & 0xff00001fu) != 0x54000008u) continue;
        out->mute_offset = (unsigned int)pair_offset;
        matches++;
    }
    if (matches != 1) return;
    out->mute_entry = f->address;
    out->mute_reason = 0;
}

static void hd_symbols(const struct earc_code *fns, struct earc_hdmi_layout *out) {
    out->patch_text = fns[HF_aarch64_insn_patch_text].address;
    out->kprobe_register = fns[HF_register_kprobe].address;
    out->kprobe_unregister = fns[HF_unregister_kprobe].address;
    out->cpus_read_lock = fns[HF_cpus_read_lock].address;
    out->cpus_read_unlock = fns[HF_cpus_read_unlock].address;
}

/* Derives the HDMI set. fns[i] is zero for a function the caller could not
 * validate (earc_hdmi_valid). nonpcm_virtual is struct earc_layout.nonpcm;
 * stext_align is (_stext & ~0xfffff): the kernel image is loaded at physical 0
 * plus its text offset on these TVs, so a kernel-image address is physical
 * address + stext_align. Every item carries its own reason. */
static void earc_hdmi_decode(const struct earc_code *fns, ed_u64 nonpcm_virtual, ed_u64 stext_align,
                             struct earc_hdmi_layout *out) {
    unsigned int i;
    for (i = 0; i < sizeof(*out) / sizeof(ed_u64); i++) ((ed_u64 *)out)[i] = 0;
    out->set_output_reason = out->adec_reason = out->cb_reason = out->break_reason = "not derived";
    out->hd_output_reg = 32;
    hd_gate(fns, out);
    hd_set_output(fns, out);
    hd_adec(fns, out);
    hd_cb(fns, out);
    hd_break(fns, out);
    hd_symbols(fns, out);
    hd_input_start(fns, out);
    hd_input_properties(fns, out);
    hd_input_format(fns, out);
    hd_fops(fns, out);
    hd_mix(fns, out);
    hd_mute(fns, out);
    out->nonpcm_virtual = nonpcm_virtual;
    out->nonpcm_physical = (nonpcm_virtual && nonpcm_virtual >= stext_align) ? nonpcm_virtual - stext_align : 0;
}

/* ---- the break-hook list walk ---- */
/* A bounded forward walk of the kernel's break-hook list, restricted to nodes
 * inside the kernel image [low, high). Such a node is a static object that is
 * never freed, and removing another node only rewrites a next pointer with one
 * aligned store, so reading next from image nodes needs no RCU. A node outside
 * the image (a module's) is never read: the walk stops there. For each image
 * node only its next, its fn and the 32-bit word after fn are read. */
#define EARC_HDMI_WALK_MAX 32
enum { HW_COMPLETE, HW_OUTSIDE, HW_MISALIGNED, HW_CAPPED, HW_UNREADABLE };
struct earc_hdmi_walk {
    int status;
    unsigned int count;           /* image nodes read */
    ed_u64 stop;                  /* HW_OUTSIDE, HW_MISALIGNED, HW_CAPPED: the address not read */
    int saw_break, saw_step, saw_kret;
    struct { ed_u64 address, fn; ed_u32 word; } nodes[EARC_HDMI_WALK_MAX];
};
/* Reads width (4 or 8) bytes at address; returns 0 if it cannot. */
typedef int (*earc_hdmi_read)(void *context, ed_u64 address, unsigned int width, ed_u64 *value);

static void earc_hdmi_walk_list(const struct earc_hdmi_layout *layout, ed_u64 low, ed_u64 high,
                                earc_hdmi_read read, void *context, struct earc_hdmi_walk *out) {
    ed_u64 head = layout->break_list_head, node = 0, next, fn, word;
    ed_u64 span = (layout->break_fn_offset > layout->break_word_offset ? layout->break_fn_offset + 8 : layout->break_word_offset + 4);
    out->status = HW_UNREADABLE;
    out->count = 0;
    out->stop = 0;
    out->saw_break = out->saw_step = out->saw_kret = 0;
    if ((head & 7) || head < low || head >= high || high - head < 16) { out->status = HW_OUTSIDE; out->stop = head; return; }
    if (!read(context, head, 8, &node)) return;
    for (;;) {
        if (node == head) { out->status = HW_COMPLETE; return; }
        if (node < low || node >= high || high - node < span) { out->status = HW_OUTSIDE; out->stop = node; return; }
        if (node & 7) { out->status = HW_MISALIGNED; out->stop = node; return; }
        if (out->count == EARC_HDMI_WALK_MAX) { out->status = HW_CAPPED; out->stop = node; return; }
        if (!read(context, node, 8, &next) || !read(context, node + layout->break_fn_offset, 8, &fn) ||
            !read(context, node + layout->break_word_offset, 4, &word)) { out->status = HW_UNREADABLE; out->stop = node; return; }
        out->nodes[out->count].address = node;
        out->nodes[out->count].fn = fn;
        out->nodes[out->count].word = (ed_u32)word;
        out->count++;
        if (fn == layout->break_fn) out->saw_break = 1;
        if (fn == layout->step_fn) out->saw_step = 1;
        if (layout->kret_fn && fn == layout->kret_fn) out->saw_kret = 1;
        node = next;
    }
}

#endif
