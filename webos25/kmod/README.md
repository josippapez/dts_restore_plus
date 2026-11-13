# eARC route kernel module (webOS 25, kernels 5.4.268 and 6.12.44)

`earc_route-<generation>.ko` lets a bitstream reach an eARC receiver through
LG's bypass. The tested C2 app path uses `mode=8` (`--app`): only the lease,
output override and mix protection, without a DTS gate patch or HDMI hooks.
`mode=7` retains HDMI DTS forwarding. It is built once per kernel generation,
from one source: `earc_route-5.4.ko` (webOS 25 firmware 33.31.x, kernel
5.4.268-329.ptl4tv.5, and 33.22.x, kernel 5.4.268-329.ptl4tv.2) and
`earc_route-6.12.ko` (firmware 43.21.x, kernel 6.12.44-352), the generation-specific values passed as `-D` macros by `build.py`.
The loader passes each LG function's address and size from `/proc/kallsyms`, and
the module derives every data address and struct offset from LG's own code when
it loads. If anything does not match, it refuses to load.

The audit (`mode=0`) also derives, read-only, the set an HDMI-input -> eARC
passthrough feature would need (`earc_hdmi.h`), by symbol name and one exact
pattern each, and prints it on `earc_route: hdmi` lines. This is the audit half
only: nothing is probed, hooked or patched, no new kernel function is called and
no import is added. A missing HDMI symbol or unmatched pattern is reported "not
available", never a refusal, so the eARC route still loads.

