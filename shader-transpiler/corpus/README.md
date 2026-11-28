# Shader corpus (Phase 0, #649)

A corpus of BetterXen-authored compute kernels compiled for the console, so the RDNA 2 shader
translator ([ADR 0008](../../docs/adr/0008-m2-shader-format-gate.md), Branch D2) can be tested
against inputs whose correct behaviour is known independently of the translator
([AGENTS.md](../../AGENTS.md) §5).

For each kernel in [`hlsl/`](hlsl), three compilers produce:

- the **console container** a title would ship: DXBC with `DXIL` **and** an `ACCC` chunk whose
  payload is an `FSPX` package holding the RDNA 2 code;
- **Microsoft's console ISA listing** for the same source, with encodings;
- **PC DXIL** from the public DXC, which runs on D3DMetal today and gives the reference result.

## What is committed and what is not

| Committed | Not committed |
|---|---|
| The HLSL kernels in `hlsl/` (original BetterXen code, GPL-3.0-or-later) | Every compiler output: containers, listings, extracted code and disassembly |
| [`scripts/shader-corpus/build.sh`](../../scripts/shader-corpus/build.sh) and [`extract_code.py`](../../scripts/shader-corpus/extract_code.py) with its unit tests | The GDK, its compilers, and any file from it |
| Layout facts derived from our own outputs (below) | |

The console compilers come from authorized GDK material and run on BetterXen's own HLSL only.
Their outputs stay in `target/shader-corpus/` (git-ignored) until the project confirms they may be
published. Run the compilers in a scratch bottle, never in a title's bottle.

## Building

```sh
WINE=/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wine \
BOTTLE=<scratch bottle> \
XDXC=<GDK>/…/xbox/bin/gen9/xdxc.exe \
PIX_DXC=<GDK>/…/bin/Scarlett/pixsc/dxc.exe \
DXC=<DXC v1.9.2609>/bin/x64/dxc.exe \
LLVM_MC=$(brew --prefix llvm)/bin/llvm-mc \
scripts/shader-corpus/build.sh
```

All eight kernels currently build, and every kernel's console code extracts and decodes with
LLVM's public `gfx1030` disassembler (2026-11-28, Apple Silicon, CrossOver 26.3):

| Kernel | What it exercises | Console instructions |
|---|---|---|
| `fill` | integer ALU | 11 |
| `branch` | divergent if/else | 17 |
| `loop` | per-lane trip count, early exit | 60 |
| `lds` | groupshared memory, barriers, tree reduction | 58 |
| `wave` | `WaveActiveSum`, `WavePrefixSum`, `WaveReadLaneFirst`, `WaveActiveBallot` | 47 |
| `atomic` | global `InterlockedAdd` / `InterlockedMax` with returned values | 57 |
| `floatmath` | `mad`, `sqrt`, `rcp`, float/uint conversion | 22 |
| `groupid` | 2D groups, `SV_GroupID`, `SV_GroupThreadID`, `SV_GroupIndex` | 17 |

`extract_code.py`'s unit tests use synthetic containers and need no GDK:
`python3 -m unittest scripts/shader-corpus/test_extract_code.py`.

## Layout facts (derived from our own outputs)

- **Container:** `xdxc` keeps the `DXIL` chunk and adds `ACCC`. Title containers can be `ACCC`-only;
  the `ACCC` format is the same. `bx-cli shader` rejects these containers with the same `FSPX`
  error as a title's console-only shaders, so the corpus is a faithful stand-in for that format.
- **`FSPX`:** a 16-byte header (magic `FSPX`, version `1`, section count, total size equal to the
  payload size), then one 16-byte entry per section: kind, offset from the start of the payload,
  size, and a reserved `0`. The corpus containers have four sections, of kinds 0, 1, 6 and 7.
- **Code:** section kind 6. It starts with `v_nop_xbox_data` instructions, a console-only VOP3
  opcode (top 16 bits `0xD580`) with one literal, 12 bytes each, that carry the shader hash. Every
  corpus kernel has three. They are followed by `gfx1030` machine code ending in `s_endpgm`, then
  padding.
- **Two console backends.** `xdxc`'s `ACCC` code comes from its ACO step; the PIX compiler's
  listing comes from its NewBE step. They generate different code for the same source (for example,
  the inline buffer descriptor's word 3 is `0x31016fac` from ACO and `0x11014fac` from NewBE). The
  NewBE listing documents the console ABI (#638); the ACO code is what `ACCC` containers carry.
  Both use the root UAV address in `s[4:5]` here.

## Observations so far

- `wave` compiles to the cross-lane instructions that matter for running 64-wide waves on 32-wide
  Apple GPUs: DPP `row_shr` and `quad_perm`, `v_permlanex16`, `v_readlane` and `v_readfirstlane`.
- **Open question:** in `wave`, LLVM decodes one instruction in the ballot sequence as
  `v_cmp_class_f16` with literal `0x7c01`, although the kernel has no half-precision code. Either
  ACO genuinely emits it there, or the console gives that opcode a meaning the public `gfx1030`
  decoder does not know. The reference runs below should tell.

## Next steps

1. A reference runner: dispatch each kernel's PC DXIL on D3DMetal with fixed inputs and record
   the output buffers as expected results, plus CPU-computed expectations where the result is a
   pure function of the inputs.
2. The decoder (Phase 1), checked instruction by instruction against LLVM's disassembly and the
   NewBE listings.
3. More kernels as the decoder grows: textures and samplers, structured control flow with nested
   loops, 64-bit atomics, and wave32 versus wave64 variants.
