#!/bin/sh
# SPDX-License-Identifier: MIT
# Storage, file operations, arithmetic and runtime protocol without UI services.
set -eu

source_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="$source_dir/build-core-asan"
mkdir -p "$build_dir"
for name in core files j2me; do
  # pkg-config emits compiler/linker argument lists; splitting is intentional.
  cc -std=gnu11 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
    $(pkg-config --cflags gio-2.0) -I"$source_dir/src" \
    "$source_dir/src/note-store.c" "$source_dir/src/calculator.c" \
    "$source_dir/src/file-store.c" "$source_dir/src/j2me.c" "$source_dir/tests/test-$name.c" \
    -o "$build_dir/test-$name" $(pkg-config --libs gio-2.0) -lm
  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$build_dir/test-$name"
done
