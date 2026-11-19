#!/bin/sh
# Floats have to come out bit-identical on every platform the sim runs on.
# This builds tools/det_check.cpp (det_math sweeps plus a scripted sim run,
# hashed) with every compiler and target it finds, runs them (other CPUs
# under qemu user mode), and compares each one's output with native GCC's.
#
#   tools/cross_check.sh
#
# Needs EnTT's sources (configure any preset first, or set ENTT_DIR).
# Other CPUs need their cross compiler and qemu, e.g. on Arch:
# aarch64-linux-gnu-gcc + qemu-user (qemu-aarch64).
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
entt=${ENTT_DIR:-$root/build/ci/_deps/entt-src/src}
out=${OUT_DIR:-$root/build/cross}
mkdir -p "$out"

if [ ! -d "$entt/entt" ]; then
  echo "EnTT not found at $entt (configure a preset, or set ENTT_DIR)" >&2
  exit 2
fi

srcs="$root/tools/det_check.cpp $(find "$root/src/sim" -name '*.cpp' | sort)"
# the sim's own flags that matter for floats, plus what it's built with
flags="-std=c++26 -ffp-contract=off -fno-exceptions -fno-rtti -I$root/src -isystem $entt"

# name, compiler, extra flags, runner (empty: run natively)
configs="
gcc-O2|g++|-O2|
gcc-O0|g++|-O0|
clang-O2|clang++|-O2|
aarch64-gcc-O2|aarch64-linux-gnu-g++|-O2 -static|qemu-aarch64
riscv64-gcc-O2|riscv64-linux-gnu-g++|-O2 -static|qemu-riscv64
"

reference="$out/gcc-O2.txt"
rm -f "$out/failed"
# (the loop runs in a subshell, so a failure is recorded as a file)
echo "$configs" | while IFS='|' read -r name cxx extra runner; do
  [ -n "$name" ] || continue
  if ! command -v "$cxx" >/dev/null 2>&1; then
    echo "skip  $name ($cxx not installed)"
    continue
  fi
  if [ -n "$runner" ] && ! command -v "$runner" >/dev/null 2>&1; then
    echo "skip  $name ($runner not installed)"
    continue
  fi
  # shellcheck disable=SC2086 # flags are meant to split
  "$cxx" $flags $extra $srcs -o "$out/$name"
  # shellcheck disable=SC2086
  $runner "$out/$name" >"$out/$name.txt"
  if [ "$name" = gcc-O2 ]; then
    echo "ref   $name"
  elif cmp -s "$reference" "$out/$name.txt"; then
    echo "same  $name"
  else
    echo "DIFF  $name:"
    diff "$reference" "$out/$name.txt" | sed 's/^/        /'
    touch "$out/failed"
  fi
done

if [ -e "$out/failed" ]; then
  rm "$out/failed"
  echo "floats differ between platforms"
  exit 1
fi
echo "all identical:"
sed 's/^/  /' "$reference"
