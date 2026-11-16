#!/bin/sh
# run.sh -- host test of earc-route-load.sh's kernel check: the module is
# chosen from the running kernel's own stock modules, not from a release
# allowlist. Stock modules and earc_route-*.ko are generated ELF stand-ins
# (vermagic and struct module layout only); id, uname, insmod and dmesg are
# stubs; EARC_LOADER_ROOT stands in for /. No TV, no real module.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
LOADER="$HERE/../../earc-route-load.sh"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
FAILURES=0
CASES=0

V54='5.4.268 SMP preempt mod_unload aarch64'
V612='6.12.44 SMP preempt mod_unload aarch64'

# The loader's two function lists must equal the module's X-macro lists.
python3 -I - "$LOADER" "$HERE/../../earc_decode.h" "$HERE/../../earc_hdmi.h" <<'PY' || exit 1
import re, sys
loader, decode, hdmi = (open(p).read() for p in sys.argv[1:4])
def header(text, macro):
    body = re.search(r'#define ' + macro + r'\(X\)(.*?)\n\n', text, re.S).group(1)
    return [(n, int(a)) for n, a in re.findall(r'X\((\w+), ([01])\)', body)]
def script(name):
    body = re.search(name + r' = \[(.*?)\]\n', loader, re.S).group(1)
    return [(n, int(a)) for n, a in re.findall(r"\('(\w+)', ([01])\)", body)]
for macro, text, name in (('EARC_FUNCTIONS', decode, 'FUNCTIONS'), ('EARC_HDMI_FUNCTIONS', hdmi, 'HDMI_FUNCTIONS')):
    if header(text, macro) != script(name):
        print('FAIL loader %s differs from %s' % (name, macro))
        sys.exit(1)
from pathlib import Path
module = Path(sys.argv[3]).with_name('earc_route.c').read_text()
handler = module.split('static int input_property_prehandler(', 1)[1].split('\n}', 1)[0]
assert re.search(r'\|\|\s*regs->regs\[hdmi.input_forced_reg\]\) return 0;', handler.split('earc_input_property(', 1)[0]), 'forced PCM must return before property rewriting'
print('PASS loader function lists equal EARC_FUNCTIONS and EARC_HDMI_FUNCTIONS')
print('PASS forced PCM bypasses property rewriting')
PY

# fake_module PATH VERMAGIC SIZE INIT EXIT
fake_module() {
  python3 -I - "$@" <<'PY'
import struct, sys
path, magic, size, init, exit_ = sys.argv[1], sys.argv[2], int(sys.argv[3], 16), int(sys.argv[4], 16), int(sys.argv[5], 16)
names = [b'', b'.modinfo', b'.gnu.linkonce.this_module', b'.symtab', b'.strtab', b'.rela.gnu.linkonce.this_module', b'.shstrtab']
shstr = b'\0'.join(names) + b'\0'
offs = {}
pos = 0
for n in names:
    offs[n] = pos
    pos += len(n) + 1
modinfo = b'license=GPL\0vermagic=' + magic.encode() + b'\0'
strtab = b'\0init_module\0cleanup_module\0'
symtab = bytes(24) + struct.pack('<IBBHQQ', 1, 0x12, 0, 0, 0, 0) + struct.pack('<IBBHQQ', 13, 0x12, 0, 0, 0, 0)
rela = struct.pack('<QQq', init, (1 << 32) | 257, 0) + struct.pack('<QQq', exit_, (2 << 32) | 257, 0)
body = b''
placed = {}
for name, blob in ((b'.modinfo', modinfo), (b'.symtab', symtab), (b'.strtab', strtab),
                   (b'.rela.gnu.linkonce.this_module', rela), (b'.shstrtab', shstr)):
    placed[name] = (64 + len(body), len(blob))
    body += blob
shoff = 64 + len(body)
def sh(name, kind, off, sz, link=0, info=0, entsize=0):
    return struct.pack('<IIQQQQIIQQ', offs[name], kind, 0, 0, off, sz, link, info, 8, entsize)
headers = bytes(64)
headers += sh(b'.modinfo', 1, *placed[b'.modinfo'])
headers += sh(b'.gnu.linkonce.this_module', 8, shoff, size)
headers += sh(b'.symtab', 2, *placed[b'.symtab'], link=4, info=1, entsize=24)
headers += sh(b'.strtab', 3, *placed[b'.strtab'])
headers += sh(b'.rela.gnu.linkonce.this_module', 4, *placed[b'.rela.gnu.linkonce.this_module'], link=3, info=2, entsize=24)
headers += sh(b'.shstrtab', 3, *placed[b'.shstrtab'])
elf = bytearray(64)
elf[:16] = b'\x7fELF\x02\x01\x01' + bytes(9)
struct.pack_into('<HHI', elf, 16, 1, 183, 1)
struct.pack_into('<Q', elf, 0x28, shoff)
struct.pack_into('<HHHHHH', elf, 0x34, 64, 0, 0, 64, 7, 6)
open(path, 'wb').write(bytes(elf) + body + headers)
PY
}

