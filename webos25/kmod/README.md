# eARC route kernel module (webOS 25, 5.4.268)

`earc_route.ko` connects a renderer's PCM-bypass output to the TV's eARC output 3
with non-PCM signalling, so a DTS bitstream reaches an eARC receiver. It is a
port of [Glasshouse](https://github.com/rorygallagher2024/glasshouse)'s
`native/dts-forwarding` route (GPL-2.0-only; see [NOTICE](NOTICE)).

Glasshouse's module and the earlier C5 port are pinned to one kernel image: every
LG-private address is compiled in. This one is built once per kernel generation.
The loader passes each LG function's address and size from `/proc/kallsyms`, and
the module derives every data address from LG's own code when it loads. If
anything does not match, it refuses to load.

Status: built and host-tested against the C5 33.31.68 and G5 33.31.69 kernels.
Not yet audited on a TV. The `.ko` is not committed yet (`out/` is ignored).

## Files

| File | What it is |
|---|---|
| `earc_decode.h` | Freestanding AArch64 decoder and derivations, shared by the module and the test |
| `earc_route.c` | The module: parameter checks, decode, audit (`mode=0`) or route (`mode=1`) |
| `build.sh`, `build.py` | Container build plus ABI, import and export checks against kernel captures |
| `earc-route-load.sh` | TV-side loader (root, busybox sh + python3) |
| `test/run.sh`, `test/test_decode.c` | Host test: oracles, refusal tests, C5/G5 difference |

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
| clock table, stride, entry 3 | `aud_clock_update_output`: the mutex is table + index * stride + 0x38. Also requires the index <= 3 check, entry+8 and +9 reads, the object at +16, the rate at object+12, the flag at object+16, and a call through `*(*(object+128)+32)` |

Struct offsets the route uses are checked the same way. The connection walk
(+48 list, +16 peer, +32 owner, +40 port id, +88 block-info) is checked in
`_bm_disconnect_block_locked`, through its `_bm_get_block_port(_bm_find_block_by_block_info(info), port, 1)`
call. `bm_get_block_info_by_index` must return +88. The renderer's +76/+77 are
checked in `renderer_get_pcm_bypass` and `renderer_get_uid_pcm_bypass_head`. The
output's +44 == 3, +76 rate, +84 and +129 eARC-on are checked in `output_set_earc_on`.
That function's calls also confirm `fn_aud_clock_update_output` and `fn_audio_hal_earc_set_on`.

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
build refuses unless:

- every stock module has a 0x380-byte `struct module` with init at 0x150, exit at
  0x338 and vermagic `5.4.268 SMP preempt mod_unload aarch64`, and the built
  module's layout is identical;
- GCC's optimised tree has no `__builtin_unreachable` or `__builtin_trap`;
- the imports are only `printk`, `mutex_lock`/`mutex_unlock`, `msleep`, the
  kthread calls and `param_{set,get}_{uint,ulong}`, and each one is in every
  kernel's PREL32 ksymtab at its kallsyms address.

## Test

```sh
EARC_C5_DIR=... EARC_G5_DIR=... [EARC_EXTRA_DIRS="..."] sh webos25/kmod/test/run.sh
```

Without captures it prints `SKIP` and passes. With them, it checks each image's
sha256 and then:

- asserts every derived value against the hand-derived C5 values and
  Glasshouse's G5 values;
- runs refusal tests that mutate one instruction (or the loader's
  `mutex_lock` address) and require a specific refusal;
- checks that C5 and G5 differ exactly where expected: rate +104/+96, acmod
  +108/+100, NONPCM +68/+60.

## Load (on the TV, as root)

```sh
sh earc-route-load.sh                 # audit: decode, print to dmesg, unload
sh earc-route-load.sh --route renderer=0 [restore_pcm=0|1] [carrier_rate=48000|192000]
```

The loader refuses unless:

- the release is allowlisted (`5.4.268-329.ptl4tv.5`);
- the stock `aspectratiodrv.ko` has our vermagic and `struct module` layout;
- every needed symbol is in `/proc/kallsyms` exactly once, in the core kernel,
  with type T/t (`_etext`: T/t/D/d) and a non-zero address.

It then runs `insmod` once and prints one result line. The module re-checks
every address against `[_stext, _etext)`, compares `fn_mutex_lock` and
`fn_mutex_unlock` with the symbols the kernel resolved for it, and refuses a
derived data address that is misaligned or not within 64 MiB after `_etext`.
