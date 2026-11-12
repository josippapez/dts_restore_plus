/* SPDX-License-Identifier: GPL-2.0-only */
/* Decision logic for the session load (mode 7), shared by the
 * module and the host test so both run the same code, as earc_hdmi.h is. It
 * holds no kernel ABI and calls nothing: the kernel structs (struct kprobe,
 * struct pt_regs, struct break_hook) and the live register/patch/probe calls
 * live in earc_route.c, which is the module only. Here are only the pure rules:
 *
 *  - the break-hook decision (is the list already set up, must we register,
 *    or must we refuse for a conflict or an incomplete walk),
 *  - the _set_output policies (an HDMI DTS stream, an app holding the lease),
 *  - the "only restore the word we wrote" gate rule,
 *  - the two DTS-HD probes in _cb_output_status (accept codec 7, keep its
 *    name load inside the table),
 *  - refusal from the derived HDMI layout.
 *
 * None of these writes anything; the module turns a decision into an action. */
#ifndef EARC_PROBE_H
#define EARC_PROBE_H

#include "earc_hdmi.h"

/* The BRK immediates a plain pre-handler kprobe makes the CPU take: #4 when the
 * planted breakpoint traps (KPROBES_BRK_IMM), #6 when the out-of-line
 * single-step slot finishes (KPROBES_BRK_SS_IMM). Measured: arch_prepare_kprobe
 * builds 0xd42000c0 = brk #6 as the slot trailer, and earc_hdmi.h's HF_KPROBE_BRK
 * is 0xd4200080 = brk #4. A plain pre-handler probe never reaches the kretprobe
 * trampoline (brk #7, 6.12 only), so that hook is not needed. */
#define EARC_BRK_IMM_PROBE 4
#define EARC_BRK_IMM_STEP  6

/* What stage a must do with the kernel's break-hook list. */
enum earc_hook_decision {
    EHD_ALREADY,    /* both kprobe handlers already on the list: do nothing */
    EHD_REGISTER,   /* register module-owned hooks for #4 and #6 */
    EHD_INCOMPLETE, /* the walk did not complete: refuse */
    EHD_CONFLICT    /* a node already answers #4 or #6: refuse */
};

/* call_break_hook's rule: a node answers BRK immediate `want` iff
 * (want & ~node_mask) == node_imm. */
static int earc_hook_answers(unsigned int want, ed_u32 node_imm, ed_u32 node_mask) {
    return ((want & ~node_mask) & 0xffff) == (node_imm & 0xffff);
}

/* Decides stage a from the audit walk and the derived handler addresses. The
 * walk must be complete (every image node seen, list returned to the head);
 * if both kprobe handlers are already linked the kernel owns them and we touch
 * nothing; otherwise no existing node may answer #4 or #6, and the immediate
 * and mask fields must have been derived so the conflict rule can be applied. */
static enum earc_hook_decision earc_hook_decide(const struct earc_hdmi_walk *walk,
                                                int break_fields_known) {
    unsigned int i;
    if (walk->status != HW_COMPLETE) return EHD_INCOMPLETE;
    if (walk->saw_break && walk->saw_step) return EHD_ALREADY;
    if (!break_fields_known) return EHD_CONFLICT;  /* cannot prove no conflict */
    for (i = 0; i < walk->count; i++) {
        ed_u32 imm = walk->nodes[i].word & 0xffff, mask = walk->nodes[i].word >> 16;
        if (earc_hook_answers(EARC_BRK_IMM_PROBE, imm, mask) ||
            earc_hook_answers(EARC_BRK_IMM_STEP, imm, mask))
            return EHD_CONFLICT;
    }
    return EHD_REGISTER;
}

/* _set_output's index is the main-audio decoder: an ADEC port (0..3) as itself,
 * an AMIXER port N as N + 4 (common_input_convert_index). Its bypass branch
 * connects renderer `index` and calls decoder_set_output_mode(index, 1, 1),
 * which takes 0 or 1 and the same ADEC table, so ADEC slot = renderer =
 * decoder for 0 and 1. Any other index has no ADEC slot: earc_adec_slot gives
 * the three addresses of slot 0 or 1 and refuses every other index, so a caller
 * that reads only what it returns never reads past the two derived slots. */
struct earc_adec_slot { ed_u64 codec, open, started; };
static int earc_adec_slot(const struct earc_hdmi_layout *h, ed_u64 index, struct earc_adec_slot *slot) {
    if (index > 1) return 0;
    slot->codec = index ? h->adec1_codec : h->adec0_codec;
    slot->open = index ? h->adec1_open : h->adec0_open;
    slot->started = index ? h->adec1_started : h->adec0_started;
    return 1;
}

