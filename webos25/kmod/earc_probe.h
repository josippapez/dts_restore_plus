/* SPDX-License-Identifier: GPL-2.0-only */
/* Decision logic for the HDMI-passthrough active modes (mode>=2), shared by the
 * module and the host test so both run the same code, as earc_hdmi.h is. It
 * holds no kernel ABI and calls nothing: the kernel structs (struct kprobe,
 * struct pt_regs, struct break_hook) and the live register/patch/probe calls
 * live in earc_route.c, which is the module only. Here are only the pure rules:
 *
 *  - the break-hook decision (is the list already set up, must we register,
 *    or must we refuse for a conflict or an incomplete walk),
 *  - the DTS-core policy match a _set_output probe applies,
 *  - the "only restore the word we wrote" gate rule,
 *  - per-mode refusal from the derived HDMI layout.
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

/* lg-g5-dts's DTS-core policy for a _set_output(index, descriptor, ...) call:
 * decoder index 1, the descriptor's mask word == 0x20 (eARC), and ADEC 1 open
 * and started on codec 48 (DTS). The caller has already confirmed the
 * descriptor pointer is non-null and read these through the derived addresses. */
static int earc_policy_match(ed_u64 index, ed_u32 mask_word, ed_u32 codec, ed_u8 open, ed_u8 started) {
    return index == 1 && mask_word == 0x20 && codec == 48 && open == 1 && started == 1;
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

/* Per-mode refusal from the derived layout, the code-fact half (address-range
 * checks are the module's, done against _etext at load). mode 2 needs the break
 * hooks derived; mode 3 needs the gate stock and patch service; modes 4 and 5
 * need both plus the _set_output probe site, the mask offset and ADEC state.
 * Returns the reason or 0. */
static const char *earc_mode_refusal(const struct earc_hdmi_layout *h, int mode) {
    int needs_hooks = mode == 2 || mode == 4 || mode == 5;
    int needs_gate = mode == 3 || mode == 4 || mode == 5;
    int needs_probe = mode == 4 || mode == 5;
    if (needs_hooks) {
        if (h->break_reason) return h->break_reason;
        if (!h->break_fn || !h->step_fn) return "kprobe break handlers not derived";
        /* Both are called through these addresses; a symbol the loader could
         * not pass is 0 here. */
        if (!h->break_register) return "register_kernel_break_hook not found";
        if (!h->break_unregister) return "unregister_kernel_break_hook not found";
        if (!h->break_fields_known) return "break-hook immediate and mask not derived (cannot check for a conflict)";
    }
    if (needs_gate) {
        if (h->gate_state == GATE_BREAKPOINT) return "DTS gate holds a breakpoint (another tool is probing it)";
        if (h->gate_state == GATE_SWITCHED) return "DTS gate is already switched (another tool opened it)";
        if (h->gate_state != GATE_STOCK) return "DTS gate is not in its stock state";
        if (!h->patch_text) return "aarch64_insn_patch_text not found";
        if (!h->cpus_read_lock || !h->cpus_read_unlock) return "cpus_read_lock/unlock not found";
    }
    if (needs_probe) {
        if (h->set_output_reason) return h->set_output_reason;
        if (h->set_output_first_brk) return "_set_output entry holds a breakpoint";
        if (h->adec_reason) return h->adec_reason;
    }
    return 0;
}

#endif
