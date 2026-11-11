#!/bin/sh
# earc-route-load.sh -- load earc_route.ko on a rooted LG webOS 25 TV.
#
#   sh earc-route-load.sh                    audit: decode, print, unload (mode=0)
#   sh earc-route-load.sh --route [renderer=0|1] [restore_pcm=0|1] [carrier_rate=48000|192000]
#
# Runs as root, on the TV only (busybox sh + python3). Refuses unless the
# kernel release is allowlisted, the stock module's vermagic and struct module
# layout equal ours, and every symbol the module needs appears exactly once in
# /proc/kallsyms with a non-zero address. The module then derives the rest from
# LG's code and refuses itself on any mismatch. One insmod, never retried.
# Prints one result line. earc_route-5.4.ko and earc_route-6.12.ko must sit
# next to this script; the release picks the one to load.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)

refuse() { echo "earc-route: REFUSED: $*"; exit 1; }

[ "$(id -u)" = 0 ] || refuse "must run as root"
# Allowlist: release -> module, stock module, struct module size, init and exit
# offsets (the same values build.py checks). Anything else is refused.
release=$(uname -r)
case "$release" in
  5.4.268-329.ptl4tv.5)
    KO="$HERE/earc_route-5.4.ko"
    STOCK="/lib/modules/$release/kernel/aspectratiodrv.ko"
    ABI="0x380 0x150 0x338" ;;
  6.12.44-352)
    KO="$HERE/earc_route-6.12.ko"
    STOCK="/usr/lib/modules/$release/kernel/aspectratiodrv.ko"
    ABI="0x4c0 0x138 0x480" ;;
  *) refuse "kernel $release is not in the allowlist (5.4.268-329.ptl4tv.5, 6.12.44-352)" ;;
esac
[ -f "$KO" ] || refuse "$KO missing"
[ ! -e /sys/module/earc_route ] || refuse "earc_route is already loaded"
[ -f "$STOCK" ] || refuse "stock module $STOCK missing"
INSMOD=$(command -v insmod || true)
if [ -z "$INSMOD" ] && [ -x /sbin/insmod ]; then INSMOD=/sbin/insmod; fi
[ -n "$INSMOD" ] || refuse "insmod not found"

options="mode=0"
want_mode=0
if [ $# -gt 0 ]; then
  case "$1" in
    --route)
      shift; options="mode=1"; want_mode=1
      for option in "$@"; do
        case "$option" in
          renderer=0|renderer=1|restore_pcm=0|restore_pcm=1|carrier_rate=48000|carrier_rate=192000)
            options="$options $option" ;;
          *) refuse "unknown --route option: $option" ;;
        esac
      done ;;
    --selftest)  shift; options="mode=2"; want_mode=2; [ $# -eq 0 ] || refuse "--selftest takes no options" ;;
    --open-gate) shift; options="mode=3"; want_mode=3; [ $# -eq 0 ] || refuse "--open-gate takes no options" ;;
    --observe)   shift; options="mode=4"; want_mode=4; [ $# -eq 0 ] || refuse "--observe takes no options" ;;
    --dts-core)
      shift; options="mode=5"; want_mode=5
      for option in "$@"; do
        case "$option" in
          window_seconds=*) [ "${option#window_seconds=}" -ge 0 ] 2>/dev/null || refuse "bad $option"; options="$options $option" ;;
          supervised=0|supervised=1) options="$options $option" ;;
          *) refuse "unknown --dts-core option: $option" ;;
        esac
      done ;;
    *) refuse "usage: $0 [--route ... | --selftest | --open-gate | --observe | --dts-core [window_seconds=N] [supervised=0|1]]" ;;
  esac
fi

# Active modes (>=1 that write; the HDMI passthrough modes 2-5) refuse to load
# beside another DTS tool, whose kprobes or gate change would collide with ours.
# The audit (mode 0) only reports them (below).
if [ "$want_mode" -ge 2 ]; then
  for mod in dts_gate dts_core_route gh_dts_native; do
    [ -e "/sys/module/$mod" ] && refuse "another DTS tool is loaded: $mod (unload it first)"
  done
  [ -f /var/lib/lg-dts-core/enabled ] && refuse "lg-dts-core is enabled (/var/lib/lg-dts-core/enabled); disable it first"
fi

# Checks both modules' ELF and reads /proc/kallsyms; prints the fn_/sz_
# parameters, or the reason for refusing.
parameters=$(python3 -I - "$STOCK" "$KO" $ABI <<'PY'
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
    ('register_kernel_break_hook', 1), ('call_break_hook', 1), ('unregister_kernel_break_hook', 0),
    ('kprobe_breakpoint_handler', 0), ('kprobe_breakpoint_ss_handler', 0), ('kretprobe_breakpoint_handler', 0),
    ('aarch64_insn_patch_text', 0), ('register_kprobe', 0), ('unregister_kprobe', 0),
    ('cpus_read_lock', 0), ('cpus_read_unlock', 0)]


def fail(reason):
    print(reason)
    sys.exit(1)