/* ---- DTS-HD: lg-g5-dts's tools/dts-hd-route ----
 * LG's output-status codec for DTS-HD is 7 and the ADEC public codec is 51
 * (status_hooks.h, policy.h). _cb_output_status accepts codecs up to the derived
 * bound (4) and names the codec from a table of 6 entries, so codec 7 is turned
 * into PCM and its name is loaded from past the table. */
#define EARC_HD_EVENT_CODEC 7
#define EARC_HD_ADEC_CODEC  51
#define EARC_OUTPUT_EARC    3   /* output type, at output+44 (earc_decode checks it in output_set_earc_on) */
#define EARC_OUTPUT_TYPE_OFFSET 44

/* An HDMI DTS stream: _set_output(index, descriptor, ...) for decoder 0 or 1 on
 * eARC (mask 0x20) whose ADEC slot is open and started on DTS core (48) or
 * DTS-HD (51). The caller null-checked the descriptor. */
static int earc_session_policy_match(ed_u64 index, ed_u32 mask_word, ed_u32 codec, ed_u8 open, ed_u8 started) {
    return index <= 1 && mask_word == 0x20 && (codec == 48 || codec == EARC_HD_ADEC_CODEC) && open == 1 && started == 1;
}

/* An app holding the lease (app_route = renderer index + 1) gets LG's eARC bypass for that renderer. */
static int earc_app_policy_match(ed_u64 index, ed_u32 mask_word, ed_u32 app_route) {
    return app_route && index == app_route - 1 && mask_word == 0x20;
}

/* The registers the two probes read and write, from the probed instructions:
 * `cmp wC,#bound` and `ldr xN,[xT,xI,lsl #3]`. */
static unsigned int earc_hd_codec_reg(const struct earc_hdmi_layout *h) { return (h->hd_compare_word >> 5) & 31; }
static unsigned int earc_hd_index_reg(const struct earc_hdmi_layout *h) { return (h->hd_name_word >> 16) & 31; }
static unsigned int earc_hd_name_reg(const struct earc_hdmi_layout *h) { return h->hd_name_word & 31; }

/* The probe on the acceptance compare: 1 means continue at the supported
 * branch (pc = hd_supported_branch, compare skipped, earc_hd_accept_pstate)
 * with the codec register untouched. Only for a DTS-HD event on the eARC output
 * while the ADEC slot given is started on DTS-HD. The caller null-checked the
 * output. */
static int earc_hd_accept(ed_u32 event_codec, ed_u32 output_type, ed_u32 adec_codec, ed_u8 adec_started) {
    return event_codec == EARC_HD_EVENT_CODEC && output_type == EARC_OUTPUT_EARC &&
        adec_codec == EARC_HD_ADEC_CODEC && adec_started == 1;
}

/* Which ADEC slot the acceptance reads. `matched` is the decoder of the last
 * _set_output policy match (0 or 1), or EARC_NO_DECODER before the first one.
 * After a match only that decoder's slot counts: the other decoder can stay
 * open on an earlier input (a C5 kept adec.p1 open on an idle input while
 * adec.p0 played), so its slot says nothing about this output. Before any
 * match (mode 6 has no _set_output probe; in mode 7 nothing orders the first
 * match before a status event) nothing says which decoder feeds the output, so
 * either slot started on 51 accepts: lg-g5-dts's rule on ADEC 1, applied to
 * both decoders. Only codec[0..1] and started[0..1] are read, whatever
 * `matched` holds. Returns the decoder accepted for, or -1. */
#define EARC_NO_DECODER 2u
static int earc_hd_accept_decoder(ed_u32 event_codec, ed_u32 output_type, ed_u32 matched,
                                  const ed_u32 codec[2], const ed_u8 started[2]) {
    unsigned int n;
    for (n = 0; n < 2; n++)
        if ((matched > 1 || matched == n) && earc_hd_accept(event_codec, output_type, codec[n], started[n])) return (int)n;
    return -1;
}

/* The flags the skipped compare leaves when it accepts: those of `cmp x,#x`
 * (Z and C set, N and V clear), which satisfy both the b.ls of shape 1 and the
 * fall-through of shape 2's b.ne. lg-g5-dts leaves the flags of an earlier
 * compare; this makes the redirected state one the stock branch can produce. */
static ed_u64 earc_hd_accept_pstate(ed_u64 pstate) {
    return (pstate & ~0xf0000000ULL) | 0x60000000ULL;
}