`mode=7` (`--dts-hd`) is the session load, a port of
[lg-g5-dts](https://github.com/dakenf/lg-g5-dts)'s `tools/dts-hd-route`
(GPL-2.0-only) onto our derived addresses. It writes to kernel memory
(break-hook list, up to three kprobes, one patched instruction); every write is
listed with its undo under [The session load](#the-session-load-mode7). It
handles either HDMI audio decoder, 0 or 1, and keeps an HDMI input on DTS by itself
(see [HDMI input](#hdmi-input-mode7)).

Status: both builds host-tested against the kernels listed under Test.
Experimental. Automatic loading on 6.12 is disabled pending real-TV verification.
The release ships both builds and `earc-route-load.sh` for the app
(see [In the app](#in-the-app)); everything else here is root over SSH.

On 2026-11-11 a tester's LG CS (OLED55CS6LA, firmware 33.31.68, kernel
5.4.268-329.ptl4tv.5) with a Samsung HW-Q930F over eARC and a set-top box on
HDMI 1 ran mode 7 with a since-removed userspace script that wrote the HDMI
input's DSP properties. The soundbar showed DTS for a DTS core stream, DTS-HD for
DTS-HD MA and DTS:X for DTS:X.

On one C5 (33.31.68, kernel 5.4.268), also 2026-11-11, `mode=7` ran once on a
live DTS core stream without a receiver: both self-tests passed, three probes
were placed and unload left the TV stock.

On 2026-11-12 a C2 (OLED42C27LA, 33.31.68) with an eARC receiver ran this build with
the `app_route` lease on renderer 0 while the app played DTS-HD MA: the
`_set_output` probe forced LG's bypass twice, eARC went non-PCM, and unload left
the gate stock. The same TV ran this build with the app again (rebuilt lease
plugin) and with the boot script's supervisor, which loaded and unloaded it.

Not run on any TV: kernel 5.4.268-329.ptl4tv.2 (33.22.x), host-tested only, and
the 6.12 build, which passed the audit and a session load in QEMU on a C5 43.21.x
kernel with LG's audio hardware init stubbed.

Decoder 0 is host-tested only for HDMI inputs. Only a TV with an eARC
receiver can confirm the decoder-0 override: on the C5's speakers `_set_output`
never matches (`mask=0x2`). Every `pass match`, `pass nomatch` and `hd` line
prints the decoder, so the next receiver run shows which one played.
`out/` is ignored. The release ships the `.ko` files in `webos25/restore/kmod-out/`.
After a rebuild, copy them there (RELEASING.md). Neither generation uses
MODVERSIONS (no `__versions`, no CRCs) or BTI, so no `-mbranch-protection` flag is
needed; both kernels have PAC on.

## Files

| File | What it is |
|---|---|
| `earc_decode.h` | Freestanding AArch64 decoder and derivations, shared by the module and the test |
| `earc_route.c` | The module: parameter checks, decode, audit (`mode=0`), session (`mode=7`) |
| `earc_hdmi.h` | The HDMI passthrough audit's derivations and its break-hook list walk, shared by the module and the test |
| `earc_probe.h` | The session's pure decision logic (break-hook decision, `_set_output` policies, gate-restore rule, the two DTS-HD probes, refusal), shared by the module and the test |
| `build.sh`, `build.py` | Container build plus ABI, import and export checks against kernel captures |
| `earc-route-load.sh` | TV-side loader (root, busybox sh + python3) |
| `test/run.sh`, `test/test_decode.c` | Host test: sha-keyed oracles, refusal tests, C5/G5 and 5.4/6.12 differences |
| `test/loader/run.sh` | Host test of `earc-route-load.sh`'s kernel check (module chosen from the running kernel's stock modules, refusals), with generated stand-in modules and stubbed commands |

## What is derived, and from where

Each analysed function is decoded whole, from its kallsyms address to the next
symbol. A dataflow pass tracks what every register holds. Each value below must
come from one exact pattern; anything else refuses.

| Value | Derived from |
|---|---|
| NONPCM, HAL_A | `audio_hal_earc_set_output_format`: the only static word argument 0 is stored to; HAL_A is that store's base register |
| rate, HAL_B | same function: the only static word argument 1 is stored to; HAL_B is that store's base |
| CS0, CS2 | NONPCM-12 and NONPCM-4, only if that function accesses both words through the HAL_A base; CS2 also has to be the word `audio_hal_earc_set_mute` sets or clears bit 18 in |
| mode select | a word that function loads from HAL_A+0 |
| channel count | `audio_hal_earc_set_channel`: the only static word argument 0 is stored to; must be HAL_B |
| acmod | same function: argument 1's word, through the HAL_B base. The function must also read the rate and NONPCM words |
| control | `audio_hal_earc_set_audio_mode`: the word stored as `(word & ~7) \| mode` through HAL_B |
| block-manager mutex | `bm_connect_block` and `bm_disconnect_block`: the one static mutex they lock and unlock around their `_locked` call, arguments passed through. Both must use the same mutex |
| clock table, stride, entry 3 | `aud_clock_update_output`: the mutex is table + index * stride + offset (the stride from the index scaling: `3 * idx << 5` on 5.4, `ubfiz`/`sub`/`lsl` = `idx * 0x70` on 6.12). The table is where the one pair of byte loads at entry+8/+9 followed by an 8-byte load at entry+16 starts; the mutex offset (0x38 on 5.4, 0x48 on 6.12) must be an aligned 32-byte field after +0x18 inside the entry. Also requires the index <= 3 check |
| clock object fields | same function: the object at entry+16; its rate is the one word argument 1 is stored to (+12 on 5.4, +8 on 6.12); if argument 2 is also stored as a word (+12 on 6.12) the function takes a channel count and the flag is argument 3, else argument 2; the flag is the one byte store (+16 on both). The control at object+128; the callback is the one load from it that is called with argument 1 as its argument 0 (+32 on 5.4, +40 on 6.12), and with a channel count also one distinct load called with argument 2 (+48 on 6.12) |

Struct offsets the route uses are checked the same way. The connection walk
(+48 list, +16 peer, +32 owner, +40 port id, +88 block-info) is checked in
`_bm_disconnect_block_locked`, through its `_bm_get_block_port(_bm_find_block_by_block_info(info), port, 1)`
call. `bm_get_block_info_by_index` must return +88. The renderer's +76/+77 are
checked in `renderer_get_pcm_bypass` and `renderer_get_uid_pcm_bypass_head`. The
output's +44 == 3, +76 rate, +84 and +129 eARC-on are checked in `output_set_earc_on`;
when the clock function takes a channel count, that function must also pass it
output+72. Its calls also confirm `fn_aud_clock_update_output` and `fn_audio_hal_earc_set_on`.
`aud_hal_set_arc_channel`, which the 6.12 clock function calls, is a stub
(`mov w0, #0; ret`) on every 6.12 kernel measured.

Still fixed in the source, because no code pattern carries them: LG's semantic
constants (output and clock index 3, port 0x100, the 0x50020020 HD command and
its format block, HAL mode values 6/7 and the CS0 bits). Also the renderer's
+44/+48/+52 and the output's +68/+72/+80 fields, which are read but not
derivable from the analysed functions. `aimc_send_command` and
`aud_hal_set_arc_sample_rate` are trusted from kallsyms alone. The callback is
only compared, so a wrong value refuses the route.

## HDMI passthrough audit (derived, read-only)

`earc_hdmi.h` derives what an HDMI-input -> eARC passthrough feature would later
need, for the audit to print. `_cb_output_status` goes through the engine above.
`_set_output` (larger than `ED_MAX_INSNS`), the ADEC table (its index is a value
loaded from a control struct, which the engine's argument-scaled value cannot
hold) and the break-hook functions go through a small tracker that is sound per
basic block: it forgets every register at each branch target, and declines a
function with an indirect branch or a branch that leaves it. Each value below
comes from one exact pattern; none, or two candidates, is "not available".

The loader passes the HDMI functions like the others. One rule validates them
(`earc_hdmi_valid`, used by the module and the test): aligned, inside
`[_stext, _etext)`, and for analysed ones a non-zero multiple-of-4 size that
does not pass `_etext` and is at most 16 KiB.

| Value | Derived from |
|---|---|
| DTS gate and its state | `aud_hal_dts_supported`'s two words: stock (`mov w0,#0; ret`), switched (`mov w0,#1; ret`: another tool is active), a breakpoint (another tool is probing it), or unknown |
| `_set_output` kprobe site, mask offset | its first word (a kprobe can sit on it; any `brk` there is reported as another tool's probe), and the offset of the only 32-bit load from a copy of argument 1 whose loaded register is compared with an immediate (0x14). That 0x20 in that word means eARC is fixed in the source |
| ADEC table base, stride, codec/open/started | `alsa_decoder_codec`: `slot = constant + index * stride` from `sbfiz x,xI,#1,#32; add x,x,wI,sxtw; add xS,<constant>,x,lsl #5` (stride 96), the index being the value `cmp wI,#1; b.hi` checked in the same block (`b.gt`, signed, on 5.4.268-329.ptl4tv.2: it too accepts indexes 0 and 1 and rejects every index above them); codec is the only 32-bit store through the slot, open the only byte offset loaded through it. `alsa_decoder_start` must form the same base and stride and load exactly two slot bytes, open and started. ADEC 0 = base, ADEC 1 = base + stride |
| `_cb_output_status` DTS-HD sites | the event codec is the word loaded from argument 0. Exactly one compare of it with an immediate is the acceptance: shape 1 (5.4) `cmp codec,#4; b.ls` to the 32-bit store of the codec, shape 2 (6.12) the last compare of the chain `cmp #2; b.eq; b.ls; cmp #3; b.eq; cmp #4; b.ne`, whose `b.ne` falls through into that store. The store is the supported branch; the one `ldr x,[constant, codec, lsl #3]` is the name load and gives the table. The output register (x19 on every kernel measured) is the base register of that store, taken only if the engine shows it holding argument 2 at the compare |
| break-hook list head, hook fields | `register_kernel_break_hook`: its only pre-indexed 64-bit load from a constant base. `call_break_hook`, which must compute the same head: its one `blr`, the one `ldr xN,[xN]` (the node), the one 64-bit load from the node into the `blr` register (`fn`, +16), and, of the node's two 16-bit loads, the one `cmp x, w, uxth` compares (immediate, +24) and the other (mask, +26) |
| kprobe handlers, service symbols | by name: `kprobe_breakpoint_handler`, `kprobe_breakpoint_ss_handler`, `kretprobe_breakpoint_handler` (6.12 only), `aarch64_insn_patch_text`, `cpus_read_{lock,unlock}`, `[un]register_kprobe`, `[un]register_kernel_break_hook`. In the kernels' ksymtab: `[un]register_kprobe`, `cpus_read_{lock,unlock}`; not exported: the patch function and the break-hook pair |
| non-PCM word, physical | `struct earc_layout.nonpcm` minus `_stext & ~0xfffff`: the image is loaded at physical 0 plus its text offset (`/proc/iomem` on the C5: `00088000-0197ffff : Kernel code`) |

Any `brk #4` (what a kprobe plants) inside `_cb_output_status` is reported with
its offset as another tool's probe, before the pattern is judged; `brk #0x800`
(`BUG()`) is not.

Live reads, all through derived addresses inside the 64 MiB after `_etext`,
nothing written: ADEC 0's and ADEC 1's codec, open and started; the name table's entries and
strings; and the break-hook list. That walk starts at the head and reads, for at
most 32 nodes, only `next`, `fn` and the word after `fn`, and only from nodes
inside the kernel image: those are static objects that are never freed, and
removing a node rewrites one `next` pointer, so no RCU is needed. It stops at
the first node outside the image (another module's) without reading it. It
prints each image node and whether the walk was complete and met the kprobe
handlers, which is how the audit says whether they are registered. The stock
hook objects are not derived: `arch_init_kprobes`, which names them, is init
text and is freed after boot.

lg-g5-dts (G5 33.30.80) reads the non-PCM word at physical 0x264d5bc; on G5
33.31.x it is 0x264d5c4. Its ADEC 1 addresses are 8 below ours as well: the
data moved by 8 between those firmwares, it is the same word.

## The session load (`mode=7`)

A port of lg-g5-dts's DTS-from-HDMI path onto the addresses our decoders derive,
so it needs no per-TV constants. lg-g5-dts is pinned to one G5 image; its
conflict rule, self-test, DTS-core policy and gate patch are reused, its
hardcoded addresses replaced by the derived set and the audit's break-hook walk.
`earc_probe.h` holds the pure decisions (shared with the test); `earc_route.c`
holds the kernel ABI and the live calls. The load first runs the same HDMI
derivation the audit runs and **refuses** (never degrades) unless every item it
depends on derived cleanly, the DTS check is stock or already returns 1 (see
below), no probe-relevant site holds a breakpoint, and `_set_output`'s entry is
kprobe-placeable (`earc_mode_refusal`). It also needs `earc_hd_refusal`: the
`_cb_output_status` sites derived with no breakpoint in the function, the ADEC
table, the output register, no SP/ZR in the probed instructions, both
kprobe-placeable, and a live name table with more entries than the bound.

**Kernels that already have the change.** Some stock kernels ship what the
session would patch or probe in. It then leaves that as it is, instead of
refusing:

| Stock kernel | DTS check | `_cb_output_status` | Mode 7 places |
|---|---|---|---|
| C2/CS/G2, C5, G5 (5.4 and 6.12) | `mov w0,#0` | `cmp wC,#4`, 6 names | gate patch, both status probes, `_set_output` probe |
| 2026 OLED (`W26O` 43.11.81, 43.21.98) | `mov w0,#1` | `cmp wC,#4`, 6 names | both status probes, `_set_output` probe |
| C3/G3 (`W23O` 33.31.68), G4 (`W24O` 23.23.30) | `mov w0,#1` | `cmp wC,#9`, 11 names | `_set_output` probe only |

A check that already returns 1 is neither patched nor restored (`gate_open=1`,
`gate_opened_by_us=0`); mode 7 re-reads it before arming and refuses if it no
longer returns 1. The words are the same as another tool's patch would leave,
so this relies on the loader refusing beside the known DTS modules. A compare
of 7 or more with codec 7 named inside the table needs neither status probe
(`hd_probes_registered=0`): LG's code accepts and names codec 7 itself.
Host-tested on the images above; **not run on any of these TVs.** The loader has no release allowlist (see Load), so these kernels are
checked like any other. LG documents native DTS on C3/G3 and C4/G4, so try
them stock first.

**DTS-HD.** lg-g5-dts has two modules built from one source:
`tools/dts-core-route` (the `_set_output` probe, ADEC codec 48) and
`tools/dts-hd-route`, which is the same file with the policy codec changed to 51
and two more kprobes in `_cb_output_status` (`status_hooks.h`). Its DTS-core
sessions never have those two; its DTS-HD session loads `dts_gate.ko`, then the
HD module with all three probes. The two modules apply the same override
(`regs[2] = 0`, `regs[3] = 1`) and differ on the `_set_output` path only in the
policy codec, so one load has the gate, the two status probes and the
`_set_output` override on ADEC codec 48 or 51. It serves a DTS core session as
well as a DTS-HD one: a core session then runs with the name probe in place, and
a core-to-HD track change never passes through an open gate without it. The
compare probe still acts only for event codec 7 while an ADEC slot is started on
codec 51 (which slot: *Decoder 0 and decoder 1* below).

LG's output-status event for DTS-HD carries codec 7. `_cb_output_status` only
runs past its entry for output types 2 and 3 (optical and eARC; any other type
returns -1 before the compare), accepts codecs up to the bound (4), turns
anything else into codec 0 (PCM signalling), and on both paths loads the codec's
log name from a 6-entry table, so codec 7 already reads past that table in
stock code once the gate lets a DTS-HD stream start. The two probes:

- *compare* (`hd-compare`, `cmp wC,#4`): for event codec 7 on an output of type
  3 while the ADEC slot it reads is started on codec 51, continue at the supported branch
  (`hd-supported`, the `str wC,[xOutput,#68]`) with the codec register untouched
  and the compare skipped, as its `b.ls` (5.4) or the fall-through of its `b.ne`
  (6.12) would. Unlike lg-g5-dts, which leaves the flags of an earlier compare,
  the flags are set to those of an accepting compare (Z and C).
- *name load* (`name-load`, `ldr xN,[table,xI,lsl #3]`): for an index outside
  the table, put the table's own last entry in `xN` and skip the load. The
  table size is the live count the audit prints (`entries=6`), not a constant,
  and the substitute is read once at load from `table[entries-1]`, an entry the
  count has already checked as a pointer to a string in the kernel's data
  (`"???"` on all 13 saved kernels of both generations). lg-g5-dts substitutes
  its own strings (`dts-hd`, `unknown`); those would be module memory, which
  LG's log call reads after the pre-handler has returned, so a task preempted
  there across an unload would read freed memory. The `dts-hd` label in LG's
  log is given up for that.

Both are needed because accepting codec 7 is what sends it to the name load on
the supported path. The open gate is what lets a codec-7 event reach the name
load at all, so the name probe is registered before the gate is
opened and removed after it is restored. Every override lasts until
`rmmod earc_route`; the app lease lasts while its `/dev/earc_route` is open.

**Decoder 0 and decoder 1.** These TVs have two HDMI audio decoders: `adec.p0`,
with the DSP instance `dsp0/hdmi0`, and `adec.p1`, with `dsp0/hdmi1`. On a C5 an
HDMI input keeps the decoder it used last, so a source can play on either one.
lg-g5-dts, and this module before, overrode decoder 1 only.

`_set_output`'s first argument is the main-audio decoder. `_connect_output` and
`_connect_output_all` compute it with `common_input_convert_index`: ADEC port N
(0 to 3) stays N, and AMIXER port N becomes N + 4. In its bypass branch the
function connects renderer `index` to the mixer (`bm_connect_block_reserved_out`)
and calls `decoder_set_output_mode(index, 1, 1)`. That function takes only 0 or
1 and indexes the same ADEC table, so for 0 and 1 the ADEC slot, the renderer and
the decoder are one number. The kernel builds the compressed path the same way
for both decoders: `input_set_type`, `decoder_preconnect`,
`renderer_preconnect` and `renderer_create_bypass_pipeline` check no index. The
bypass gain block the scripts unmute belongs to mixer 0 and serves both
decoders. All of this comes from the C5 33.31.68 disassembly and has not been
seen on a TV.

- *Policy*: index 0 or 1, read through that decoder's slot.
  `earc_adec_slot` gives the slot of index 0 or 1 and nothing for any other
  index, so a mixer input (4 to 12) never reads past the two derived slots.
  The mask, codec, open and started conditions are unchanged.
- *Matched decoder:* each policy match records its index (an atomic word). The
  DTS-HD acceptance then reads only that decoder's slot. Before the first
  match, either slot started on codec 51 accepts. Nothing guarantees that the first match comes before a
  status event. After a match the other slot is ignored, because a decoder can
  stay open on an earlier input. `_cb_output_status`'s DTS-HD path itself reads
  no ADEC state; the ADEC condition comes from lg-g5-dts. The name-load probe
  does not depend on the decoder.
- *Prints:* `pass match index=N ... adecN-codec=`, `pass nomatch index=N ...
  adecN-codec= open= started=` (`no-adec-slot` for an index above 1),
  `hd status ... matched=M adec0-codec= started= adec1-codec= started=
  decoder=D: ...` and `hd name ... matched=M`, where M is the recorded decoder
  (-1 before a match) and D the decoder accepted for (-1 for stock).

**Stage a (break handlers + self-test).** On both generations a plain pre-handler
kprobe needs the BRK #4 (trap) and #6 (single-step) handlers; the BRK-based
single-step is upstream's on both (v5.4.268 `arch_prepare_ss_slot` ends the
slot with `BRK64_OPCODE_KPROBES_SS`), not an LG change. It never reaches
the kretprobe trampoline (#7, 6.12 only), so that hook is not registered. The
module walks the kernel's break-hook list: if both handlers are already on it, it
does nothing; if the walk is incomplete, or a node would also answer #4 or #6
(`(want & ~node_mask) == node_imm`), or the immediate/mask were not derived, it
refuses; otherwise it registers module-owned hooks (mask 0) pointing at the
kernel's own handlers, through `register_kernel_break_hook`'s kallsyms address.
The self-test then probes a leaf (`add w0,w0,#1`), whose pre-handler sets x0 to 10
and expects 11 with exactly one hit; `register_kprobe` on a core-text function of
the still-initialising module succeeds (the module is `MODULE_STATE_COMING`,
`try_module_get` succeeds, the function is not in the init section). The leaf stays a named symbol: on 6.12 `register_kprobe`
returns `-ENOENT` unless kallsyms resolves the probed address. The session
then runs lg-g5-dts's second pass on the same leaf, since their probes redirect
`pc`: the pre-handler sets x0 to 10, adds 4 to `pc` and returns 1, and the call
must come back as 10 with one hit (the leaf is first checked to be exactly
`add w0,w0,#1; ret`). `kprobe_breakpoint_handler` of every capture resumes at
`regs->pc` without single-stepping when the pre-handler returns non-zero.

Stage a is needed because, on 5.4, LG's `init_kprobes` begins with a vendor test
of a flag that only a `kprobe` kernel command-line parameter sets and returns
before registering anything (verified in three 5.4 captures; the live C5's command
line has no such parameter), so every 5.4 TV lacks the two break hooks while
`register_kprobe` itself works. On 6.12 `init_kprobes` runs ungated and the kernel
registers the #4, #6 and #7 hooks, so stage a should find them already present
(`already`); not confirmed on a TV.

**Kernel writes and their undo.** Nothing else writes kernel memory.

| Write | Made by | Undo |
|---|---|---|
| break-hook list: add our #4 and #6 nodes | stage a (only if not already present) | `unregister_kernel_break_hook` (#6 then #4) |
| one kprobe on `_set_output` | `register_kprobe` | `unregister_kprobe` |
| one kprobe on the `_cb_output_status` name load (before the gate is opened) | `register_kprobe` | `unregister_kprobe`, after the gate is restored |
| one kprobe on the `_cb_output_status` compare (after the gate is opened) | `register_kprobe` | `unregister_kprobe`, before the gate is restored |
| one kprobe on the self-test leaf (twice) | `register_kprobe` | `unregister_kprobe`, within init |
| one instruction: DTS check word 0 `mov w0,#0` -> `mov w0,#1` (not when it is already `mov w0,#1`) | `aarch64_insn_patch_text` under `cpus_read_lock` | patch back to `mov w0,#0`, but only if the live word is still the one we wrote |

**Failure paths** all go through `passthrough_cleanup`, which `cleanup_module` also runs. On a kernel that already has the change, it skips the gate patch or the two `_cb_output_status` probes (`hd_probes_registered=0`).

**New imports** (called directly; build.py proves each is exported by every kernel
of the generation): `register_kprobe`, `unregister_kprobe`, `cpus_read_lock`,
`cpus_read_unlock`. `register_kernel_break_hook`,
`unregister_kernel_break_hook` and `aarch64_insn_patch_text` are not exported and
are called through their kallsyms addresses.

**Hand-declared kernel structs** (`earc_route.c`, with static assertions). `struct
pt_regs` (`regs[31]` at +0, `pc` +256, `pstate` +264: the `user_pt_regs` prefix;
`kprobe_breakpoint_handler` of every capture reads `[regs,#256]` and
`[regs,#264]`; the probes touch x2/x3, the derived registers, `pc` and the NZCV
bits of `pstate`) and `struct break_hook` (`fn` +16,
`imm` +24, `mask` +26, 32 bytes) are identical on both generations. `struct
kprobe` differs: `addr` +0x28, `pre_handler` +0x40 on both; its size is 0x88 on
5.4 (flags +0x80) and 0x80 on 6.12 (flags +0x78, after `fault_handler` was
dropped). The kernel touches nothing past `flags`, so that is the full size; we
zero the struct and set only `addr` and `pre_handler`. The sizes are
`-D` macros from `build.py`; the `_Static_assert`s, compiled per generation, are
the layout check (the captures hold no kprobe instance to measure directly).

**6.12 (host-tested only).** `aarch64_insn_patch_text` has the same signature
(`int(void **addrs, u32 *insns, int cnt)`), the pre-handler contract is the same
(return non-zero to skip single-stepping; we return 0), the break-hook layout and
the registration/patch call conventions match 5.4. Nothing refuses on 6.12 for a
generation reason. Only `struct kprobe`'s size differs, and that is handled.
The probes work on both shapes of `_cb_output_status` by the same rule
(continue at the derived supported branch): on 5.4 that is the `b.ls` target, on
6.12 the instruction after the `b.ne`. The host test derives the sites and the
registers (codec w6, output x19, name x7 from index x4, 6 entries) on all five
6.12 kernels.

The pre-handler runs in breakpoint context: it calls no kernel function but `printk` (first 5 matches, first 8 non-matches), takes no
lock, sleeps not, reads only the load-time bounds-checked ADEC addresses and the
caller's descriptor (null-checked), updates atomics, and rewrites x2/x3 for a match. The two `_cb_output_status` pre-handlers obey the
same contract: they read the probed instruction's own registers, the output's
type word through the output pointer the function itself has just read through
(null-checked), and both ADEC slots' codec and started flags; they print the first 8
stock and first 5 acting hits each (`earc_route: hd status codec=... `,
`earc_route: hd name codec=...`), and write `pc`, the NZCV flags and
the name register. The name written there is a pointer into the kernel image
(the table's last entry), because LG's log call uses it after the pre-handler
has returned. We never probe a pre-handler itself (only `_set_output`, the two
`_cb_output_status` instructions and the self-test leaf), so no probe recursion
is possible; a `NOKPROBE_SYMBOL` marker,
which lg-g5-dts uses, is unnecessary here and unavailable freestanding.

## Build

```sh
webos25/kmod/build.sh CAPTURE_DIR...           # debian:12-slim, GCC 12.2
EARC_BUILD_IMAGE=c5re-tools:latest webos25/kmod/build.sh CAPTURE_DIR...
```

Each capture directory holds `kernel.bin`, `kallsyms.txt` and `stock-module.ko`
from one TV. They are private LG files: keep them outside the repository. The
generation of a capture is its stock module's vermagic; `out/earc_route-5.4.ko`
and `out/earc_route-6.12.ko` are built for the generations present. `kallsyms.txt`
must be what `/proc/kallsyms` shows on the TV. The 6.12 survey captures were
0x10000 too high (a RELA addend proves it; `_stext` is `0xffffffc080010000`, file
offset 0x10000, link base `_text` `0xffffffc080000000`), so they were corrected by
subtracting 0x10000; the originals are kept as `kallsyms.txt.survey`. The build
refuses unless, per generation:

| | 5.4.268 | 6.12.44 |
|---|---|---|
| vermagic | `5.4.268 SMP preempt mod_unload aarch64` | `6.12.44 SMP preempt mod_unload aarch64` |
| `struct module` size, init, exit offsets | 0x380, 0x150, 0x338 | 0x4c0, 0x138, 0x480 |
| `struct kprobe` size, flags offset | 0x88, 0x80 | 0x80, 0x78 |
| `printk` import | `printk` | `_printk` |

- every stock module of the generation has that vermagic and `struct module`
  layout, and the built module's layout is identical (name at offset 24 in both);
- the built module's `struct kprobe`, `struct pt_regs` and `struct break_hook`
  `_Static_assert`s hold with the generation's `-D` macros (the layout check the
  captures allow: they hold no kprobe instance to measure directly);
- GCC's optimised tree has no `__builtin_unreachable` or `__builtin_trap`;
- the imports are only the printk symbol above, `mutex_lock`/`mutex_unlock`,
  `msleep`, the kthread calls, `param_{set,get}_{uint,ulong}` and, for the active
  modes, `register_kprobe`, `unregister_kprobe`, `cpus_read_lock`,
  `cpus_read_unlock`; each one is in every kernel's PREL32
  ksymtab (`struct kernel_symbol` is `{value, name, namespace}` offsets on both)
  at its kallsyms address, the name spelled exactly (6.12 merges `mutex_lock`
  into the tail of `ww_mutex_lock`). The image base for that proof is
  `(_stext & ~0xfffff) + text_offset` from the Image header:
  `0xffffffc010088000` on 5.4, `0xffffffc080000000` on 6.12.

## Test

```sh
sh webos25/kmod/test/fetch.sh        # once: firmwares in test/kernels.tsv -> test/kernels/ (git-ignored)
sh webos25/kmod/test/run.sh          # every capture in test/kernels/, plus any EARC_*_DIR
sh webos25/kmod/test/loader/run.sh   # the loader's kernel check; needs no captures
```

`kernels.tsv` lists one firmware per kernel build the archive has for webOS 23
to 26, with its zip's sha256. `run.sh` also runs the shipped loader on each
capture's own stock module and kallsyms: a kernel the boot script accepts must
get its generation's `.ko` and every symbol. Every directory is passed to the
decode test with the sha256 of its `kernel.bin`
(`--kernel SHA256 DIR`); the four variables are equivalent apart from naming.
Without captures it prints `SKIP` and passes. With them, the test applies the
oracle table keyed by that sha256, so an oracle never meets another image:

- 5.4: C5 33.31.68 (hand-derived) and G5 33.31.69 (Glasshouse's values). There
  is no eARC oracle row for a C5 33.31.69, or for any kernel 5.4.268-329.ptl4tv.2
  (33.22.x) image: that one decodes and prints its eARC route values with no
  oracle, and has an HDMI oracle row only (see below). A kernel never run on a
  TV, ptl4tv.2 included, is checked by the host test alone;
- 6.12: C5 43.21.73 and 43.21.77, G5 43.21.71 and 43.21.77, G2 43.21.74, each
  derived from its corrected `kallsyms.txt` and disassembly;
- every other image is decoded and printed without an oracle.

For each oracle kernel it asserts every derived value, including the clock
layout and the image link base. On the first oracle kernel of each generation
it runs refusal tests that mutate one instruction (or the loader's
`mutex_lock` address) and require a specific refusal, including an
`output_set_earc_on` whose clock-call argument is unknown and never loaded
from the output (the rate on 5.4, the channel count on 6.12: both sides
unknown must not match); on 6.12 also: the channel callback load removed, the
rate/channel store removed, moved onto the flag, or moved so the channel count
reaches +0x80.
Removing the `sub` of the 6.12 index scaling is not a refusal: the stride, the
mutex and the entry loads then agree on 0x80, so the test asserts that the
decoder follows the code to 0x80. It also checks that C5 and G5 differ exactly
where expected in each generation (rate +104/+96, acmod +108/+100, NONPCM
+68/+60) and that a family's 5.4 and 6.12 kernels differ exactly in the clock
layout (stride 0x60/0x70, mutex +0x38/+0x48, callback +32/+40, object rate
+12/+8, channel count none/+12) and in no HAL offset.

The HDMI set has its own sha-keyed oracle, asserted whenever its image is given,
with or without an eARC oracle for it: the kernels above plus the tester's
CS kernel (G2 33.31.68, whose values are also the ones its `.ko` files use) and
G2 33.22.95 (kernel 5.4.268-329.ptl4tv.2, derived by hand from its disassembly).
The other rows were checked against an independent static derivation. Each row
also holds ADEC 0's three addresses: the slot array's own base plus 8, 10 and 20,
which equal ADEC 1's minus the 96-byte stride in every row (C5 33.31.68: codec
`...a24c`, open `...a240`, started `...a242`). `_set_output`
of every capture must pass the validity rule with its kallsyms size (2308 to
2544 bytes). The break-hook walk runs on each image (the list is empty before
boot) and on lists built in test memory: complete with both kprobe handlers,
leaving the image, longer than the cap, a misaligned `next`. Refusal tests, on
the first oracle kernel of each generation, each require their own reason: the
gate changed to a third value; `brk #4` on the gate, at the `_set_output` entry
and at both probed `_cb_output_status` sites; `brk #0x800` there not counted;
the mask load from a non-argument register; a second mask candidate; the ADEC
scaling changed (in either function), its bound check removed or changed to
`b.ge` (a signed `b.gt` must still derive the same ADEC 1), the codec store
removed, a second store through the slot; the acceptance branch condition
changed, a second acceptance compare, the name load removed; the list-head load
removed.

The active-mode decision logic (`earc_probe.h`) is tested off-TV through the same
code the module runs: the break-hook decision on synthetic lists walked by
`earc_hdmi_walk_list` (already registered, register, a node answering #4 or #6,
immediate/mask unknown, an incomplete walk); the HDMI and app-lease
`_set_output` policies over a table of cases;
`earc_adec_slot` (slot 0 and slot 1 for index 0 and 1, nothing for any other
index); the DTS-HD slot choice (only the matched decoder's slot after a match,
either slot before one, never a slot above 1); the gate "restore only what we wrote" rule; and `earc_mode_refusal`
when an item the session depends on is missing.

The DTS-HD decisions are tested on every kernel given, oracle or not: the sites
and registers derive and the session accepts the stock image; the probed words
are `cmp wC,#bound`, `str wC,[xOutput,..]` and the name load; the acceptance
acts for codec 7 on the eARC output with ADEC 1 started on 51 and not for the
bound, codecs 5 and 8, the optical output, ADEC on 48 or stopped; the probe
registers are codec w6, output x19, name index x4 and name destination x7; the
name decision substitutes for 7 and the first index past that kernel's table
and keeps every index inside it; the substitute, `table[entries-1]`, is the
in-image string `"???"`. `earc_hd_refusal` has one test per reason on a
synthetic layout (a breakpoint, the compare not derived, ADEC not derived, a
bound of 7, no output register, a load into xzr, a PC-relative instruction at
the compare and at the name load, a table of 4 entries or none), and on the first oracle kernel of each generation three
mutations of the real function (breakpoints at both sites, the name load
removed, `mov x19,x2` changed so the store's base is not argument 2).

## Adding a kernel

The loader does not list releases: it checks the running kernel (see Load), so
a new release of a known generation needs no change to load. These steps add
it to the host test, which is what qualifies a kernel before a TV run:

1. Read the user's release from `uname -r`, or from `KERNEL_RELEASE=` in the
   output of `webos25/restore/report-tv.sh` (report version 5 and later).
2. Find firmwares on that release in the firmware archive index,
   `https://lg.slada.sk/processed_fw.json`: each EPK entry has a `kernelVersion`
   field. Match the user's board by `firmwareotaID`, not by the version string.
3. Download from `https://tv.slada.sk/<relativePath>` and check the file against
   the entry's `zipFileSha256`.
4. Extract the EPK with epk2extract (steps in
   [`add-tv-generation`](../../.claude/skills/add-tv-generation/SKILL.md)).
5. The kernel is `/boot/Image.lz4-<release>` in the rootfs. On 33.22.x the EPK has
   no `kernel.pak`, so the rootfs is the only place. LG's LZ4P container needs
   unpacking before it is a raw arm64 Image (c5-native's `unlz4p.py`, in a private
   repository).
6. Make the symbol table with `kallsyms-finder` from vmlinux-to-elf. It must be
   what `/proc/kallsyms` shows on the TV (see Build for the 6.12 offset).
7. The stock module is `/lib/modules/<release>/kernel/aspectratiodrv.ko` in the
   rootfs (`/usr/lib/modules/<release>/...` on 6.12).
8. Put the files in a capture directory: `kernel.bin`, `kallsyms.txt`,
   `stock-module.ko`, and a `NOTES.txt` of your own (firmware version, source
   file, hashes; no script reads it). `build.sh` and `test/run.sh` need only the
   first three. Keep the directory outside the repository.
9. (No loader change.)
10. Run `webos25/kmod/build.sh CAPTURE_DIR...` (it runs `build.py`'s ABI, import
    and export checks against the capture).
11. Add the image to `test/test_decode.c`: an oracle row for the eARC values and an
    HDMI oracle row, derived independently of the decoder, keyed by the
    `kernel.bin` sha256. Run `sh test/run.sh` with the directory in
    `EARC_EXTRA_DIRS` and check that the refusal tests still fail on a mutated
    instruction.

Host tests passing is not a TV run. Say so in the Status paragraph until someone
has loaded the module on that kernel.

## Load (on the TV, as root)

```sh
sh earc-route-load.sh                 # audit: decode, print to dmesg, unload
sh earc-route-load.sh --dts-hd        # the session, until rmmod earc_route
```

`--dts-hd` refuses, before `insmod`, if another DTS tool is loaded
(`dts_gate`, `dts_core_route`, `gh_dts_native`) or `/var/lib/lg-dts-core/enabled`
exists; the audit only reports those. **No 6.12 TV has run it; no session has
run on decoder 0.** Read-only parameters report the state: `gate_open`,
`gate_opened_by_us`, `hooks_registered`, `probe_registered`, `selftest_passed`,
`calls_seen`, `calls_matched`, `overrides`, `app_overrides`,
`hd_probes_registered` (2 when both are in), `hd_status_seen`,
`hd_status_accepts`, `hd_name_seen`, `hd_name_fixes`.

`/dev/earc_route` is the app lease: an app opens it and writes its renderer
index plus 1 (`1` or `2`) once. While that file is open, `_set_output` for that
renderer on eARC is forced to LG's bypass; the kernel drops the lease when the
file is closed, by the app or by its crash or kill. One holder at a time
(`EBUSY`), and the module cannot be unloaded while it is held. Its handler table
is laid out from the kernel's own calls (`misc_open`, `__fput`, and
`__vfs_write` on 5.4 or `vfs_write` on 6.12), and the module refuses to load
when they do not match. `app_route` (read-only) shows the current holder.

In dmesg, the session prints `earc_route: pass nomatch ...` for the first 8 `_set_output` calls that miss the policy, with every input of the decision: `index`, `desc` (0 = null descriptor, `mask` then shown as 0), `mask`, that decoder's `adec<index>-codec`/`open`/`started` (`no-adec-slot` for an index above 1, which has no ADEC slot), and the call's `codec-arg`/`bypass-arg`.

`earc_route-5.4.ko` and `earc_route-6.12.ko` sit next to the script. There is
no release allowlist: the loader reads the running kernel's own modules under
`/lib/modules/$(uname -r)` and `/usr/lib/modules/$(uname -r)` (up to eight,
sorted) and loads the one `earc_route-*.ko` whose vermagic and `struct module`
layout (`.gnu.linkonce.this_module` size, `init_module`/`cleanup_module`
offsets) equal theirs. The builds are:

| module | vermagic | size/init/exit |
|---|---|---|
| `earc_route-5.4.ko` | `5.4.268 SMP preempt mod_unload aarch64` | 0x380/0x150/0x338 |
| `earc_route-6.12.ko` | `6.12.44 SMP preempt mod_unload aarch64` | 0x4c0/0x138/0x480 |

The loader refuses, before `insmod`, unless:

- the kernel has stock modules and they all agree on vermagic and layout;
- exactly one `earc_route-*.ko` has that vermagic and layout;
- every needed symbol is in `/proc/kallsyms` exactly once, in the core kernel,
  with type T/t (`_etext`: T/t/D/d) and a non-zero address.

The module then derives every LG address and offset from the kernel's code and
refuses itself on any mismatch. What neither checks: `struct kprobe`'s size and
`flags` offset are compiled per generation, measured on 5.4.268-329 and
6.12.44-352; a kernel of the same vermagic with a different kprobe layout is not
detected. The session's own kprobe self-tests run before it changes anything. `test/loader/run.sh` tests these rules with generated
stand-in modules.

The HDMI audit's symbols are optional: one that is missing is not passed and
only that part of the audit says so. Before loading, the loader prints one
`earc-route: other-tool:` line for each other DTS tool it sees, without
refusing: `/var/lib/lg-dts-core` (and whether its `enabled` file exists),
Glasshouse's `/var/lib/lg-dts-native`, and loaded modules `dts_gate`,
`dts_core_route` and `gh_dts_native`.

It then runs `insmod` once and prints one result line. The module re-checks
every address against `[_stext, _etext)`, compares `fn_mutex_lock` and
`fn_mutex_unlock` with the symbols the kernel resolved for it, and refuses a
derived data address that is misaligned or not within 64 MiB after `_etext`.

## HDMI input (`mode=7`)

On C2/CS, C5 and G5, LG's `input_set_hdmi` sends every DTS type to the HDMI
input's DSP instance as `codec=0` (PCM), and `alsa_decoder_start` then skips
`input_start` and `input_drop_data(0)` for a DTS ADEC codec, so the instance stays
dropped. C3/G3's kernel handles DTS there itself. Mode 7 makes the others do the same:

| Probe | Site | Acts when | Does |
|---|---|---|---|
| start | the `cmp w0,#1` after `bl input_get_hdmi_codec` in `alsa_decoder_start` | that decoder's ADEC slot is open on 48 or 51 and the input reads PCM | `w0 = 0`: LG starts the input and clears its drop |
| property | `aimc_set_property` entry | called from `input_set_hdmi` with `codec=%d` 0 or `mode=%d` 1, not forced PCM, for the input of a decoder open on 48 or 51 | sends C3/G3's values: type 4 codec 5 / mode 1, 202 codec 6 / mode 4, 203 codec 5 or 6 (by rate) / mode 5; the 1000+ types modes 6, 7, 8 |
| format change | the DSP's `hdmi_dts_format_changed` event, subscribed per decoder after the start probe acted | a DTS sub-type change (the DSP drops the input) | under LG's decoder lock: `input_set_hdmi`, `start` 0 then 1, `input_drop_data(0)`, as C3/G3's `input_hdmi_dts_format_changed_cb` |

Derived from LG's code: the start compare and its index register, the input
table and the decoder lock (`alsa_decoder_start`), `input_set_hdmi`'s frame, its
audio-info offset (`add x1,sp,#K` before `GetAudioInfo`), its input and forced-PCM
registers and format strings (read back at load), and the input's uid offset
(`input_drop_data`). Counters: `input_starts`, `input_properties`, `format_changes`.

**Verified:** host-tested on C5 5.4, G3 5.4 and C5 6.12, with mutation refusals. On
a C2 with an eARC receiver, writing those values by hand made a PS5's DTS core
play as DTS on the soundbar; the module loads with both probes and unloads clean.
**Not verified:** the probes acting on a live HDMI stream, and the format-change path.

## In the app

DTS Enabler uses a systemd oneshot to load the module while DTS-HD is on.
Turning DTS-HD off, Disable and Uninstall unload it; busy unloads are reported.
The exact tested C2 build uses app-only mode; other supported profiles retain
mode 7. No app setting offers DTS to HDMI devices; see [`../app/README.md`](../app/README.md).
