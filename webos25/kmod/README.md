# eARC route kernel module (webOS 25, kernels 5.4.268 and 6.12.44)

`earc_route-<generation>.ko` connects a renderer's PCM-bypass output to the TV's eARC output 3
with non-PCM signalling, so a DTS bitstream reaches an eARC receiver. It is a
port of [Glasshouse](https://github.com/rorygallagher2024/glasshouse)'s
`native/dts-forwarding` route (GPL-2.0-only; see [NOTICE](NOTICE)).

Glasshouse's module and the earlier C5 port are pinned to one kernel image: every
LG-private address is compiled in. This one is built once per kernel generation,
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

`mode=2` to `mode=5` are the *active* HDMI-passthrough stages, a port of
[lg-g5-dts](https://github.com/dakenf/lg-g5-dts) (GPL-2.0-only) onto our derived
addresses: `mode=2` self-tests the kprobe BRK dispatch, `mode=3` opens the DTS
check, `mode=4` observes `_set_output`, `mode=5` overrides the DTS-core route for
a bounded window. `mode=6` and `mode=7` are a port of
lg-g5-dts's `tools/dts-hd-route`: `mode=6` watches the two `_cb_output_status`
sites and changes nothing, `mode=7` is the session load for both DTS core and
DTS-HD. These modes write
to kernel memory (break-hook list, up to three kprobes, one patched instruction);
every write is listed with its undo under
[HDMI passthrough active modes](#hdmi-passthrough-active-modes-mode2). `mode=0`
and `mode=1` are unchanged, byte for byte.

Status: both builds host-tested against the kernels listed under Test.
Experimental, root over SSH, not shipped in the app or the release.

On 2026-11-11 a tester's LG CS (OLED55CS6LA, firmware 33.31.68, kernel
5.4.268-329.ptl4tv.5) with a Samsung HW-Q930F over eARC and a set-top box on
HDMI 1 ran `hdmi-session.sh --apply 30` (mode 7). The soundbar showed DTS for a
DTS core stream, DTS-HD for DTS-HD MA and DTS:X for DTS:X. It worked again after
a TV power cycle. The first run failed because the script gave up during the
input-service restart; it now waits for the input to come back (a wait loop,
fixed since). That is the only run with an eARC receiver. `hdmi-watch.sh` has run
only on the C5, which has no receiver.

On one C5 (33.31.68, kernel 5.4.268), also 2026-11-11: the audit, `mode=2`,
`mode=3` and `mode=4` ran and left the TV in its stock
state. With `mode=3` loaded the HDMI decoder started on a live DTS stream
(`UserCodec=DTS`, `Start=1`) and refused it again after unload. `mode=4`'s
`_set_output` pre-handler fired on every input-service restart (`calls_seen=1`
each time) and printed `pass nomatch index=1 desc=1 mask=0x2 adec1-codec=21
open=1 started=1`: that TV plays through its own speakers, with no eARC receiver,
so the policy never matched. `mode=6` loaded with nothing playing, passed both
self-tests (`result=11` and the redirect's `result=10`), placed its two probes and
unloaded stock; it saw no output-status event. `mode=7` ran once on a live DTS
core stream (`Codec=UNKNOWN_DTS`, 48 kHz): both self-tests, the check opened,
three probes placed, the decoder started on DTS after an input-service restart,
and the one `_set_output` call came before that start (`adec1-codec=0
started=0`), so nothing was overridden; no output-status event, `earc_nonpcm=0`,
unload left the TV stock.

Not run on any TV: `mode=5`; kernel 5.4.268-329.ptl4tv.2 (33.22.x), which is
host-tested only; the 6.12 build and kernel.

The module and the loader still print `UNTESTED ON HARDWARE` for modes 5, 6 and 7.
That message is out of date for mode 7, which has run on a C5 and, with a receiver,
on a CS. It will be reworded in a later build.
Neither `.ko` is committed (`out/` is ignored). Neither generation uses
MODVERSIONS (no `__versions`, no CRCs) or BTI, so no `-mbranch-protection` flag is
needed; both kernels have PAC on.

## Files

| File | What it is |
|---|---|
| `earc_decode.h` | Freestanding AArch64 decoder and derivations, shared by the module and the test |
| `earc_route.c` | The module: parameter checks, decode, audit (`mode=0`), route (`mode=1`), HDMI passthrough (`mode=2`..`7`) |
| `earc_hdmi.h` | The HDMI passthrough audit's derivations and its break-hook list walk, shared by the module and the test |
| `earc_probe.h` | The active modes' pure decision logic (break-hook decision, DTS-core policy, gate-restore rule, the two DTS-HD probes, per-mode refusal), shared by the module and the test |
| `build.sh`, `build.py` | Container build plus ABI, import and export checks against kernel captures |
| `earc-route-load.sh` | TV-side loader (root, busybox sh + python3) |
| `test/run.sh`, `test/test_decode.c` | Host test: sha-keyed oracles, refusal tests, C5/G5 and 5.4/6.12 differences |
| `hdmi-session.sh` | TV-side DTS core or DTS-HD session from an HDMI input (root, busybox sh): preflight, audio graph setup, restore |
| `test/session/run.sh`, `test/session/fixtures/` | Host test of `hdmi-session.sh` against a C5's own status files, with stubbed commands |
| `hdmi-watch.sh` | TV-side watcher: keeps the `hdmi-session.sh` route applied across seeks and input switches (root, busybox sh) |
| `test/watch/run.sh` | Host test of `hdmi-watch.sh` on a timeline of input states, using `test/session/fixtures/` |

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
| ADEC table base, stride, codec/open/started | `alsa_decoder_codec`: `slot = constant + index * stride` from `sbfiz x,xI,#1,#32; add x,x,wI,sxtw; add xS,<constant>,x,lsl #5` (stride 96), the index being the value `cmp wI,#1; b.hi` checked in the same block (`b.gt`, signed, on 5.4.268-329.ptl4tv.2: it too accepts index 1 and rejects every index above it); codec is the only 32-bit store through the slot, open the only byte offset loaded through it. `alsa_decoder_start` must form the same base and stride and load exactly two slot bytes, open and started. ADEC 1 = base + stride |
| `_cb_output_status` DTS-HD sites | the event codec is the word loaded from argument 0. Exactly one compare of it with an immediate is the acceptance: shape 1 (5.4) `cmp codec,#4; b.ls` to the 32-bit store of the codec, shape 2 (6.12) the last compare of the chain `cmp #2; b.eq; b.ls; cmp #3; b.eq; cmp #4; b.ne`, whose `b.ne` falls through into that store. The store is the supported branch; the one `ldr x,[constant, codec, lsl #3]` is the name load and gives the table. The output register (x19 on every kernel measured) is the base register of that store, taken only if the engine shows it holding argument 2 at the compare |
| break-hook list head, hook fields | `register_kernel_break_hook`: its only pre-indexed 64-bit load from a constant base. `call_break_hook`, which must compute the same head: its one `blr`, the one `ldr xN,[xN]` (the node), the one 64-bit load from the node into the `blr` register (`fn`, +16), and, of the node's two 16-bit loads, the one `cmp x, w, uxth` compares (immediate, +24) and the other (mask, +26) |
| kprobe handlers, service symbols | by name: `kprobe_breakpoint_handler`, `kprobe_breakpoint_ss_handler`, `kretprobe_breakpoint_handler` (6.12 only), `aarch64_insn_patch_text`, `cpus_read_{lock,unlock}`, `[un]register_kprobe`, `[un]register_kernel_break_hook`. In the kernels' ksymtab: `[un]register_kprobe`, `cpus_read_{lock,unlock}`; not exported: the patch function and the break-hook pair |
| non-PCM word, physical | `struct earc_layout.nonpcm` minus `_stext & ~0xfffff`: the image is loaded at physical 0 plus its text offset (`/proc/iomem` on the C5: `00088000-0197ffff : Kernel code`) |

Any `brk #4` (what a kprobe plants) inside `_cb_output_status` is reported with
its offset as another tool's probe, before the pattern is judged; `brk #0x800`
(`BUG()`) is not.

Live reads, all through derived addresses inside the 64 MiB after `_etext`,
nothing written: ADEC 1's codec, open and started; the name table's entries and
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

## HDMI passthrough active modes (`mode>=2`)

A port of lg-g5-dts's DTS-from-HDMI path onto the addresses our decoders derive,
so it needs no per-TV constants. lg-g5-dts is pinned to one G5 image; its
conflict rule, self-test, DTS-core policy and gate patch are reused, its
hardcoded addresses replaced by the derived set and the audit's break-hook walk.
`earc_probe.h` holds the pure decisions (shared with the test); `earc_route.c`
holds the kernel ABI and the live calls. Each mode first runs the same HDMI
derivation the audit runs and **refuses** (never degrades) unless every item it
depends on derived cleanly, the DTS check is stock (for modes that touch it), no
probe-relevant site holds a breakpoint, and `_set_output`'s entry is
kprobe-placeable (`earc_mode_refusal`). Modes 6 and 7 also need
`earc_hd_refusal`: the `_cb_output_status` sites derived with no breakpoint in
the function, the ADEC table, a compare bound below 7, the output register, no
SP/ZR in the probed instructions, both kprobe-placeable, and a live name table
with more entries than the bound.

| Mode | Loader | What it does | Stays loaded |
|---|---|---|---|
| 2 | `--selftest` | Registers the BRK #4/#6 hooks (or confirms the kernel's), self-tests a kprobe on a module leaf, undoes everything, returns `-ECANCELED` | no |
| 3 | `--open-gate` | Opens the DTS check (`aud_hal_dts_supported`: `mov w0,#0` -> `mov w0,#1`) | yes |
| 4 | `--observe` | Stage a + opens the gate + a counting kprobe on `_set_output` (no register change) | yes |
| 5 | `--dts-core` | Stage a + opens the gate + a kprobe on `_set_output` that overrides the DTS-core route for a bounded window | yes |
| 6 | `--observe-hd` | Stage a + the redirect self-test + counting kprobes on the `_cb_output_status` compare and name load. The gate stays shut; no register, flag or pc is changed | yes |
| 7 | `--dts-hd` | The session stage for DTS core and DTS-HD: stage a + the redirect self-test + the `_cb_output_status` name-load kprobe + opens the gate + the `_cb_output_status` compare kprobe (acts for DTS-HD only) + the `_set_output` kprobe overriding the route of a DTS core or DTS-HD stream (ADEC codec 48 or 51) for a bounded window | yes |

**Composition.** A real session needs the DTS check open **and** the `_set_output`
probe active from one load (a module loads once), so modes 4 and 5 include opening
the check. Mode 3 exists to test the check alone; mode 2 to test BRK dispatch
alone. Mode 5's `window_seconds` (default 30, max 120; 0 only with `supervised=1`)
bounds the override; after it, mode 5 observes only. The window is driven by the
module's existing watchdog-thread pattern (a 100 ms-tick kthread), not a new clock
import.

**DTS-HD (modes 6 and 7).** lg-g5-dts has two modules built from one source:
`tools/dts-core-route` (the `_set_output` probe, ADEC codec 48) and
`tools/dts-hd-route`, which is the same file with the policy codec changed to 51
and two more kprobes in `_cb_output_status` (`status_hooks.h`). Its DTS-core
sessions never have those two; its DTS-HD session loads `dts_gate.ko`, then the
HD module with all three probes. The two modules apply the same override
(`regs[2] = 0`, `regs[3] = 1`) and differ on the `_set_output` path only in the
policy codec, so one load of mode 7 has the gate, the two status probes and the
`_set_output` override on ADEC codec 48 or 51. It serves a DTS core session as
well as a DTS-HD one: a core session then runs with the name probe in place, and
a core-to-HD track change never passes through an open gate without it. The
compare probe still acts only for event codec 7 while ADEC 1 is started on codec
51. Mode 5 is unchanged: codec 48 only, no status probes.

LG's output-status event for DTS-HD carries codec 7. `_cb_output_status` only
runs past its entry for output types 2 and 3 (optical and eARC; any other type
returns -1 before the compare), accepts codecs up to the bound (4), turns
anything else into codec 0 (PCM signalling), and on both paths loads the codec's
log name from a 6-entry table, so codec 7 already reads past that table in
stock code once the gate lets a DTS-HD stream start. The two probes:

- *compare* (`hd-compare`, `cmp wC,#4`): for event codec 7 on an output of type
  3 while ADEC 1 is started on codec 51, continue at the supported branch
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
load at all, so in mode 7 the name probe is registered before the gate is
opened and removed after it is restored. In
lg-g5-dts the name fix is unconditional and the acceptance is not bounded by the
window (only the `_set_output` override is); mode 7 keeps that: the acceptance
has to last as long as the DSP instance the session configured, since a later
status event that is not accepted puts the eARC output back to PCM signalling
under a compressed stream. `window_seconds`/`supervised` bound the `_set_output`
override exactly as in mode 5. Mode 6 registers the same two probes with the
same decisions but only counts and prints (`would accept`, `would substitute`); it
does not open the gate, because the sites are reached by any output-status event
on an optical or eARC output, whatever the codec. On 6.12 the derived compare is
the last of a chain, so mode 6 sees there only events with codec 4 or above; the
name load sees every event on both generations.

**Known limitation of modes 3, 4 and 5 (unverified).** They open the gate with
no name probe. While one of them is loaded and a DTS-HD source plays on an
optical or eARC output, a codec-7 status event reaches LG's stock name load,
which reads `table[7]`; on the 5.4 images `table[6]` and `table[7]` are the
ASCII of a neighbouring string, not pointers. This is read from the disassembly
and from lg-g5-dts's `tests/emulate_status.py` (its stock run logs with that
garbage pointer), not observed on a TV, and whether LG's log call dereferences
it depends on its log mask. Do not play DTS-HD while modes 3 to 5 are loaded.
Real sessions therefore use mode 7 (`--dts-hd`) for DTS core too: it registers
the name probe before it opens the gate, so a source that switches from a core
track to a DTS-HD track is covered. Modes 3 to 5 stay as test stages and are
themselves unchanged.

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
`try_module_get` succeeds, the function is not in the init section), and mode 2
confirms it empirically. The leaf stays a named symbol: on 6.12 `register_kprobe`
returns `-ENOENT` unless kallsyms resolves the probed address. Modes 6 and 7
then run lg-g5-dts's second pass on the same leaf, since their probes redirect
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

Expected mode-2 output on a 5.4 TV: `break walk with hooks: incomplete nodes=0`
(by design -- our two nodes are module memory, which the walk never reads) and,
after undo, `break walk after undo: complete nodes=1` (LG's `bug_handler` stays on
the list). The explicit `synchronize_rcu` after each unregister is a second,
redundant wait: `unregister_kernel_break_hook` (and `unregister_kprobe`, through
`unregister_kprobes`) already wait a grace period on both generations.

**Kernel writes and their undo.** Nothing else writes kernel memory.

| Write | Made by | Undo |
|---|---|---|
| break-hook list: add our #4 and #6 nodes | stage a (only if not already present) | `unregister_kernel_break_hook` (#6 then #4), then `synchronize_rcu` |
| one kprobe on `_set_output` (modes 4, 5, 7) | `register_kprobe` | `unregister_kprobe` |
| one kprobe on the `_cb_output_status` name load (modes 6, 7; in mode 7 before the gate is opened) | `register_kprobe` | `unregister_kprobe`, after the gate is restored |
| one kprobe on the `_cb_output_status` compare (modes 6, 7; in mode 7 after the gate is opened) | `register_kprobe` | `unregister_kprobe`, before the gate is restored |
| one kprobe on the self-test leaf (modes 2, 4, 5; twice in modes 6, 7) | `register_kprobe` | `unregister_kprobe`, within init |
| one instruction: DTS check word 0 `mov w0,#0` -> `mov w0,#1` (modes 3, 4, 5, 7) | `aarch64_insn_patch_text` under `cpus_read_lock` | patch back to `mov w0,#0`, but only if the live word is still the one we wrote |

**Failure paths** undo in reverse: self-test fail -> unregister hooks, `synchronize_rcu`; gate-patch fail -> unregister hooks, `synchronize_rcu`; probe-register fail -> restore gate, unregister hooks, `synchronize_rcu`; window-thread fail -> unregister probe, restore gate, unregister hooks, `synchronize_rcu`. `cleanup_module` (modes 3-5) stops the window thread, unregisters the probe, restores the gate, unregisters the hooks, then `synchronize_rcu`. It does **not** repair the audio graph: like lg-g5-dts, that is left to the session script.
Modes 6 and 7 take every failure through that same cleanup, which also removes the two `_cb_output_status` probes. Mode 7 sets up in the order hooks, self-tests, name-load probe, gate, compare probe, `_set_output` probe, window thread, and the cleanup runs it backwards: window thread, `_set_output` probe, compare probe, gate, name-load probe, hooks, `synchronize_rcu`. Their failure points are the self-tests, the name-load `register_kprobe`, the gate patch (mode 7), the other two `register_kprobe` calls and the window thread (mode 7).

**New imports** (called directly; build.py proves each is exported by every kernel
of the generation): `register_kprobe`, `unregister_kprobe`, `cpus_read_lock`,
`cpus_read_unlock`, `synchronize_rcu`. `register_kernel_break_hook`,
`unregister_kernel_break_hook` and `aarch64_insn_patch_text` are not exported and
are called through their kallsyms addresses.

**Hand-declared kernel structs** (`earc_route.c`, with static assertions). `struct
pt_regs` (`regs[31]` at +0, `pc` +256, `pstate` +264: the `user_pt_regs` prefix;
`kprobe_breakpoint_handler` of every capture reads `[regs,#256]` and
`[regs,#264]`; modes 2-5 touch x0..x3, modes 6 and 7 also the derived registers,
`pc` and the NZCV bits of `pstate`) and `struct break_hook` (`fn` +16,
`imm` +24, `mask` +26, 32 bytes) are identical on both generations. `struct
kprobe` differs: `addr` +0x28, `pre_handler` +0x40 on both; its size is 0x88 on
5.4 (flags +0x80) and 0x80 on 6.12 (flags +0x78, after `fault_handler` was
dropped). The kernel touches nothing past `flags`, so that is the full size; we
zero the struct and set only `addr` and `pre_handler`. The sizes are
`-D` macros from `build.py`; the `_Static_assert`s, compiled per generation, are
the layout check (the captures hold no kprobe instance to measure directly).

**6.12.** Everything works: `aarch64_insn_patch_text` has the same signature
(`int(void **addrs, u32 *insns, int cnt)`), the pre-handler contract is the same
(return non-zero to skip single-stepping; we return 0), the break-hook layout and
the registration/patch call conventions match 5.4. No mode refuses on 6.12 for a
generation reason. Only `struct kprobe`'s size differs, and that is handled.
Modes 6 and 7 work on both shapes of `_cb_output_status` by the same rule
(continue at the derived supported branch): on 5.4 that is the `b.ls` target, on
6.12 the instruction after the `b.ne`. The host test derives the sites and the
registers (codec w6, output x19, name x7 from index x4, 6 entries) on all five
6.12 kernels.

The pre-handler runs in breakpoint context: it calls no kernel function but `printk` (first 5 matches, first 8 non-matches), takes no
lock, sleeps not, reads only the load-time bounds-checked ADEC addresses and the
caller's descriptor (null-checked), updates atomics, and (mode 5, in-window)
rewrites x2/x3. The two `_cb_output_status` pre-handlers (modes 6, 7) obey the
same contract: they read the probed instruction's own registers, the output's
type word through the output pointer the function itself has just read through
(null-checked), and ADEC 1's codec and started flag; they print the first 8
stock and first 5 acting hits each (`earc_route: hd status codec=... `,
`earc_route: hd name codec=...`), and in mode 7 write `pc`, the NZCV flags and
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
  `cpus_read_unlock` and `synchronize_rcu`; each one is in every kernel's PREL32
  ksymtab (`struct kernel_symbol` is `{value, name, namespace}` offsets on both)
  at its kallsyms address, the name spelled exactly (6.12 merges `mutex_lock`
  into the tail of `ww_mutex_lock`). The image base for that proof is
  `(_stext & ~0xfffff) + text_offset` from the Image header:
  `0xffffffc010088000` on 5.4, `0xffffffc080000000` on 6.12.

## Test

```sh
EARC_C5_DIR=... EARC_G5_DIR=... [EARC_EXTRA_DIRS="..."] [EARC_612_DIRS="..."] sh webos25/kmod/test/run.sh
```

Every directory is passed to the test with the sha256 of its `kernel.bin`
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
The other rows were checked against an independent static derivation. `_set_output`
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
immediate/mask unknown, an incomplete walk); the DTS-core policy over a table of
cases; the gate "restore only what we wrote" rule; and `earc_mode_refusal` for
each mode when an item it depends on is missing (break hooks for mode 2, the gate
and patch service for mode 3, `_set_output`/ADEC for modes 4-5), including that a
mode does not refuse on an item it does not depend on.