/* The probe on the name load: an index inside the table keeps LG's load (0);
 * any other is given the table's own last entry and the load is skipped
 * (pc = hd_name_load + 4), so nothing past the table is ever used as a
 * pointer. The substitute is a string in the kernel image ("???" on every
 * kernel measured), not module memory: LG's log call uses it after the
 * pre-handler has returned, which an unload must not be able to outrun. */
static int earc_hd_name_substitute(ed_u64 index, unsigned int entries) {
    return index >= entries;
}

/* Some stock kernels already accept codec 7 and name it from inside the table:
 * C3/G3 33.31.68 and G4 23.23.30 compare against 9 and have 11 names. Mode 7
 * then places neither DTS-HD probe; LG's own compare and load already do what
 * the probes would. */
static int earc_hd_native(const struct earc_hdmi_layout *h, unsigned int name_entries) {
    return h->hd_compare_bound >= EARC_HD_EVENT_CODEC && name_entries > h->hd_compare_bound;
}

/* What the session needs of _cb_output_status, beyond earc_mode_refusal.
 * name_entries is the live count of the name table's entries. cb_reason covers
 * the compare, the supported branch, the name load and any breakpoint in the
 * function. On a native kernel (earc_hd_native) no probe goes there, so only
 * the derivation itself is required. Returns the reason or 0. */
static const char *earc_hd_refusal(const struct earc_hdmi_layout *h, unsigned int name_entries) {
    if (h->cb_reason) return h->cb_reason;
    if (h->adec_reason) return h->adec_reason;
    if (h->hd_compare_bound >= EARC_HD_EVENT_CODEC) {
        if (name_entries <= h->hd_compare_bound) return "the codec-name table is smaller than the accepted codecs";
        return 0;
    }
    if (h->hd_output_reg > 30) return "the output register at the acceptance compare was not derived";
    if (earc_hd_codec_reg(h) == 31 || earc_hd_index_reg(h) == 31 || earc_hd_name_reg(h) == 31)
        return "a probed _cb_output_status instruction uses SP or ZR";
    if (!hf_kprobe_placeable(h->hd_compare_word) || !hf_kprobe_placeable(h->hd_name_word))
        return "a probed _cb_output_status instruction is not kprobe-placeable";
    if (name_entries <= h->hd_compare_bound) return "the codec-name table is smaller than the accepted codecs";
    return 0;
}

/* The gate state machine's restore rule: restore the stock first word only if
 * the current first word is still the one this module wrote. `current` is the
 * live first word; `written` is what we patched in (0x52800020). Returns the
 * word to write back, or 0 if nothing should be written. */
#define EARC_GATE_STOCK_W0  0x52800000u  /* mov w0, #0 */
#define EARC_GATE_OPEN_W0   0x52800020u  /* mov w0, #1 */
static ed_u32 earc_gate_restore_word(ed_u32 current, ed_u32 written) {
    return current == written ? EARC_GATE_STOCK_W0 : 0;
}

/* The session's refusal from the derived layout, the code-fact half (address
 * checks are the module's, against _etext at load): the break hooks, the gate
 * (stock, or already returning 1 as on C3/G3, G4 and 2026 kernels, which is
 * neither patched nor restored) with its patch service, and the _set_output
 * probe site, mask offset and ADEC state. Returns the reason or 0. */
static const char *earc_mode_refusal(const struct earc_hdmi_layout *h) {
    if (h->break_reason) return h->break_reason;
    if (!h->break_fn || !h->step_fn) return "kprobe break handlers not derived";
    /* Both are called through these addresses; a symbol the loader could not pass is 0 here. */
    if (!h->break_register) return "register_kernel_break_hook not found";
    if (!h->break_unregister) return "unregister_kernel_break_hook not found";
    if (!h->break_fields_known) return "break-hook immediate and mask not derived (cannot check for a conflict)";
    if (h->gate_state == GATE_BREAKPOINT) return "DTS gate holds a breakpoint (another tool is probing it)";
    if (h->gate_state != GATE_STOCK && h->gate_state != GATE_SWITCHED) return "DTS gate is not in its stock state";
    if (h->gate_state == GATE_STOCK) {
        if (!h->patch_text) return "aarch64_insn_patch_text not found";
        if (!h->cpus_read_lock || !h->cpus_read_unlock) return "cpus_read_lock/unlock not found";
    }
    if (h->set_output_reason) return h->set_output_reason;
    if (h->set_output_first_brk) return "_set_output entry holds a breakpoint";
    if (h->adec_reason) return h->adec_reason;
    return 0;
}

#endif
