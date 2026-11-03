"""Build earc_route.ko and check it against private captures of 5.4.268 kernels.

usage: build.py --output DIR CAPTURE_DIR...

Each capture directory holds kernel.bin (raw arm64 Image), kallsyms.txt (live
/proc/kallsyms) and stock-module.ko (an unmodified module from that TV). The
module itself does not depend on them: they prove its layout matches the stock
modules and that every symbol it imports is exported by each kernel. Run where
aarch64-linux-gnu-{gcc,readelf,nm} exist (build.sh does this in a container).
Port of Glasshouse's build-target.py (native/dts-forwarding).
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

VERMAGIC = '5.4.268 SMP preempt mod_unload aarch64'
BASE = 0xffffffc010088000  # link base of the raw Image; no KASLR on these TVs
IMPORTS = {'printk', 'mutex_lock', 'mutex_unlock', 'msleep', 'kthread_create_on_node', 'wake_up_process',
           'kthread_stop', 'kthread_should_stop', 'param_set_uint', 'param_get_uint',
           'param_set_ulong', 'param_get_ulong'}

parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('captures', type=Path, nargs='+')
args = parser.parse_args()
source = Path(__file__).resolve().parent / 'earc_route.c'
work = args.output / 'build'
work.mkdir(parents=True, exist_ok=True)


def readelf(*arguments):
    return subprocess.check_output(['aarch64-linux-gnu-readelf', *map(str, arguments)], text=True)


def module_abi(path):
    sections = readelf('-SW', path)
    row = next(line for line in sections.splitlines() if '.gnu.linkonce.this_module ' in line and 'RELA' not in line)
    size = int(row.split('PROGBITS')[1].split()[2], 16)
    info = readelf('-p', '.modinfo', path)
    magic = [line.split('vermagic=')[1] for line in info.splitlines() if 'vermagic=' in line]
    relocations = readelf('-rW', path).split("Relocation section '.rela.gnu.linkonce.this_module'")[1]
    offsets = {name: int(re.search(r'^([0-9a-f]+).* ' + name + r' \+', relocations, re.M).group(1), 16)
               for name in ('init_module', 'cleanup_module')}
    return {'size': size, 'vermagic': magic, **offsets}


def kallsyms(path):
    symbols = {}
    for line in path.read_text().splitlines():
        fields = line.split()
        if len(fields) >= 3:
            symbols.setdefault(fields[2], []).append(int(fields[0], 16) if len(fields) == 3 else 0)
    return symbols


def prel32_exports(image, symbols, names):
    """Each name whose PREL32 ksymtab entry {value, name, namespace} points at
    its kallsyms address (Linux 5.4 CONFIG_HAVE_ARCH_PREL32_RELOCATIONS)."""
    positions = {}
    for name in names:
        needle = b'\0' + name.encode() + b'\0'
        at = image.find(needle)
        while at >= 0:
            positions[at + 1] = name
            at = image.find(needle, at + 1)
    exports = {}
    for index, (relative,) in enumerate(struct.iter_unpack('<i', image[:len(image) // 4 * 4])):
        at = index * 4
        name = positions.get(at + relative)
        if name and at >= 4 and at + 8 <= len(image):
            value = at - 4 + struct.unpack_from('<i', image, at - 4)[0]
            namespace = at + 4 + struct.unpack_from('<i', image, at + 4)[0]
            if symbols.get(name) == [BASE + value] and 0 <= namespace < len(image) and image[namespace] == 0:
                exports[name] = hex(BASE + value)
    return exports


expected_abi = {'size': 0x380, 'vermagic': [VERMAGIC], 'init_module': 0x150, 'cleanup_module': 0x338}
for capture in args.captures:
    stock = module_abi(capture / 'stock-module.ko')
    if stock != expected_abi:
        raise SystemExit('Stock module ABI differs in ' + str(capture) + ': ' + repr(stock))

module = args.output / 'earc_route.ko'
optimized = work / 'earc_route.optimized'
subprocess.run(['aarch64-linux-gnu-gcc', '-c', '-O2', '-Wall', '-Wextra', '-Werror',
                '-fno-stack-protector', '-fno-pic', '-fno-pie', '-fno-asynchronous-unwind-tables',
                '-ffreestanding', '-fno-builtin', '-mgeneral-regs-only', '-mno-outline-atomics',
                '-fdump-tree-optimized=' + str(optimized), str(source), '-o', str(module)], check=True)
# GCC 12 once replaced a call through a constant kernel address, reached via
# a function-pointer parameter, with __builtin_unreachable; the function then
# ran off its end. No such path may survive in a module.
if re.search(r'__builtin_(unreachable|trap)', optimized.read_text()):
    raise SystemExit('Compiler inserted an unreachable path: ' + str(optimized))
actual = module_abi(module)
if actual != expected_abi:
    raise SystemExit('Built module ABI differs from stock: ' + repr(actual))
undefined = subprocess.check_output(['aarch64-linux-gnu-nm', '-u', str(module)], text=True)
imports = {line.split()[-1] for line in undefined.splitlines() if line.strip()}
if imports - IMPORTS:
    raise SystemExit('Unexpected unresolved imports: ' + repr(sorted(imports - IMPORTS)))

kernels = {}
for capture in args.captures:
    image = (capture / 'kernel.bin').read_bytes()
    exports = prel32_exports(image, kallsyms(capture / 'kallsyms.txt'), imports)
    if set(exports) != imports:
        raise SystemExit('Imports not exported by ' + str(capture) + ': ' + repr(sorted(imports - set(exports))))
    kernels[hashlib.sha256(image).hexdigest()] = exports

manifest = {'module': 'earc_route.ko', 'moduleSha256': hashlib.sha256(module.read_bytes()).hexdigest(),
            'vermagic': VERMAGIC, 'abi': {key: hex(value) if isinstance(value, int) else value
                                          for key, value in actual.items()},
            'imports': sorted(imports), 'exportsByKernelSha256': kernels}
(args.output / 'earc_route.json').write_text(json.dumps(manifest, indent=2) + '\n')
print('Built', module, manifest['moduleSha256'])
print('Stock ABI matched in', len(args.captures), 'capture(s); imports exported by every kernel:', ', '.join(sorted(imports)))
