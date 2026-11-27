#!/usr/bin/env bash
# Builds the test's program (clang -pie) and library (clang -shared), kept next to their sources.
set -euo pipefail
cd "$(dirname "$0")"
CC="${WASM_CC:-/opt/homebrew/opt/llvm/bin/clang}"
LD="${WASM_LD:-wasm-ld}"
pic=()
# grep without -q reads the whole help: with pipefail an early exit would count as no.
if "$LD" --help 2>&1 | grep -- '--experimental-pic' >/dev/null; then pic+=(--experimental-pic); fi
"$CC" --target=wasm32 -nostdlib -fPIC -O0 -c library.c -o library.o
"$LD" -m wasm32 ${pic[@]+"${pic[@]}"} --shared --no-entry --unresolved-symbols=import-dynamic \
    --export=library_add --export=library_name --export=library_fill --export=library_apply \
    --export=library_increment --export=library_work -o library.so library.o
"$CC" --target=wasm32 -nostdlib -fPIC -O0 -c program.c -o program.o
"$LD" -m wasm32 ${pic[@]+"${pic[@]}"} --pie --no-entry --export=app_main --export=app_name_char \
    --export=app_struct --export=app_callback --export=app_counter --export=app_work_only --export=app_saved --export=_start \
    -o program.wasm program.o library.so
rm -f library.o program.o
