#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build the shader corpus (#649) locally. For every kernel in shader-transpiler/corpus/hlsl:
#   <name>.xs.cso / .xs.lst   console container (DXIL + ACCC/FSPX) and listing, from xdxc
#   <name>.pix.lst            console ISA listing with encodings, from the PIX compiler
#   <name>.pc.cso             DXIL from the public PC DXC, for a D3DMetal reference run
#   <name>.code.bin / .aco.s  console machine code extracted from the ACCC chunk, and its
#                             llvm-mc gfx1030 disassembly
# Outputs go to $OUT (default target/shader-corpus, which git ignores) and must not be
# committed: they are compiler outputs from authorized GDK tools (see the corpus README).
#
# Required environment:
#   WINE      a CrossOver/Wine loader that accepts --bottle (CrossOver's bin/wine)
#   BOTTLE    a scratch bottle for the compilers (never a title's bottle)
#   XDXC      the console xdxc.exe (GDK, xbox/bin/gen9)
#   PIX_DXC   the console PIX dxc.exe (GDK, bin/Scarlett/pixsc)
#   DXC       the PC dxc.exe from DXC release v1.9.2609
#   LLVM_MC   llvm-mc built with the AMDGPU target
set -euo pipefail

for var in WINE BOTTLE XDXC PIX_DXC DXC LLVM_MC; do
    if [ -z "${!var:-}" ]; then
        echo "error: set $var (see the header of $0)" >&2
        exit 2
    fi
done

root=$(cd "$(dirname "$0")/../.." && pwd)
src="$root/shader-transpiler/corpus/hlsl"
out=${OUT:-$root/target/shader-corpus}
mkdir -p "$out"

# Wine takes Windows paths; Z: maps the host root.
win() { printf 'Z:%s' "$1"; }
compile() { WINEDEBUG=-all "$WINE" --bottle "$BOTTLE" "$@"; }

failures=0
for hlsl in "$src"/*.hlsl; do
    name=$(basename "$hlsl" .hlsl)
    log="$out/$name.log"
    : > "$log"
    if ! compile "$XDXC" -T cs_6_6 -E main -Fo "$(win "$out/$name.xs.cso")" -Fc "$(win "$out/$name.xs.lst")" \
            "$(win "$hlsl")" >>"$log" 2>&1; then
        echo "FAIL $name: xdxc (see $log)"; failures=$((failures + 1)); continue
    fi
    if ! compile "$PIX_DXC" -T cs_6_6 -E main -Fc "$(win "$out/$name.pix.lst")" "$(win "$hlsl")" >>"$log" 2>&1; then
        echo "FAIL $name: PIX dxc (see $log)"; failures=$((failures + 1)); continue
    fi
    if ! compile "$DXC" -nologo -T cs_6_6 -E main -Fo "$(win "$out/$name.pc.cso")" "$(win "$hlsl")" >>"$log" 2>&1; then
        echo "FAIL $name: PC dxc (see $log)"; failures=$((failures + 1)); continue
    fi
    if ! python3 "$root/scripts/shader-corpus/extract_code.py" "$out/$name.xs.cso" \
            --bin "$out/$name.code.bin" --asm "$out/$name.aco.s" --llvm-mc "$LLVM_MC"; then
        echo "FAIL $name: code extraction or decode (see $out/$name.aco.s)"; failures=$((failures + 1)); continue
    fi
done

if [ "$failures" -ne 0 ]; then
    echo "FAIL: shader corpus ($failures kernel(s) failed)"
    exit 1
fi
echo "PASS: shader corpus built in $out"
