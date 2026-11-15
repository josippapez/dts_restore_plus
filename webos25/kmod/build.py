"""Build earc_route-<generation>.ko and check it against private kernel captures.

usage: build.py --output DIR CAPTURE_DIR...

Each capture directory holds kernel.bin (raw arm64 Image), kallsyms.txt (live
/proc/kallsyms, so 0x10000 below the survey capture on 6.12) and
stock-module.ko (an unmodified module from that TV). The generation of a
capture, 5.4.268 or 6.12.44, comes from its stock module's vermagic; one module
is built per generation that has a capture. The modules themselves do not
depend on the captures: they prove each module's layout matches the stock
modules of its generation and that every symbol it imports is exported by each
kernel of that generation. Run where
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

# Per kernel generation: vermagic, struct module (size, init offset, exit offset)
# of the stock aspectratiodrv.ko, the kernel's name for printk, and the
# -D macros earc_route.c takes. Neither generation uses MODVERSIONS or BTI.
# kprobe: (sizeof struct kprobe, offset of its last field `flags`) of each
# generation, measured from register_kprobe/arch_prepare_kprobe in the captures
# (5.4: flags +0x80, size 0x88; 6.12: flags +0x78, size 0x80 after fault_handler
# was dropped). The module's _Static_asserts, compiled per generation with these,
# are the layout check; the captures hold no kprobe instance to measure directly.
GENERATIONS = {
    '5.4': {'vermagic': '5.4.268 SMP preempt mod_unload aarch64', 'abi': (0x380, 0x150, 0x338),
            'printk': 'printk', 'kprobe': (0x88, 0x80)},
    '6.12': {'vermagic': '6.12.44 SMP preempt mod_unload aarch64', 'abi': (0x4c0, 0x138, 0x480),
             'printk': '_printk', 'kprobe': (0x80, 0x78)},
}
# The active modes (mode>=2) call these five directly; each is proven exported by
# every kernel of the generation (prel32_exports). register_kernel_break_hook,
# unregister_kernel_break_hook and aarch64_insn_patch_text are NOT exported and
# are called through kallsyms addresses, so they are not imports.
IMPORTS = {'mutex_lock', 'mutex_unlock', 'msleep', 'kthread_create_on_node', 'wake_up_process',
           'kthread_stop', 'kthread_should_stop', 'param_set_uint', 'param_get_uint',
           'param_set_ulong', 'param_get_ulong',
           'register_kprobe', 'unregister_kprobe', 'cpus_read_lock', 'cpus_read_unlock', 'synchronize_rcu'}

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


def link_base(image, symbols):
    """The address of file offset 0: (_stext & ~0xfffff) + the Image header's
    text_offset (0x88000 on 5.4, 0 on 6.12). No KASLR on these TVs."""
    if symbols.get('_stext', [0]) == [0] or len(symbols['_stext']) != 1:
        raise SystemExit('kallsyms has no single _stext')
    text_offset, = struct.unpack_from('<Q', image, 8)
    return (symbols['_stext'][0] & ~0xfffff) + text_offset


def prel32_exports(image, symbols, names, base):
    """Each name whose PREL32 ksymtab entry {value, name, namespace} points at
    its kallsyms address (CONFIG_HAVE_ARCH_PREL32_RELOCATIONS, 5.4 and 6.12)
    and whose name string, which the linker may merge as the tail of a longer
    one (6.12: mutex_lock inside ww_mutex_lock), spells it exactly."""
    wanted = {symbols[name][0] - base: name for name in names if len(symbols.get(name, [])) == 1}
    exports = {}
    for index, (relative,) in enumerate(struct.iter_unpack('<i', image[:len(image) // 4 * 4])):
        at = index * 4
        name = wanted.get(at + relative)
        if name and at + 12 <= len(image):
            string = at + 4 + struct.unpack_from('<i', image, at + 4)[0]
            namespace = at + 8 + struct.unpack_from('<i', image, at + 8)[0]
            if image[string:string + len(name) + 1] == name.encode() + b'\0' and 0 <= namespace < len(image) and image[namespace] == 0:
                exports[name] = hex(base + at + relative)
    return exports


def define(name, value):
    return '-D' + name + '=' + str(value)


def generation_of(capture):
    stock = module_abi(capture / 'stock-module.ko')
    for name, generation in GENERATIONS.items():
        if stock['vermagic'] == [generation['vermagic']]:
            expected = {'size': generation['abi'][0], 'vermagic': [generation['vermagic']],
                        'init_module': generation['abi'][1], 'cleanup_module': generation['abi'][2]}
            if stock != expected:
                raise SystemExit('Stock module ABI differs in ' + str(capture) + ': ' + repr(stock))
            return name, expected
    raise SystemExit('Stock module of ' + str(capture) + ' has an unknown vermagic: ' + repr(stock['vermagic']))


by_generation = {}
for capture in args.captures:
    name, expected = generation_of(capture)
    by_generation.setdefault(name, (expected, []))[1].append(capture)

for name, (expected_abi, captures) in sorted(by_generation.items()):
    generation = GENERATIONS[name]
    size, init, exit_ = generation['abi']
    module = args.output / ('earc_route-' + name + '.ko')
    optimized = work / ('earc_route-' + name + '.optimized')
    subprocess.run(['aarch64-linux-gnu-gcc', '-c', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fno-stack-protector', '-fno-pic', '-fno-pie', '-fno-asynchronous-unwind-tables',
                    '-ffreestanding', '-fno-builtin', '-mgeneral-regs-only', '-mno-outline-atomics',
                    define('EARC_MODULE_SIZE', hex(size)), define('EARC_MODULE_INIT', hex(init)),
                    define('EARC_MODULE_EXIT', hex(exit_)), define('EARC_VERMAGIC', '"' + generation['vermagic'] + '"'),
                    define('EARC_PRINTK_SYMBOL', '"' + generation['printk'] + '"'),
                    define('EARC_KPROBE_SIZE', hex(generation['kprobe'][0])),
                    define('EARC_KPROBE_FLAGS', hex(generation['kprobe'][1])),
                    '-fdump-tree-optimized=' + str(optimized), str(source), '-o', str(module)], check=True)
    # GCC 12 once replaced a call through a constant kernel address, reached via
    # a function-pointer parameter, with __builtin_unreachable; the function then
    # ran off its end. No such path may survive in a module.
    if re.search(r'__builtin_(unreachable|trap)', optimized.read_text()):
        raise SystemExit('Compiler inserted an unreachable path: ' + str(optimized))
    actual = module_abi(module)
    if actual != expected_abi:
        raise SystemExit('Built ' + name + ' module ABI differs from stock: ' + repr(actual))
    undefined = subprocess.check_output(['aarch64-linux-gnu-nm', '-u', str(module)], text=True)
    imports = {line.split()[-1] for line in undefined.splitlines() if line.strip()}
    allowed = IMPORTS | {generation['printk']}
    if imports - allowed or generation['printk'] not in imports:
        raise SystemExit('Unexpected unresolved imports in ' + name + ' module: ' + repr(sorted(imports ^ allowed)))

    kernels = {}
    for capture in captures:
        image = (capture / 'kernel.bin').read_bytes()
        symbols = kallsyms(capture / 'kallsyms.txt')
        exports = prel32_exports(image, symbols, imports, link_base(image, symbols))
        if set(exports) != imports:
            raise SystemExit('Imports not exported by ' + str(capture) + ': ' + repr(sorted(imports - set(exports))))
        kernels[hashlib.sha256(image).hexdigest()] = exports

    manifest = {'module': module.name, 'generation': name, 'moduleSha256': hashlib.sha256(module.read_bytes()).hexdigest(),
                'vermagic': generation['vermagic'], 'abi': {key: hex(value) if isinstance(value, int) else value
                                                            for key, value in actual.items()},
                'imports': sorted(imports), 'exportsByKernelSha256': kernels}
    (args.output / ('earc_route-' + name + '.json')).write_text(json.dumps(manifest, indent=2) + '\n')
    print('Built', module, manifest['moduleSha256'])
    print(name + ': stock ABI matched in', len(captures), 'capture(s); imports exported by every kernel:',
          ', '.join(sorted(imports)))
