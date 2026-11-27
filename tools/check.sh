#!/usr/bin/env bash
# Builds and runs the test on the kernel and the interpreter next to this
# repository: KERNEL (default ../wasm-rtos) and VM (default its vm/), under
# AddressSanitizer and UndefinedBehaviorSanitizer, then ASan alone (the
# predecoded loop) and with every memory paged.
set -uo pipefail
cd "$(dirname "$0")/.."
KERNEL="$(cd "${KERNEL:-../wasm-rtos}" && pwd)"
VM="$(cd "${VM:-$KERNEL/vm}" && pwd)"
out=build/kernel
rm -rf build && mkdir -p "$out/vm"
# The kernel sees this VM as its vm/ (it includes vm/source/wm.h).
for f in "$KERNEL"/*.c "$KERNEL"/*.h; do ln -s "$f" "$out/"; done
ln -s "$KERNEL/include" "$out/include"
ln -s "$KERNEL/hw" "$out/hw"
ln -s "$VM/source" "$out/vm/source"
fail=0
for mode in "-fsanitize=address,undefined" "-fsanitize=address" "-fsanitize=address -DWM_MEMORY_ALWAYS_PAGED=1"; do
    # shellcheck disable=SC2086
    if ! cc -std=c11 -O1 -g $mode -w -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L -I. -I"$out" -I"$VM/source" \
            test/test.c dylink.c os_dylink.c "$out"/*.c "$out"/hw/*.c "$VM"/source/wm_*.c -lm -o build/test 2>build/cc.txt; then
        echo "test:  FAIL (does not build: $(grep -m1 error build/cc.txt)) $mode"; fail=1; continue
    fi
    build/test >build/out.txt 2>&1 && echo "test:  ok ($(grep -c ^PASS build/out.txt) checks) $mode" ||
        { echo "test:  FAIL $mode"; grep -v ^PASS build/out.txt | head -20; fail=1; }
done
exit $fail
