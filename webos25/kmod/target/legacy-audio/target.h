/* SPDX-License-Identifier: GPL-2.0-only */
/* LG's old AUDIO_* driver on webOS 6 (C1 and its 2021 siblings, kernel 4.4.84-229).
 * No address is built in: each function comes from kallsyms and must have the analysed
 * shape (core/code.h), and the data the hooks read is derived from LG's own code. */
#ifndef EARC_TARGET_LEGACY_AUDIO_H
#define EARC_TARGET_LEGACY_AUDIO_H

/* X(name, instructions, shape). The kernel's own break-hook and patching code first. */
#define EARC_FUNCTIONS(X) \
    X(brk_handler, 48, 0xf9b3ebae) \
    X(register_break_hook, 22, 0x1b85b958) \
    X(unregister_break_hook, 22, 0xa9f2f344) \
    X(aarch64_insn_patch_text, 48, 0x5b32365f) \
    X(__aarch64_insn_write, 68, 0x2b0c4fa9) \
    X(AUDIO_SetIecOutEncodedType, 76, 0x1807a087) \
    X(sndout_earc_output_type, 98, 0x2e09f863) \
    X(_AUDIO_SetSRCFormat, 64, 0x126d7054) \
    X(sndout_earc_onoff, 62, 0xfcee5c03) \
    X(AUDIO_GetMainAudioIndex, 22, 0x5e81df6b) \
    X(sndout_connect_output, 246, 0x5f1b9283) \
    X(_arc_connect_output, 224, 0x757c904c) \
    X(_AUDIO_ConnectRenderer, 190, 0xfbd15345) \
    X(_AUDIO_ConnectOutput_FindOutCtrlModule, 290, 0x6d51cac6) \
    X(_AUDIO_ConnectOutput_FindConnectModule, 508, 0x4d33bae8)
#include "../../core/code.h"

/* Encoded types: 3 = ARC port; 6 = MAT, LG's HBR passthrough. The shapes pin these values
 * and offsets: renderer + 0x1b0 = device and device + 0x8c = index (_AUDIO_ConnectRenderer),
 * request + 0x68 = mix flag, and _AUDIO_SetSRCFormat's format at sp + 0x40. */
static const struct earc_hook earc_hooks[] = {
    /* AUDIO_SetIecOutEncodedType(port, type): eARC carries the leased stream as HBR. */
    {EF_AUDIO_SetIecOutEncodedType, 0, 0xa9bd7bfd, WHEN_EARC_PORT(0, 3), SET_REG(1, 6)},
    /* sndout_earc_output_type(kcontrol, request): clear LG's eARC mix flag. */
    {EF_sndout_earc_output_type, 0, 0xd10203ff, WHEN_LEASE(), CLEAR_U64(1, 0x68)},
    /* _AUDIO_SetSRCFormat before its output defaults: x19 = renderer. */
    {EF__AUDIO_SetSRCFormat, 0x6c, 0x52800400, WHEN_LEASE_DEVICE(19, 0x1b0, 0x8c), CARRIER(0x40)},
};

/* LG's eARC-on byte and main renderer index: the one byte store in sndout_earc_onoff and the
 * one word load in AUDIO_GetMainAudioIndex, cross-checked against their kallsyms names. */
static volatile u8 *earc_on;
static volatile u32 *earc_main_index;

static const char *earc_target_derive(const struct earc_code *fns)
{
    struct earc_scan scan;
    earc_scan(&fns[EF_sndout_earc_onoff], 1, 1, &scan);
    if (scan.found != 1 || scan.address != kallsyms_lookup_name("_gSndOutEarcOnOff"))
        return "_gSndOutEarcOnOff";
    earc_on = (volatile u8 *)scan.address;
    earc_scan(&fns[EF_AUDIO_GetMainAudioIndex], 4, 0, &scan);
    if (scan.found != 1 || scan.address != kallsyms_lookup_name("_gMainAudioIndex"))
        return "_gMainAudioIndex";
    earc_main_index = (volatile u32 *)scan.address;
    return 0;
}

#endif
