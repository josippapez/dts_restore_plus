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
# Prints one result line. earc_route.ko must sit next to this script.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
KO="$HERE/earc_route.ko"
RELEASES="5.4.268-329.ptl4tv.5"

refuse() { echo "earc-route: REFUSED: $*"; exit 1; }

[ "$(id -u)" = 0 ] || refuse "must run as root"
release=$(uname -r)
case " $RELEASES " in *" $release "*) ;; *) refuse "kernel $release is not in the allowlist ($RELEASES)" ;; esac
[ -f "$KO" ] || refuse "$KO missing"
[ ! -e /sys/module/earc_route ] || refuse "earc_route is already loaded"
STOCK="/lib/modules/$release/kernel/aspectratiodrv.ko"
[ -f "$STOCK" ] || refuse "stock module $STOCK missing"

options="mode=0"
if [ $# -gt 0 ]; then
  [ "$1" = "--route" ] || refuse "usage: $0 [--route [renderer=0|1] [restore_pcm=0|1] [carrier_rate=48000|192000]]"
  shift
  options="mode=1"
  for option in "$@"; do
    case "$option" in
      renderer=0|renderer=1|restore_pcm=0|restore_pcm=1|carrier_rate=48000|carrier_rate=192000)
        options="$options $option" ;;
      *) refuse "unknown --route option: $option" ;;
    esac
  done
fi

# Checks both modules' ELF and reads /proc/kallsyms; prints the fn_/sz_
# parameters, or the reason for refusing.
parameters=$(python3 -I - "$STOCK" "$KO" <<'PY'
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
if stock[1:] != (0x380, 0x150, 0x338):
    fail('stock module layout is not 0x380 bytes with init at 0x150 and exit at 0x338: ' + repr(stock))
if ours != stock:
    fail('module ABI ' + repr(ours) + ' differs from the stock module ' + repr(stock))

wanted = {name for name, _ in FUNCTIONS} | {'_stext', '_etext'}
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
print(' '.join(output))
PY
) || refuse "$parameters"

# The kernel's own refusal reasons are in dmesg; keep only lines from this run.
before=$(dmesg | grep -c 'earc_route:' || true)
# shellcheck disable=SC2086 # options and parameters are word lists
if result=$(/sbin/insmod "$KO" $options $parameters 2>&1); then status=0; else status=$?; fi
last=$(dmesg | grep 'earc_route:' | tail -n 1 || true)
after=$(dmesg | grep -c 'earc_route:' || true)
[ "$after" != "$before" ] || last="(no earc_route kernel messages)"

case "$options" in
  mode=0)
    # Only the audit path returns -ECANCELED, after decoding everything.
    case "$result" in
      *"Operation canceled"*) echo "earc-route: AUDIT OK: decoded and unloaded; dmesg | grep earc_route: has every value ($last)" ;;
      *) echo "earc-route: REFUSED by the module (insmod status $status: $result): $last"; exit 1 ;;
    esac ;;
  *)
    if [ "$status" = 0 ] && [ "$(cat /sys/module/earc_route/parameters/active 2>/dev/null)" = 1 ]; then
      echo "earc-route: ROUTED ($options): $last"
    else
      echo "earc-route: ROUTE FAILED (insmod status $status: $result): $last"; exit 1
    fi ;;
esac
