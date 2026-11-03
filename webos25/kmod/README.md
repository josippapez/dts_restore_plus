# eARC route kernel module (webOS 25, kernels 5.4.268 and 6.12.44)

`earc_route-<generation>.ko` connects a renderer's PCM-bypass output to the TV's eARC output 3
with non-PCM signalling, so a DTS bitstream reaches an eARC receiver. It is a
port of [Glasshouse](https://github.com/rorygallagher2024/glasshouse)'s
`native/dts-forwarding` route (GPL-2.0-only; see [NOTICE](NOTICE)).

Glasshouse's module and the earlier C5 port are pinned to one kernel image: every
LG-private address is compiled in. This one is built once per kernel generation,
from one source: `earc_route-5.4.ko` (webOS 25 firmware 33.31.x, kernel
5.4.268-329.ptl4tv.5) and `earc_route-6.12.ko` (firmware 43.21.x, kernel
6.12.44-352), the generation-specific values passed as `-D` macros by `build.py`.
The loader passes each LG function's address and size from `/proc/kallsyms`, and
the module derives every data address and struct offset from LG's own code when
it loads. If anything does not match, it refuses to load.

Status: both builds host-tested against the kernels listed under Test; the 6.12
build has never run on a TV. Neither `.ko` is committed (`out/` is ignored).
Neither generation uses MODVERSIONS (no `__versions`, no CRCs) or BTI, so no
`-mbranch-protection` flag is needed; both kernels have PAC on.

## Files

| File | What it is |
|---|---|
| `earc_decode.h` | Freestanding AArch64 decoder and derivations, shared by the module and the test |
| `earc_route.c` | The module: parameter checks, decode, audit (`mode=0`) or route (`mode=1`) |
| `build.sh`, `build.py` | Container build plus ABI, import and export checks against kernel captures |
| `earc-route-load.sh` | TV-side loader (root, busybox sh + python3) |
| `test/run.sh`, `test/test_decode.c` | Host test: sha-keyed oracles, refusal tests, C5/G5 and 5.4/6.12 differences |

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
| `printk` import | `printk` | `_printk` |

- every stock module of the generation has that vermagic and `struct module`
  layout, and the built module's layout is identical (name at offset 24 in both);
- GCC's optimised tree has no `__builtin_unreachable` or `__builtin_trap`;
- the imports are only the printk symbol above, `mutex_lock`/`mutex_unlock`,
  `msleep`, the kthread calls and `param_{set,get}_{uint,ulong}`, and each one is
  in every kernel's PREL32 ksymtab (`struct kernel_symbol` is `{value, name,
  namespace}` offsets on both) at its kallsyms address, the name spelled exactly
  (6.12 merges `mutex_lock` into the tail of `ww_mutex_lock`). The image base for
  that proof is `(_stext & ~0xfffff) + text_offset` from the Image header:
  `0xffffffc010088000` on 5.4, `0xffffffc080000000` on 6.12.

## Test

```sh
EARC_C5_DIR=... EARC_G5_DIR=... [EARC_EXTRA_DIRS="..."] [EARC_612_DIRS="..."] sh webos25/kmod/test/run.sh
```

Every directory is passed to the test with the sha256 of its `kernel.bin`
(`--kernel SHA256 DIR`); the four variables are equivalent apart from naming.
Without captures it prints `SKIP` and passes. With them, the test applies the
oracle table keyed by that sha256, so an oracle never meets another image:

- 5.4: C5 33.31.68/69 (hand-derived) and G5 33.31.69 (Glasshouse's values);
- 6.12: C5 43.21.73 and 43.21.77, G5 43.21.71 and 43.21.77, G2 43.21.74, each
  derived from its corrected `kallsyms.txt` and disassembly;
- every other image is decoded and printed without an oracle.

For each oracle kernel it asserts every derived value, including the clock
layout and the image link base. On the first oracle kernel of each generation
it runs refusal tests that mutate one instruction (or the loader's
`mutex_lock` address) and require a specific refusal; on 6.12 also: the channel
callback load removed, the rate/channel store removed or moved onto the flag.
Removing the `sub` of the 6.12 index scaling is not a refusal: the stride, the
mutex and the entry loads then agree on 0x80, so the test asserts that the
decoder follows the code to 0x80. It also checks that C5 and G5 differ exactly
where expected in each generation (rate +104/+96, acmod +108/+100, NONPCM
+68/+60) and that a family's 5.4 and 6.12 kernels differ exactly in the clock
layout (stride 0x60/0x70, mutex +0x38/+0x48, callback +32/+40, object rate
+12/+8, channel count none/+12) and in no HAL offset.

## Load (on the TV, as root)

```sh
sh earc-route-load.sh                 # audit: decode, print to dmesg, unload
sh earc-route-load.sh --route renderer=0 [restore_pcm=0|1] [carrier_rate=48000|192000]
```

`earc_route-5.4.ko` and `earc_route-6.12.ko` sit next to the script; `uname -r`
picks one, with its stock module path and `struct module` layout:

| release | module | stock module | size/init/exit |
|---|---|---|---|
| `5.4.268-329.ptl4tv.5` | `earc_route-5.4.ko` | `/lib/modules/$release/kernel/aspectratiodrv.ko` | 0x380/0x150/0x338 |
| `6.12.44-352` | `earc_route-6.12.ko` | `/usr/lib/modules/$release/kernel/aspectratiodrv.ko` | 0x4c0/0x138/0x480 |

The loader refuses unless:

- the release is one of the two above (a missing stock module path refuses too;
  nothing is probed);
- the stock `aspectratiodrv.ko` has the generation's layout and our module
  has the same vermagic and layout;
- every needed symbol is in `/proc/kallsyms` exactly once, in the core kernel,
  with type T/t (`_etext`: T/t/D/d) and a non-zero address.

It then runs `insmod` once and prints one result line. The module re-checks
every address against `[_stext, _etext)`, compares `fn_mutex_lock` and
`fn_mutex_unlock` with the symbols the kernel resolved for it, and refuses a
derived data address that is misaligned or not within 64 MiB after `_etext`.
