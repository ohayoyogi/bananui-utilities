#!/bin/sh
# SPDX-License-Identifier: MIT
# Storage/arithmetic checks independent of libbananui and device services.
set -eu

source_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="$source_dir/build-core-asan"
mkdir -p "$build_dir"
# pkg-config emits compiler/linker argument lists; word splitting is intentional.
cc -std=gnu11 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
  $(pkg-config --cflags gio-2.0) -I"$source_dir/src" \
  "$source_dir/src/note-store.c" "$source_dir/src/calculator.c" "$source_dir/tests/test-core.c" \
  -o "$build_dir/test-core" $(pkg-config --libs gio-2.0) -lm
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$build_dir/test-core"
