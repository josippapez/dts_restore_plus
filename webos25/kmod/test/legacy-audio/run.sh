#!/bin/sh
set -eu

# target/legacy-audio on each 4.4 capture: check.c on the host (shapes, derived data), then
# earc_route-4.4.ko loaded into the capture's own kernel in QEMU.
#   sh test/legacy-audio/run.sh [CAPTURE...]   default: every test/captures/ capture with a 4.4 kernel
here=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
kmod=$(CDPATH= cd -- "$here/../.." && pwd)
if [ $# -eq 0 ]; then
    for dir in "$kmod"/test/captures/*/; do
        LC_ALL=C grep -aq 'Linux version 4\.4\.' "$dir/kernel.bin" 2>/dev/null && set -- "$@" "${dir%/}"
    done
fi
[ $# -gt 0 ] || { echo "SKIP: no 4.4 captures"; exit 0; }
mkdir -p "$kmod/out/test-legacy-audio"
${CC:-cc} -std=c99 -D_POSIX_C_SOURCE=200809L -O2 -Wall -Wextra -Werror -o "$kmod/out/test-legacy-audio/check" "$here/check.c"
"$kmod/out/test-legacy-audio/check" "$@"
EARC_BUILD_IMAGE=${EARC_BUILD_IMAGE:-dts-kmod-native} bash "$kmod/build.sh" "$@"
for capture in "$@"; do
    name=$(basename "$capture") work=$kmod/out/test-legacy-audio/$name
    mkdir -p "$work"
    awk 'NF == 3 { if (seen[$3]++) dup[$3] = 1; at[$3] = $1 }
         END { for (n in at) if (!dup[n] && n ~ /^[A-Za-z_][A-Za-z0-9_]*$/) print n " = 0x" at[n] ";" }' \
        "$capture/kallsyms.txt" > "$work/kernel-symbols.ld"
    docker run --rm -v "$here:/src:ro" -v "$work:/work" -v "$kmod:/kmod:ro" dts-kmod-native sh -eu -c '
        cd /work
        flags="-O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -fno-asynchronous-unwind-tables -mgeneral-regs-only -mno-outline-atomics -mcmodel=large"
        aarch64-linux-gnu-gcc -c $flags /src/harness.c -o harness.o
        aarch64-linux-gnu-gcc -c /src/harness.S -o harness-blob.o
        aarch64-linux-gnu-ld -T /src/harness.ld -T kernel-symbols.ld harness.o harness-blob.o -o harness.elf
        aarch64-linux-gnu-nm -n harness.elf > harness-symbols.txt
        aarch64-linux-gnu-objcopy -O binary harness.elf harness.bin
    '
    node "$here/make-image.cjs" "$capture" "$work"
    for cpus in 1 4; do
        log="$work/real-${cpus}cpu.log"
        result=0
        gtimeout --signal=TERM --kill-after=3 30 qemu-system-aarch64 \
            -machine virt,gic-version=2 -cpu cortex-a53 -accel tcg \
            -m 1024 -smp "$cpus" -display none -monitor none -serial stdio -nic none \
            -kernel "$work/kernel-real.bin" \
            -append 'console=ttyAMA0 earlycon=pl011,0x09000000 loglevel=8 ignore_loglevel panic=0' \
            < /dev/null > "$log" 2>&1 || result=$?
        grep -E 'LEGACY_AUDIO: (FAIL|stress|PASS)|earc_route:|Kernel panic|Internal error|Unable to handle' "$log" || true
        [ "$result" -eq 0 ] || { printf 'FAIL: QEMU exit=%s (%s, %s CPUs)\n' "$result" "$name" "$cpus"; exit 1; }
        grep -q 'LEGACY_AUDIO: PASS failures=0' "$log" || { printf 'FAIL: %s, %s CPUs\n' "$name" "$cpus"; exit 1; }
        if grep -Eq 'Kernel panic|Internal error|Unable to handle' "$log"; then exit 1; fi
        printf 'PASS: earc_route-4.4.ko on %s, %s CPUs\n' "$name" "$cpus"
    done
done