def module_abi(path):
    """(vermagic, this_module size, init offset, exit offset) of a 64-bit
    little-endian AArch64 relocatable module."""
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
    for name in ('.modinfo', '.gnu.linkonce.this_module', '.rela.gnu.linkonce.this_module'):
        if len(named.get(name, [])) != 1:
            fail(path + ': expected one ' + name + ' section')
    modinfo = named['.modinfo'][0]
    fields = data[modinfo[4]:modinfo[4] + modinfo[5]].split(b'\0')
    magic = [field[9:].decode() for field in fields if field.startswith(b'vermagic=')]
    if len(magic) != 1:
        fail(path + ': expected one vermagic')
    rela = named['.rela.gnu.linkonce.this_module'][0]
    symtab = sections[rela[6]]
    targets = {}
    for i in range(rela[5] // 24):
        offset, info, _ = struct.unpack_from('<QQq', data, rela[4] + 24 * i)
        name_offset, = struct.unpack_from('<I', data, symtab[4] + 24 * (info >> 32))
        targets.setdefault(string(symtab[6], name_offset), []).append(offset)
    if len(targets.get('init_module', [])) != 1 or len(targets.get('cleanup_module', [])) != 1:
        fail(path + ': init_module/cleanup_module relocations not found once each')
    return magic[0], named['.gnu.linkonce.this_module'][0][5], targets['init_module'][0], targets['cleanup_module'][0]


stock, ours = module_abi(sys.argv[1]), module_abi(sys.argv[2])
abi = tuple(int(value, 16) for value in sys.argv[3:6])
if stock[1:] != abi:
    fail('stock module layout is not %#x bytes with init at %#x and exit at %#x: %r' % (abi + (stock,)))
if ours != stock:
    fail('module ABI ' + repr(ours) + ' differs from the stock module ' + repr(stock))

wanted = {name for name, _ in FUNCTIONS} | {name for name, _ in HDMI_FUNCTIONS} | {'_stext', '_etext'}
found = {}
core = set()
with open('/proc/kallsyms') as kallsyms:
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
print(' '.join(output))
PY
) || refuse "$parameters"

# Report other DTS tools present (never a refusal): the audit reads live state,
# and another tool's kprobes/gate change what is seen. A tester greps one line.
report_tool() {  # report_tool TEXT
  echo "earc-route: other-tool: $1"
}
[ -d /var/lib/lg-dts-core ] && report_tool "lg-dts-core present at /var/lib/lg-dts-core (enabled=$([ -f /var/lib/lg-dts-core/enabled ] && echo yes || echo no))"
[ -d /var/lib/lg-dts-native ] && report_tool "glasshouse gh_dts_native present at /var/lib/lg-dts-native"
for mod in dts_gate dts_core_route gh_dts_native; do
  [ -e "/sys/module/$mod" ] && report_tool "module $mod is loaded"
done

# The kernel's own refusal reasons are in dmesg; keep only lines from this run.
before=$(dmesg | grep -c 'earc_route:' || true)
# shellcheck disable=SC2086 # options and parameters are word lists
if result=$("$INSMOD" "$KO" $options $parameters 2>&1); then status=0; else status=$?; fi
last=$(dmesg | grep 'earc_route:' | tail -n 1 || true)
after=$(dmesg | grep -c 'earc_route:' || true)
[ "$after" != "$before" ] || last="(no earc_route kernel messages)"

param() { cat "/sys/module/earc_route/parameters/$1" 2>/dev/null; }
case "$want_mode" in
  0|2)
    # The audit (0) and a PASSED self-test (2) return -ECANCELED and unload. A
    # refused or failed self-test returns a different code and lands below.
    case "$result" in
      *"Operation canceled"*)
        if [ "$want_mode" = 0 ]; then echo "earc-route: AUDIT OK: decoded and unloaded; dmesg | grep earc_route: has every value ($last)"
        else echo "earc-route: SELF-TEST PASSED and unloaded; dmesg | grep earc_route: has the result ($last)"; fi ;;
      *)
        if [ "$want_mode" = 0 ]; then echo "earc-route: REFUSED by the module (insmod status $status: $result): $last"
        else echo "earc-route: SELF-TEST did not pass (insmod status $status: $result): $last"; fi
        exit 1 ;;
    esac ;;
  1)
    if [ "$status" = 0 ] && [ "$(param active)" = 1 ]; then echo "earc-route: ROUTED ($options): $last"
    else echo "earc-route: ROUTE FAILED (insmod status $status: $result): $last"; exit 1; fi ;;
  3)
    if [ "$status" = 0 ] && [ "$(param gate_open)" = 1 ]; then echo "earc-route: DTS GATE OPEN (mode 3); unload earc_route to restore ($last)"
    else echo "earc-route: GATE OPEN FAILED (insmod status $status: $result): $last"; exit 1; fi ;;
  4|5)
    if [ "$status" = 0 ] && [ "$(param probe_registered)" = 1 ]; then
      echo "earc-route: PASSTHROUGH ACTIVE ($options) selftest=$(param selftest_passed) gate_open=$(param gate_open); UNTESTED ON HARDWARE ($last)"
    else echo "earc-route: PASSTHROUGH FAILED (insmod status $status: $result): $last"; exit 1; fi ;;
esac