# setup RELEASE TREE STOCK_ABI... : a fresh root with stock modules under TREE,
# kallsyms with every symbol the loader requires, and both earc_route builds.
setup() {
  release=$1; tree=$2; shift 2
  rm -rf "$WORK/root" "$WORK/app" "$WORK/bin"
  mkdir -p "$WORK/root/proc" "$WORK/root/sys/module" "$WORK/app" "$WORK/bin" "$WORK/root$tree/$release/kernel"
  i=0
  while [ $# -ge 4 ]; do
    fake_module "$WORK/root$tree/$release/kernel/stock$i.ko" "$1" "$2" "$3" "$4"
    i=$((i + 1)); shift 4
  done
  cp "$LOADER" "$WORK/app/earc-route-load.sh"
  fake_module "$WORK/app/earc_route-5.4.ko" "$V54" 0x380 0x150 0x338
  fake_module "$WORK/app/earc_route-6.12.ko" "$V612" 0x4c0 0x138 0x480
  python3 -I - "$WORK/root/proc/kallsyms" "$WORK/app/earc-route-load.sh" <<'PY'
import re, sys
text = open(sys.argv[2]).read()
names = re.findall(r"\('([A-Za-z0-9_]+)', [01]\)", text) + ['_stext', '_etext']
with open(sys.argv[1], 'w') as out:
    for i, name in enumerate(sorted(set(names))):
        out.write('%x T %s\n' % (0xffffffc010100000 + 0x1000 * i, name))
    out.write('%x T zz_after\n' % (0xffffffc010100000 + 0x1000 * len(names) + 0x1000))
PY
  printf '#!/bin/sh\necho 0\n' > "$WORK/bin/id"
  printf '#!/bin/sh\necho %s\n' "$release" > "$WORK/bin/uname"
  printf '#!/bin/sh\necho "earc_route: stub"\n' > "$WORK/bin/dmesg"
  chmod +x "$WORK/bin/id" "$WORK/bin/uname" "$WORK/bin/dmesg"
}

# insmod_stub BODY: records its arguments, then runs BODY.
insmod_stub() {
  printf '#!/bin/sh\necho "$@" > %s/insmod.args\n%s\n' "$WORK" "$1" > "$WORK/bin/insmod"
  chmod +x "$WORK/bin/insmod"
}

# expect WHAT STATUS PATTERN [ARGS...]: run the loader, check status and output.
expect() {
  what=$1; want=$2; pattern=$3; shift 3
  CASES=$((CASES + 1))
  rm -f "$WORK/insmod.args"
  if out=$(PATH="$WORK/bin:$PATH" EARC_LOADER_ROOT="$WORK/root" ${LOADER_SHELL:-sh} "$WORK/app/earc-route-load.sh" "$@" 2>&1); then got=0; else got=1; fi
  if [ "$got" = "$want" ] && printf '%s\n' "$out" | grep -q -- "$pattern"; then
    echo "PASS $what"
  else
    echo "FAIL $what: status $got (want $want)"; printf '%s\n' "$out" | sed 's/^/    /'
    FAILURES=$((FAILURES + 1))
  fi
}
loaded() {  # loaded KO: the stub insmod was called with that module
  CASES=$((CASES + 1))
  if [ -f "$WORK/insmod.args" ] && grep -q "/$1 " "$WORK/insmod.args"; then echo "PASS   insmod got $1"
  else echo "FAIL   insmod did not get $1: $(cat "$WORK/insmod.args" 2>/dev/null || echo 'not called')"; FAILURES=$((FAILURES + 1)); fi
}
not_loaded() {
  CASES=$((CASES + 1))
  if [ ! -f "$WORK/insmod.args" ]; then echo "PASS   insmod not called"
  else echo "FAIL   insmod was called: $(cat "$WORK/insmod.args")"; FAILURES=$((FAILURES + 1)); fi
}

CANCEL='echo "insmod: ERROR: could not insert module: Operation canceled" >&2; exit 1'

# Releases never named in the loader: the kernel's own modules decide.
for release in 5.4.268-329.ptl4tv.5 5.4.268-320 5.4.268-329.ptl4tv.9; do
  setup "$release" /lib/modules "$V54" 0x380 0x150 0x338 "$V54" 0x380 0x150 0x338
  insmod_stub "$CANCEL"
  expect "5.4 kernel $release: audit loads the 5.4 build" 0 "AUDIT OK"
  loaded earc_route-5.4.ko
done
for release in 6.12.44-352 6.12.44-347; do
  setup "$release" /usr/lib/modules "$V612" 0x4c0 0x138 0x480
  insmod_stub "$CANCEL"
  expect "6.12 kernel $release: audit loads the 6.12 build" 0 "AUDIT OK"
  loaded earc_route-6.12.ko
done

setup 6.12.44-352 /usr/lib/modules "$V612" 0x4c0 0x138 0x478
insmod_stub "$CANCEL"
expect "C6 ABI without a matching build refuses" 1 "0 of .* match this kernel"
not_loaded
fake_module "$WORK/app/earc_route-6.12-c6.ko" "$V612" 0x4c0 0x138 0x478
expect "C6 ABI selects the C6 build, not the shared-release build" 0 "AUDIT OK"
loaded earc_route-6.12-c6.ko

setup 6.12.44-352 /usr/lib/modules "$V612" 0x4c0 0x138 0x480
fake_module "$WORK/app/earc_route-6.12-c6.ko" "$V612" 0x4c0 0x138 0x478
insmod_stub "$CANCEL"
expect "original 6.12 ABI still selects the original build beside C6" 0 "AUDIT OK"
loaded earc_route-6.12.ko

setup 6.12.44-352 /usr/lib/modules "$V612" 0x4c0 0x138 0x480
mkdir -p "$WORK/root/sys/module/earc_route"
insmod_stub 'echo called > /dev/null'
expect "--check names the build without insmod, even while loaded" 0 "CHECK OK: kernel 6.12.44-352 takes earc_route-6.12.ko" --check
not_loaded
setup 5.4.268-329.ptl4tv.5 /lib/modules "$V54" 0x388 0x150 0x338
insmod_stub "$CANCEL"
expect "--check refuses a kernel no build fits" 1 "0 of .* match this kernel" --check
not_loaded

setup 5.10.0-1 /lib/modules '5.10.0 SMP preempt mod_unload aarch64' 0x3c0 0x150 0x370
insmod_stub "$CANCEL"
expect "unknown kernel generation refuses" 1 "0 of earc_route-5.4.ko, earc_route-6.12.ko match this kernel"
not_loaded

setup 5.4.268-999 /lib/modules "$V54" 0x388 0x150 0x338
insmod_stub "$CANCEL"
expect "5.4 vermagic with a different struct module size refuses" 1 "0 of .* match this kernel"
not_loaded

setup 5.4.268-329.ptl4tv.5 /lib/modules "$V54" 0x380 0x150 0x338 "$V54" 0x388 0x150 0x338
insmod_stub "$CANCEL"
expect "stock modules that disagree refuse" 1 "stock modules disagree"
not_loaded

setup 5.4.268-329.ptl4tv.5 /lib/modules
insmod_stub "$CANCEL"
expect "no stock module refuses" 1 "no stock module under"
not_loaded

setup 5.4.268-329.ptl4tv.5 /lib/modules "$V54" 0x380 0x150 0x338
grep -v -e ' _set_output$' -e ' bm_connect_block$' "$WORK/root/proc/kallsyms" > "$WORK/kallsyms" && mv "$WORK/kallsyms" "$WORK/root/proc/kallsyms"
insmod_stub "$CANCEL"
expect "a required symbol missing from kallsyms refuses" 1 "bm_connect_block appears 0 times"
not_loaded

setup 5.4.268-329.ptl4tv.5 /lib/modules "$V54" 0x380 0x150 0x338
mkdir -p "$WORK/root/sys/module/earc_route"
insmod_stub "$CANCEL"
expect "already loaded refuses" 1 "earc_route is already loaded"
not_loaded

setup 5.4.268-329.ptl4tv.5 /lib/modules "$V54" 0x380 0x150 0x338
mkdir -p "$WORK/root/sys/module/dts_gate"
insmod_stub "$CANCEL"
expect "another DTS tool refuses an active mode" 1 "another DTS tool is loaded: dts_gate" --dts-hd
not_loaded

# Mode 7 on a native kernel: no _cb_output_status probes (hd_probes_registered=0).
for hd in 2 0; do
  setup 5.4.268-329.ptl4tv.5 /lib/modules "$V54" 0x380 0x150 0x338
  insmod_stub "p=$WORK/root/sys/module/earc_route/parameters; mkdir -p \$p; echo 1 > \$p/probe_registered; echo $hd > \$p/hd_probes_registered; echo 1 > \$p/gate_open; echo 1 > \$p/selftest_passed"
  expect "mode 7 success with hd_probes_registered=$hd" 0 "PASSTHROUGH ACTIVE (mode=7" --dts-hd
  loaded earc_route-5.4.ko
done
setup 5.4.268-329.ptl4tv.5 /lib/modules "$V54" 0x380 0x150 0x338
insmod_stub "p=$WORK/root/sys/module/earc_route/parameters; mkdir -p \$p; echo 1 > \$p/probe_registered; echo 1 > \$p/hd_probes_registered"
expect "mode 7 with one status probe only is a failure" 1 "PASSTHROUGH FAILED" --dts-hd

if [ "$FAILURES" = 0 ]; then echo "earc-route-load test passed: $CASES cases (${LOADER_SHELL:-sh} $(readlink -f "$(command -v "${LOADER_SHELL:-sh}")"))"
else echo "earc-route-load test FAILED: $FAILURES of $CASES"; exit 1; fi
