#!/bin/sh
# earc-route-load.sh -- load earc_route.ko on a rooted LG webOS 25 TV, or a webOS 6 (4.4) one.
#
#   sh earc-route-load.sh                    audit: decode, print, unload (mode=0)
#   sh earc-route-load.sh --dts-hd           the session (mode=7), until rmmod earc_route
#   sh earc-route-load.sh --check            every loader check and the build choice, no insmod
#
# On 4.4 the build has no exit and no modes: --dts-hd loads it until reboot.
# Runs as root, on the TV only (busybox sh + python3, or python 2.7 on webOS 6). There is no release
# allowlist: the running kernel is checked, not its name. The loader refuses
# unless the TV's own stock modules agree on one vermagic and struct module
# layout, exactly one earc_route-*.ko next to this script has that same
# vermagic and layout, and every symbol the module needs appears exactly once
# in /proc/kallsyms with a non-zero address. The module then derives the rest
# from LG's code and refuses itself on any mismatch. One insmod, never retried.
# Prints one result line.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
# Host tests only: a directory standing in for / (module trees, kallsyms, sysfs).
ROOT=${EARC_LOADER_ROOT:-}

refuse() { echo "earc-route: REFUSED: $*"; exit 1; }

[ "$(id -u)" = 0 ] || refuse "must run as root"
release=$(uname -r)
options="mode=0"
want_mode=0
case "$*" in
  '') ;;
  --dts-hd) options="mode=7"; want_mode=7 ;;
  --check) want_mode=check ;;
  *) refuse "usage: $0 [--dts-hd|--check]" ;;
esac
[ "$want_mode" = check ] || [ ! -e "$ROOT/sys/module/earc_route" ] || refuse "earc_route is already loaded"
# Our builds are unsigned. A kernel that enforces module signatures (C6 on 43.21.79,
# G6 on 43.99.98) rejects them at insmod with "Key was rejected by service".
[ "$(cat "$ROOT/sys/module/module/parameters/sig_enforce" 2>/dev/null)" != Y ] ||
  refuse "this kernel only loads modules signed by LG (sig_enforce=Y), and earc_route is unsigned"

# The session refuses to load beside another DTS tool, whose kprobes or gate
# change would collide with ours.
# The audit (mode 0) only reports them (below).
if [ "$want_mode" = 7 ]; then
  for mod in dts_gate dts_core_route gh_dts_native; do
    [ -e "$ROOT/sys/module/$mod" ] && refuse "another DTS tool is loaded: $mod (unload it first)"
  done
  [ -f "$ROOT/var/lib/lg-dts-core/enabled" ] && refuse "lg-dts-core is enabled (/var/lib/lg-dts-core/enabled); disable it first"
fi