The DTS-HD decisions are tested on every kernel given, oracle or not: the sites
and registers derive and modes 6 and 7 accept the stock image; the probed words
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

A TV whose `uname -r` is not in the loader's allowlist is refused. To add one:

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
9. Add the release to the allowlist in `earc-route-load.sh` (the `case "$release"`
   block and its refusal message), and to the release table under Load.
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
sh earc-route-load.sh --route renderer=0 [restore_pcm=0|1] [carrier_rate=48000|192000]
sh earc-route-load.sh --selftest      # mode 2: BRK dispatch self-test, then unload
sh earc-route-load.sh --open-gate     # mode 3: open the DTS check, stay loaded
sh earc-route-load.sh --observe       # mode 4: open the gate, count _set_output calls
sh earc-route-load.sh --dts-core [window_seconds=30] [supervised=0|1]  # mode 5: override
sh earc-route-load.sh --observe-hd    # mode 6: count the two _cb_output_status sites, change nothing
sh earc-route-load.sh --dts-hd [window_seconds=30] [supervised=0|1]    # mode 7: DTS core and DTS-HD sessions
```

`--selftest`, `--open-gate`, `--observe`, `--dts-core`, `--observe-hd` and
`--dts-hd` (modes 2-7) refuse, before `insmod`, if another DTS tool is loaded
(`dts_gate`, `dts_core_route`, `gh_dts_native`) or `/var/lib/lg-dts-core/enabled`
exists; the audit only reports those. **Do not play DTS-HD while modes 3 to 5
(`--open-gate`, `--observe`, `--dts-core`) are loaded: see the known limitation
above. For a real session, DTS core included, load `--dts-hd`.** **Modes 2, 3, 4, 6 and 7 have run on one C5
without a receiver and left it stock, and mode 7 ran with a receiver on a CS (see
Status above); mode 5 has never run; nothing has run on 6.12.** Read-only parameters report the
state: `gate_open`, `gate_opened_by_us`, `hooks_registered`, `probe_registered`,
`selftest_passed`, `window_open`, `calls_seen`, `calls_matched`, `overrides`,
and for modes 6 and 7 `hd_probes_registered` (2 when both are in),
`hd_status_seen`, `hd_status_accepts`, `hd_name_seen`, `hd_name_fixes`.

`earc_nonpcm` is the live eARC non-PCM word (the cached argument 0 of
`audio_hal_earc_set_output_format`, `struct earc_layout.nonpcm`): 0 for PCM, 1
for a compressed stream. Its getter reads the derived address at every access,
in every mode that stays loaded, so a script waits for
`cat /sys/module/earc_route/parameters/earc_nonpcm` to print `1` where
lg-g5-dts runs `devmem` on a fixed physical address. `struct kernel_param_ops`
is hand-declared as `{flags, set, get, free}` (`get` at +16, static-asserted):
`param_attr_show` of every capture calls `attribute->param(+56)->ops(+16)->get(+16)`
with the buffer and the parameter and returns its count unchanged, and
`param_get_uint`'s own format is `"%u\n"`, so the getter writes the number and
the newline itself.
In dmesg, modes 4 and 5 print `earc_route: pass nomatch ...` for the first 8 `_set_output` calls that miss the policy, with every input of the decision: `index`, `desc` (0 = null descriptor, `mask` then shown as 0), `mask`, ADEC 1 `adec1-codec`/`open`/`started`, and the call's `codec-arg`/`bypass-arg`.

`earc_route-5.4.ko` and `earc_route-6.12.ko` sit next to the script; `uname -r`
picks one, with its stock module path and `struct module` layout:

| release | module | stock module | size/init/exit |
|---|---|---|---|
| `5.4.268-329.ptl4tv.5` | `earc_route-5.4.ko` | `/lib/modules/$release/kernel/aspectratiodrv.ko` | 0x380/0x150/0x338 |
| `5.4.268-329.ptl4tv.2` | `earc_route-5.4.ko` | `/lib/modules/$release/kernel/aspectratiodrv.ko` | 0x380/0x150/0x338 |
| `6.12.44-352` | `earc_route-6.12.ko` | `/usr/lib/modules/$release/kernel/aspectratiodrv.ko` | 0x4c0/0x138/0x480 |

The loader refuses unless:

- the release is one of the three above (a missing stock module path refuses too;
  nothing is probed);
- the stock `aspectratiodrv.ko` has the generation's layout and our module
  has the same vermagic and layout;
- every needed symbol is in `/proc/kallsyms` exactly once, in the core kernel,
  with type T/t (`_etext`: T/t/D/d) and a non-zero address.

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

## HDMI passthrough session (`hdmi-session.sh`)

`--dts-hd` alone makes no sound: the module opens the DTS check and overrides
LG's decisions, but does not repair the audio graph.
`hdmi-session.sh` does that for one session, from an HDMI input to an eARC
receiver. It is a port of lg-g5-dts's `tools/dts-core-route/run-on-tv.sh` and
`tools/dts-hd-route/run-on-tv.sh` (see NOTICE) that reads the HDMI port, the
decoder and the bypass gain block from the TV instead of one G5 image's values.
It runs as root with busybox `sh`, next to `earc-route-load.sh` and the `.ko`
files.

```sh
sh hdmi-session.sh --check            # read-only preflight; prints each check and a verdict
sh hdmi-session.sh --apply [15..60]   # timed test (default 30 s), then restores
sh hdmi-session.sh --session          # runs until the source changes; exit 10 on source loss
```

**Two kinds of session.** The stream on the HDMI input decides the kind, and
the preflight says which (`live DTS core at 48 kHz` or `live DTS-HD at 192 kHz`):

| Kind | `/proc/lgtv-driver/audio/hdmi` | Loader | `adec.p1` waits for | DSP instance writes |
|---|---|---|---|---|
| DTS core | `Codec=DTS` or `UNKNOWN_DTS`, `SamplingRate=48000` | `--dts-hd` | `UserCodec=DTS` | `codec 5`, `mode 1`, `data-drop-mode 1`, `drop-output-data false`, `start 1`, `drop 0` |
| DTS-HD | `Codec=DTS_HD` or `UNKNOWN_DTS_HD`, `SamplingRate=192000` | `--dts-hd` | `UserCodec=DTS_HD` | `codec 6`, `mode 4`, `sampling-freq 192000`, `channel-num 8`, `data-drop-mode 1`, `drop-output-data false`, `start 1`, `drop 0` |

Both kinds load the same module stage, mode 7 (`--dts-hd`), which overrides the
route for ADEC codec 48 and 51 and has LG's codec-name load guarded, so a source
that switches from a core track to a DTS-HD track during a core session does not
reach the unguarded load described under "Known limitation of modes 3, 4 and 5".
The script never loads `--dts-core`. The kind is still fixed for the session: a
stream that changes kind ends it like any other source change.

The names come from LG's tables, not from a guess. In the eight saved 5.4
kernels the HDMI type table prints type 202 as `DTS_HD`, 203 as `DTS_HD_HRA`,
1004 as `UNKNOWN_DTS`, 1202 as `UNKNOWN_DTS_HD` and 1203 as
`UNKNOWN_DTS_HD_HRA`, and the ADEC table prints codec 48 as `DTS` and 51 as
`DTS_HD`. In the C5 33.31.68 disassembly `HDMI21_HAL_Rx_Get_AudioType` stores
202 or 1202 for an IEC 61937 data type 0x11 (DTS-HD) burst, and 203 or 1203 for
its subtype 0x211, which is refused: lg-g5-dts has no session for it. (The same
constants are in that function on the G5 33.31.69, G2 33.31.68, C5 43.21.77 and
G5 43.21.77 kernels; its code was not read there.) A C5 prints `UNKNOWN_DTS` for a
core stream (measured) where lg-g5-dts's G5 printed `DTS`; lg-g5-dts's G5
printed `DTS_HD` for DTS-HD. **Which of the two DTS-HD names a C5 prints is
unmeasured**, so both are accepted. On the five saved 6.12 kernels the same
type numbers and the same strings are present, but the table's pointers are
filled in at boot, so the pairing is not confirmed there.

The decoder is chosen as in `hdmi-watch.sh`: `adec.pN` carries the input when it
is connected to the port of its own instance `dsp0/hdmiN` and it or the instance
is started (`UserCodec` decides between two). When nothing is started yet (a stock
TV refuses DTS), decoder 1 is used only if `adec.p1`/`hdmi1` is the one connected
pair, since the input-service restart is what starts it; otherwise it refuses.

`--check` changes nothing. It requires: root; a live stream of one of the two
kinds (`AudioData=1`); `EArcOnOff=On`; one `dsp0/hdmi1` DSP instance whose
`hdmi-port=(int)n` is connected on `adec.p1` (an input on `adec.p0` is
refused: the module only overrides decoder 1); ALSA numid 92 equal to
`2,1,0,0,1`; at most one gain block feeding a bypass block in
`/proc/aaudio/dot_module_connection`. The loader's own checks (ABI, derivation,
other DTS tools, already loaded) run only at `--apply`/`--session`; its refusal is
printed and the script stops.

`--apply` and `--session` then: load `earc-route-load.sh --dts-hd`
(`window_seconds=<seconds+20>`, or `window_seconds=0 supervised=1`);
restart `extinput-integration.service`; wait up to 10 s for `adec.p1` to read the
kind's `UserCodec` and `Start=1`; set numid 92 to its own value so LG's driver
builds the downstream connection; require `renderer_1_out100 -> mixer_0_in100`
and `mixer_0_out100 -> output_3_in0` in `dot_block_connection`; find the one gain
block feeding a bypass block.

- *DTS core:* unmute that gain block, write the DSP instance values, then sample
  state each second.
- *DTS-HD:* **mute** that gain block (`mute-input true`, `mute-output true`),
  write the DSP instance values, wait 2 s, re-check the source, and read
  `/sys/module/earc_route/parameters/earc_nonpcm` (where lg-g5-dts runs `devmem`
  on a fixed address). Unless it reads `1` the session ends there, still muted
  (`eARC is not non-PCM (earc_nonpcm=N); aborting muted`), and cleanup puts the
  gain back as it was found. Otherwise the `dsp1/output_arc` prop is saved to
  the log directory, the gain block is unmuted, and state is sampled each second.

On exit, interrupt or any failure after the first write, cleanup quiesces the DSP
instance, restores the gain and prop values it changed (for DTS-HD also
`sampling-freq` and `channel-num`), unloads `earc_route`, restores numid 92 and
restarts the input service. If anything could not be restored it prints
`Rollback incomplete; ... restart TV` and exits non-zero. It restarts the
input service at start and at exit (the flow that worked on the tester's CS),
although on a C5 an input-service restart (from an earlier `hdmi-watch.sh`) left
one HDMI input playing another's audio until a TV restart. Logs, including the
`earc_route` dmesg lines and the module counters including the `hd_*` ones (for
DTS-HD also `earc-nonpcm.txt`), go to `/tmp/dts25-hdmi-<epoch>-<pid>/`
(mode 700).

One order differs from lg-g5-dts: our route hook is already active when the
input service restarts, where lg-g5-dts loads its route module after it. In one
run on one C5 without a receiver, with a DTS core stream, `_set_output` was
called once at that restart, about 40 ms before ADEC 1 started on DTS, and did
not match (`adec1-codec=0 started=0`). So the override would hit the call the
numid 92 write provokes, as in lg-g5-dts. On a receiver TV the order is
untested.

Tester steps: start DTS core (48 kHz) or DTS-HD playback on the HDMI input with
an eARC receiver connected and eARC on; run `sh hdmi-session.sh --check` and send
its output; only if it says `CHECK OK`, run `sh hdmi-session.sh --apply 30`, watch
the receiver's format display, and send the printed log directory's contents.
To keep DTS going while you watch, instead of a 30-second test, run
`sh hdmi-session.sh --session` (not yet reported from any TV). It runs until the
stream on the input changes, so a seek or an input switch ends it and you run it
again. Keep the SSH session open while it runs. Ctrl-C stops it and puts the TV
back.
Successful script exit is not proof of sound or DTS lock.

**Verified:** the host test (`sh test/session/run.sh`, under sh, dash and busybox
ash) against real C5 status files, for both kinds; on a C5 (33.31.68, no
receiver, nothing playing) the no-argument run and `--check`, which refused on
the two failed checks and left the TV unchanged. Mode 7 by hand, once, on a C5
(33.31.68, kernel 5.4.268, TV speakers, no receiver) with a DTS 5.1 core stream
from a Google TV Streamer: the input read `Codec=UNKNOWN_DTS`,
`SamplingRate=48000`; `earc-route-load.sh --dts-hd window_seconds=30` loaded with
both self-tests ok, the DTS check open and three kprobes listed
(`_cb_output_status+0x108`, `_set_output+0x0`, `_cb_output_status+0x48`); after
the input service restart ADEC 1 read `UserCodec=DTS`, `Start=1`, `Mute=1`
within a second (kernel log: codec 48); the one `_set_output` call is the one
described above, with `mask=0x2` (the TV speakers; eARC would be 0x20); there were no
output-status events (`hd_status_seen=0`, `hd_name_seen=0`), `earc_nonpcm=0` and
no override; unload restored the check, the audit afterwards showed the stock
state, and the kernel logged no errors.

With a receiver: on a CS (OLED55CS6LA, 33.31.68, kernel 5.4.268-329.ptl4tv.5) with
a Samsung HW-Q930F over eARC and a set-top box on HDMI 1, `hdmi-session.sh --apply
30` (mode 7) made the soundbar show DTS for DTS core, DTS-HD for DTS-HD MA and
DTS:X for DTS:X, and it worked again after a TV power cycle (2026-11-11, tester
report; the soundbar's display, not a measurement of the bitstream). The first run
on that TV failed when the script gave up during the input-service restart; the
wait loop above came from that.

**Not verified:** `--session` has not been reported from any TV. Nothing has run
on kernel 5.4.268-329.ptl4tv.2 (33.22.x) or on 6.12. No DTS-HD stream has been
played into a C5 (so its `Codec=` name there is unmeasured), and the DTS-HD states
in the host test are synthetic, including the `output_arc` prop file, which has no
real sample because the C5 has no eARC output instance.

## HDMI passthrough watcher (`hdmi-watch.sh`)

`hdmi-session.sh --session` ends at the first seek or input switch and has to
be run again by hand. `hdmi-watch.sh` loads `earc_route` once
(`earc-route-load.sh --dts-hd window_seconds=0 supervised=1`), so the DTS check
is already open when a stream arrives, and then polls the input once a second.
In one C5 run with the check open before DTS arrived, the decoder accepted the
stream on its own, and again after a seek (input `NO_AUDIO`, then MAT for about
3 s, then DTS) and after switching inputs away and back, with no input-service
restart.

```sh
sh hdmi-watch.sh                              # foreground; Ctrl-C stops it
nohup sh hdmi-watch.sh > /dev/null 2>&1 &     # background; stop it with kill <pid>
```

It never restarts `extinput-integration.service`: on a C5 a restart while DTS was
live left both `adec.p0` and `adec.p1` on `HDMI_PORT2` (the PS5's audio while
watching the Streamer on port 0), which only a TV restart cleared. A DTS stream
that was already playing when the module loaded was refused by the TV and needs
restarting by hand (see *No decoder playing*).

It refuses when not root, when `earc_route` is already loaded, when another
watcher holds `/tmp/dts25-hdmi-watch.lock` (a lock whose pid is dead is taken
over), or when the loader refuses.

- *Decoder.* Decoder N (1, then 0) plays the input only if its own DSP instance
  `dsp0/hdmiN/*/prop` exists, `adec.pN` has `Connect=HDMI_PORT<that instance's
  hdmi-port>`, and `adec.pN` reads `Start=1` or the instance `start=(int)1`. A C5
  showed why: `adec.p1` stayed `Open=1` on port 2's idle `hdmi1` instance while
  `adec.p0` played port 0 through `hdmi0`. When both qualify, the one whose
  `UserCodec` is the stream's codec wins; otherwise neither is used (logged).
- *No decoder playing* a live DTS stream (it arrived before the module loaded,
  and the TV refused it): with eARC on and numid 92 at `2,1,0,0,1`, it prints once
  per stream `DTS is arriving but no decoder plays it (the TV refused it before
  the module loaded); skip, pause and play, or switch inputs and back to restart
  the stream.` and keeps watching. This is not a failed attempt.
- *Apply.* A DTS core or DTS-HD stream (the kinds in the session table above) on
  decoder 1, with `EArcOnOff=On` and numid 92 at `2,1,0,0,1`, seen on two
  polls in a row, gets the same writes in the same order as `hdmi-session.sh`
  (numid 92, route check, the one gain block, the DSP instance; DTS-HD muted
  until `earc_nonpcm` reads 1). If `adec.p1` has not started on the stream's
  codec within 3 s, the attempt fails. The DSP instance and gain values are saved once per
  run, from the first instance it writes, and saved again only for a new
  instance or gain block.
- *Revert*, at once, when the stream's kind, port, decoder or DSP instance
  changes: mute the gain block, then, if the instance still has our port and
  codec, `drop 1`, `start 0`, the saved values and the saved gain mute values
  (otherwise LG has rewritten it and it is left alone), then numid 92 to its own
  value. A core to DTS-HD change is a
  revert and then a new apply.
- *Stop* (TERM, INT or HUP): revert, `rmmod earc_route`, numid 92. If DTS is
  still live it prints `DTS is still playing on HDMI; switch to another input and
  back, or the TV keeps the passthrough decoder state until the stream changes.`
- A failed attempt (no compressed route, no single gain block, the decoder never
  started, `earc_nonpcm` not 1) is reverted and not retried until the stream
  ends. Three in a row stop the watcher.
- **Decoder 0 is not handled**: the module overrides decoder 1 only. A DTS stream
  on decoder 0 is logged once per stream with a capture (`decoder0-N/`: both
  ADEC files, both dot files, the `hdmi0` and `hdmi1` props, the last `pass nomatch` kernel
  line) and left alone.

Exit codes: 0 stopped cleanly; 1 refused, or three failed attempts in a row; 20
the rollback is incomplete (`Rollback incomplete; restart the TV.`). Logs go to
`/tmp/dts25-hdmi-watch-<epoch>-<pid>/` (mode 700): `watch.log` (each input
change, apply and revert, with the module counters), `loader.txt`, the saved
originals, the last `route.dot` and `module.log`.

**Verified:** the host test (`sh test/watch/run.sh`, under sh and bash); on a C5
without an eARC receiver, decoder and port detection through seeks, a track
restart and an input switch, and the stop. Never run with a receiver.
