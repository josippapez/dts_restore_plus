// node make-image.cjs CAPTURE WORK: a bootable copy of CAPTURE/kernel.bin whose BASE_Init
// branches to the harness (placed over the unused GFX_InitCfg), with three hardware leaves
// stubbed. Every address comes from CAPTURE/kallsyms.txt; the module's functions stay intact.
const fs = require('node:fs');
const assert = require('node:assert/strict');

const [capture, work] = process.argv.slice(2);
const original = fs.readFileSync(capture + '/kernel.bin');
const copy = Buffer.from(original);
const symbols = fs.readFileSync(capture + '/kallsyms.txt', 'utf8').trim().split('\n')
    .map(line => line.trim().split(/\s+/)).filter(f => f.length === 3)
    .map(([address, , name]) => ({address: BigInt('0x' + address), name}))
    .sort((a, b) => (a.address < b.address ? -1 : a.address > b.address ? 1 : 0));
const named = new Map();
for (const s of symbols) named.set(s.name, named.has(s.name) ? null : s.address);
const at = name => { assert.ok(named.get(name), name); return named.get(name); };
const end = name => symbols.find(s => s.address > at(name)).address;
const base = at('_text');
const offset = address => Number(address - base);

const harness = new Map(fs.readFileSync(work + '/harness-symbols.txt', 'utf8').trim().split('\n')
    .map(line => line.trim().split(/\s+/)).map(([address, , name]) => [name, BigInt('0x' + address)]));
const entry = at('BASE_Init'), target = harness.get('harness_entry');
assert.ok(target >= at('GFX_InitCfg') && target < at('GFX_InitCfg') + 64n, 'harness not linked at GFX_InitCfg');
const code = fs.readFileSync(work + '/harness.bin');
const covered = symbols.filter(s => s.address >= target && s.address < target + BigInt(code.length));
const gfx = s => /gfx/i.test(s.name);
assert.ok(covered.every(gfx), 'harness overwrites non-GFX code: ' + covered.filter(s => !gfx(s)).map(s => s.name));
assert.equal(original.readUInt32LE(offset(entry)), 0xa9b87bfd, 'BASE_Init does not start with stp x29, x30, [sp, #-128]!');
copy.writeUInt32LE((0x14000000 | (Number((target - entry) / 4n) & 0x3ffffff)) >>> 0, offset(entry));
code.copy(copy, offset(target));
const stubs = [['AUDIO_IMC_SendCmdParam', 'harness_imc'], ['AUDIO_IMC_SendCmd', 'harness_noop'],
    ['LOGM_WriteLog', 'harness_noop']];
for (const [name, replacement] of stubs) {
    assert.ok(harness.get(replacement), replacement);
    copy.writeUInt32LE(0x58000050, offset(at(name)));
    copy.writeUInt32LE(0xd61f0200, offset(at(name)) + 4);
    copy.writeBigUInt64LE(harness.get(replacement), offset(at(name)) + 8);
}
fs.writeFileSync(work + '/kernel-real.bin', copy);
console.log(`emulation image: ${code.length} harness bytes at GFX_InitCfg, ${stubs.length} hardware leaf stubs`);