# Picks the module from the running kernel's own stock modules, reads
# /proc/kallsyms, and prints the module path then the fn_/sz_ parameters, or
# the reason for refusing.
if PYTHON=$(command -v python3); then PYTHON="$PYTHON -I"
elif PYTHON=$(command -v python2); then PYTHON="$PYTHON -E -s"
else refuse "python not found"; fi
# shellcheck disable=SC2086 # PYTHON is the interpreter and its isolation flags
parameters=$($PYTHON - "$ROOT" "$release" "$HERE" <<'PY'
import os
import struct
import sys

# Mirrors EARC_FUNCTIONS in earc_decode.h: (name, analysed).
FUNCTIONS = [
    ('audio_hal_earc_set_output_format', 1), ('audio_hal_earc_set_channel', 1),
    ('audio_hal_earc_set_audio_mode', 1), ('audio_hal_earc_set_mute', 1), ('bm_connect_block', 1),
    ('bm_disconnect_block', 1), ('bm_get_block_info_by_index', 1), ('_bm_disconnect_block_locked', 1),
    ('aud_clock_update_output', 1), ('renderer_get_pcm_bypass', 1), ('renderer_get_uid_pcm_bypass_head', 1),
    ('output_set_earc_on', 1), ('audio_hal_earc_set_on', 0), ('aimc_send_command', 0),
    ('aud_hal_set_arc_sample_rate', 0), ('_bm_find_block_by_block_info', 0), ('_bm_get_block_port', 0),
    ('_bm_connect_block_locked', 0), ('mutex_lock', 0), ('mutex_unlock', 0)]

# Mirrors EARC_HDMI_FUNCTIONS in earc_hdmi.h: (name, analysed). Optional: a
# missing one disables only the HDMI audit, so it is not passed and does not
# refuse the load (the module reports it "not available").
HDMI_FUNCTIONS = [
    ('aud_hal_dts_supported', 1), ('_set_output', 1), ('_cb_output_status', 1),
    ('alsa_decoder_codec', 1), ('alsa_decoder_start', 1),
    ('input_get_hdmi_codec', 1), ('input_set_hdmi', 1), ('aimc_set_property', 1),
    ('KDRV_HDMI21_HAL_Rx_GetAudioInfo', 0), ('input_drop_data', 1),
    ('aimc_register_named_event', 0), ('aimc_unregister_named_event', 0),
    ('register_kernel_break_hook', 1), ('call_break_hook', 1), ('unregister_kernel_break_hook', 0),
    ('kprobe_breakpoint_handler', 0), ('kprobe_breakpoint_ss_handler', 0), ('kretprobe_breakpoint_handler', 0),
    ('aarch64_insn_patch_text', 0), ('register_kprobe', 0), ('unregister_kprobe', 0),
    ('cpus_read_lock', 0), ('cpus_read_unlock', 0),
    ('misc_open', 1), ('__fput', 1), ('__vfs_write', 1), ('vfs_write', 1), ('misc_register', 0), ('misc_deregister', 0),
    ('sndout_earc_output_type', 1), ('sndout_mute_input', 1)]


def fail(reason):
    print(reason)
    sys.exit(1)


def module_abi(path):
    """(vermagic, this_module size, init offset, exit offset) of a 64-bit
    little-endian AArch64 relocatable module. An offset is None when the module
    has no such function: modpost then leaves the pointer out, and a module with
    neither (the C1's atsc_dtv_soc_chip.ko) has no this_module relocations."""
    data = open(path, 'rb').read()
    if data[:6] != b'\x7fELF\x02\x01' or struct.unpack_from('<HH', data, 16) != (1, 183):
        fail(path + ' is not an AArch64 relocatable ELF')
    shoff, = struct.unpack_from('<Q', data, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', data, 0x3a)
    sections = [struct.unpack_from('<IIQQQQIIQQ', data, shoff + i * shentsize) for i in range(shnum)]

    def string(table, offset):
        start = sections[table][4] + offset
        return data[start:data.index(b'\0', start)].decode()

    named = {}
    for section in sections:
        named.setdefault(string(shstrndx, section[0]), []).append(section)
    for name in ('.modinfo', '.gnu.linkonce.this_module'):
        if len(named.get(name, [])) != 1:
            fail(path + ': expected one ' + name + ' section')
    relas = named.get('.rela.gnu.linkonce.this_module', [])
    if len(relas) > 1:
        fail(path + ': expected at most one .rela.gnu.linkonce.this_module section')
    modinfo = named['.modinfo'][0]
    fields = data[modinfo[4]:modinfo[4] + modinfo[5]].split(b'\0')
    magic = [field[9:].decode() for field in fields if field.startswith(b'vermagic=')]
    if len(magic) != 1:
        fail(path + ': expected one vermagic')
    targets = {}
    for rela in relas:
        symtab = sections[rela[6]]
        for i in range(rela[5] // 24):
            offset, info, _ = struct.unpack_from('<QQq', data, rela[4] + 24 * i)
            name_offset, = struct.unpack_from('<I', data, symtab[4] + 24 * (info >> 32))
            targets.setdefault(string(symtab[6], name_offset), []).append(offset)
    if len(targets.get('init_module', [None])) != 1 or len(targets.get('cleanup_module', [None])) != 1:
        fail(path + ': init_module/cleanup_module relocated more than once')
    return (magic[0], named['.gnu.linkonce.this_module'][0][5],
            targets.get('init_module', [None])[0], targets.get('cleanup_module', [None])[0])


def same_abi(build, stock):
    return build[:3] == stock[:3] and build[3] in (None, stock[3])


root, release, here = sys.argv[1:4]

# The running kernel's own modules: every one is built against the same struct
# module, so their vermagic, this_module size, and init and exit offsets (where
# a module has them) must agree. They are read in sorted order, so the choice is
# repeatable, until eight with an init have been read; one that disagrees
# refuses, as the layout would then be ambiguous.
stock_paths = []
for tree in ('/lib/modules/', '/usr/lib/modules/'):
    top = root + tree + release
    for directory, _, files in os.walk(top):
        stock_paths += [os.path.join(directory, name) for name in files
                        if name.endswith('.ko') and not name.startswith('earc_route')]
stock_paths = sorted(set(stock_paths))
if not stock_paths:
    fail('no stock module under /lib/modules/%s or /usr/lib/modules/%s' % (release, release))
stock = [None] * 4
with_init = 0
for path in stock_paths:
    abi = module_abi(path)
    for i, value in enumerate(abi):
        if value is not None and stock[i] is not None and value != stock[i]:
            fail('stock modules disagree on vermagic/layout: %s %r, read before it %r' % (path, abi, tuple(stock)))
        if stock[i] is None:
            stock[i] = value
    with_init += abi[2] is not None
    if with_init == 8:
        break
if stock[2] is None:
    fail('no stock module under /lib/modules/%s or /usr/lib/modules/%s has an init function' % (release, release))
stock = tuple(stock)

# Exactly one shipped build must equal the running kernel's ABI.
candidates = sorted(os.path.join(here, name) for name in os.listdir(here)
                    if name.startswith('earc_route-') and name.endswith('.ko'))
if not candidates:
    fail('no earc_route-*.ko in ' + here)
matching = [path for path in candidates if same_abi(module_abi(path), stock)]
if len(matching) != 1:
    fail('%d of %s match this kernel (vermagic %r, struct module %#x bytes, init %#x, exit %#x)' %
         ((len(matching), ', '.join(os.path.basename(path) for path in candidates)) + stock))
# A build without exit (4.4, legacy-audio) finds and checks LG's code itself: no parameters.
if module_abi(matching[0])[3] is None:
    print(matching[0] + ' permanent')
    sys.exit(0)

wanted = {name for name, _ in FUNCTIONS} | {name for name, _ in HDMI_FUNCTIONS} | {'_stext', '_etext'}
found = {}
core = set()
with open(root + '/proc/kallsyms') as kallsyms:
    for line in kallsyms:
        fields = line.split()
        if len(fields) < 3:
            continue
        address = int(fields[0], 16)
        if len(fields) == 3:
            core.add(address)
        if fields[2] in wanted:
            found.setdefault(fields[2], []).append((address, fields[1], len(fields) == 3))
core = sorted(core)


def symbol(name, types):
    matches = found.get(name, [])
    if len(matches) != 1:
        fail('kallsyms: %s appears %d times' % (name, len(matches)))
    address, kind, is_core = matches[0]
    if not is_core or kind not in types or not address:
        fail('kallsyms: %s is %s %#x%s' % (name, kind, address, '' if is_core else ' in a module'))
    return address


output = ['fn__stext=%#x' % symbol('_stext', 'Tt'), 'fn__etext=%#x' % symbol('_etext', 'TtDd')]
for name, analysed in FUNCTIONS:
    address = symbol(name, 'Tt')
    output.append('fn_%s=%#x' % (name, address))
    if analysed:
        following = [value for value in core if value > address]
        if not following:
            fail('kallsyms: nothing follows ' + name)
        output.append('sz_%s=%d' % (name, following[0] - address))


def optional_symbol(name):
    matches = found.get(name, [])
    if len(matches) != 1:
        return None
    address, kind, is_core = matches[0]
    if not is_core or kind not in 'Tt' or not address:
        return None
    return address


# HDMI set: pass what is present; skip (do not fail) anything missing.
for name, analysed in HDMI_FUNCTIONS:
    address = optional_symbol(name)
    if address is None:
        continue
    output.append('fn_%s=%#x' % (name, address))
    if analysed:
        following = [value for value in core if value > address]
        if following:
            output.append('sz_%s=%d' % (name, following[0] - address))
print(matching[0] + ' ' + ' '.join(output))
PY
) || refuse "$parameters"
KO=${parameters%% *}
parameters=${parameters#* }
if [ "$want_mode" = check ]; then echo "earc-route: CHECK OK: kernel $release takes $(basename "$KO")"; exit 0; fi
if [ "$parameters" = permanent ]; then
  [ "$want_mode" = 7 ] || refuse "$(basename "$KO") has no audit; it loads with --dts-hd only, until reboot"
  parameters= options=
fi
INSMOD=$(command -v insmod || true)
if [ -z "$INSMOD" ] && [ -x /sbin/insmod ]; then INSMOD=/sbin/insmod; fi
[ -n "$INSMOD" ] || refuse "insmod not found"

# Report other DTS tools present (never a refusal): the audit reads live state,
# and another tool's kprobes/gate change what is seen. A tester greps one line.
report_tool() {  # report_tool TEXT
  echo "earc-route: other-tool: $1"
}
[ -d "$ROOT/var/lib/lg-dts-core" ] && report_tool "lg-dts-core present at /var/lib/lg-dts-core (enabled=$([ -f "$ROOT/var/lib/lg-dts-core/enabled" ] && echo yes || echo no))"
[ -d "$ROOT/var/lib/lg-dts-native" ] && report_tool "glasshouse gh_dts_native present at /var/lib/lg-dts-native"
for mod in dts_gate dts_core_route gh_dts_native; do
  [ -e "$ROOT/sys/module/$mod" ] && report_tool "module $mod is loaded"
done
echo "earc-route: kernel $release: loading $(basename "$KO")"

# The kernel's own refusal reasons are in dmesg; keep only lines from this run.
before=$(dmesg | grep -c 'earc_route:' || true)
# shellcheck disable=SC2086 # options and parameters are word lists
if result=$("$INSMOD" "$KO" $options $parameters 2>&1); then status=0; else status=$?; fi
last=$(dmesg | grep 'earc_route:' | tail -n 1 || true)
after=$(dmesg | grep -c 'earc_route:' || true)
[ "$after" != "$before" ] || last="(no earc_route kernel messages)"

param() { cat "$ROOT/sys/module/earc_route/parameters/$1" 2>/dev/null; }
if [ -z "$options" ]; then
  # Partly patched, it stays loaded with enabled=0: a failure, not a session.
  if [ "$status" = 0 ] && [ "$(param enabled)" = 1 ]; then
    echo "earc-route: PASSTHROUGH ACTIVE (until reboot); EXPERIMENTAL ($last)"; exit 0
  fi
  echo "earc-route: PASSTHROUGH FAILED (insmod status $status: $result): $last"; exit 1
fi
case "$want_mode" in
  0)
    case "$result" in
      *"Operation canceled"*) echo "earc-route: AUDIT OK: decoded and unloaded; dmesg | grep earc_route: has every value ($last)" ;;
      *) echo "earc-route: REFUSED by the module (insmod status $status: $result): $last"; exit 1 ;;
    esac ;;
  7)
    # hd_probes_registered is 0 where the stock kernel already accepts and
    # names codec 7 (no _cb_output_status probes are needed), else 2.
    hd=$(param hd_probes_registered)
    if [ "$status" = 0 ] && [ "$(param probe_registered)" = 1 ] && { [ "$hd" = 2 ] || [ "$hd" = 0 ]; }; then
      echo "earc-route: PASSTHROUGH ACTIVE ($options) selftest=$(param selftest_passed) gate_open=$(param gate_open); EXPERIMENTAL ($last)"
    else echo "earc-route: PASSTHROUGH FAILED (insmod status $status: $result): $last"; exit 1; fi ;;
esac
