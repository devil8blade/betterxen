#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Extract the RDNA 2 code from a console-compiled DXBC container (#649).

The layout below was derived from containers the console compiler produced for BetterXen's own
corpus kernels (shader-transpiler/corpus/README.md). Every assumption is checked; anything that
does not match raises instead of guessing.

- DXBC: chunk count at byte 28, chunk offsets from byte 32; each chunk is a FourCC, a u32 size
  and the payload. The console code is in the `ACCC` chunk.
- FSPX (the ACCC payload): a 16-byte header (magic `FSPX`, version, section count, total size),
  then one 16-byte entry per section: kind, offset, size, reserved (0). Offsets are relative to
  the start of the payload.
- Section kind 6 holds the code. It starts with `v_nop_xbox_data` instructions (a VOP3 word
  whose top 16 bits are 0xD580, plus one literal: 12 bytes each) that carry the shader hash,
  followed by gfx1030 machine code ending in `s_endpgm`.
"""

import argparse
import struct
import subprocess
import sys

FSPX_MAGIC = b"FSPX"
CODE_SECTION_KIND = 6
XBOX_DATA_NOP_HIGH = 0xD580
XBOX_DATA_NOP_BYTES = 12
S_ENDPGM = 0xBF810000


class LayoutError(Exception):
    pass


def chunk(container, fourcc):
    if container[:4] != b"DXBC" or len(container) < 32:
        raise LayoutError("not a DXBC container")
    count = struct.unpack_from("<I", container, 28)[0]
    if 32 + 4 * count > len(container):
        raise LayoutError("chunk table overruns the container")
    for i in range(count):
        offset = struct.unpack_from("<I", container, 32 + 4 * i)[0]
        if offset + 8 > len(container):
            raise LayoutError(f"chunk {i} header overruns the container")
        size = struct.unpack_from("<I", container, offset + 4)[0]
        if container[offset:offset + 4] == fourcc:
            if offset + 8 + size > len(container):
                raise LayoutError(f"{fourcc.decode()} chunk overruns the container")
            return container[offset + 8:offset + 8 + size]
    raise LayoutError(f"no {fourcc.decode()} chunk")


def sections(payload):
    if payload[:4] != FSPX_MAGIC or len(payload) < 16:
        raise LayoutError("ACCC payload does not start with FSPX")
    _version, count, total = struct.unpack_from("<III", payload, 4)
    if total != len(payload):
        raise LayoutError(f"FSPX total size {total} != payload size {len(payload)}")
    table_end = 16 + 16 * count
    if table_end > len(payload):
        raise LayoutError("FSPX section table overruns the payload")
    result = []
    for i in range(count):
        kind, offset, size, reserved = struct.unpack_from("<IIII", payload, 16 + 16 * i)
        if reserved != 0:
            raise LayoutError(f"FSPX section {i} reserved word is {reserved:#x}, not 0")
        if offset < table_end or offset + size > len(payload):
            raise LayoutError(f"FSPX section {i} [{offset}, {offset + size}) is out of bounds")
        result.append((kind, offset, size))
    return result


def code_section(payload):
    matches = [(o, s) for k, o, s in sections(payload) if k == CODE_SECTION_KIND]
    if len(matches) != 1:
        raise LayoutError(f"expected one kind-{CODE_SECTION_KIND} section, found {len(matches)}")
    offset, size = matches[0]
    return payload[offset:offset + size]


def strip_xbox_data(code):
    """Return (machine code, number of v_nop_xbox_data skipped)."""
    skipped = 0
    while len(code) >= XBOX_DATA_NOP_BYTES and struct.unpack_from("<I", code, 0)[0] >> 16 == XBOX_DATA_NOP_HIGH:
        code = code[XBOX_DATA_NOP_BYTES:]
        skipped += 1
    return code, skipped


def until_endpgm(code):
    """Cut after the first dword-aligned s_endpgm. A literal that happens to equal it is not
    excluded here; the disassembler run is the authority on instruction boundaries."""
    for i in range(0, len(code) - 3, 4):
        if struct.unpack_from("<I", code, i)[0] == S_ENDPGM:
            return code[:i + 4]
    raise LayoutError("no s_endpgm in the code section")


def extract(container):
    code, skipped = strip_xbox_data(code_section(chunk(container, b"ACCC")))
    return until_endpgm(code), skipped


def disassemble(code, llvm_mc):
    text = " ".join(f"0x{b:02x}" for b in code)
    run = subprocess.run(
        [llvm_mc, "-arch=amdgcn", "-mcpu=gfx1030", "--disassemble", "-show-encoding"],
        input=text, capture_output=True, text=True, check=False,
    )
    lines = [ln.strip() for ln in run.stdout.splitlines() if ln.strip() and not ln.strip().startswith(".")]
    problems = [ln for ln in run.stderr.splitlines() if "invalid" in ln.lower() or "warning" in ln.lower()]
    return lines, problems


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("container", help="console-compiled .cso (DXBC with an ACCC chunk)")
    parser.add_argument("--bin", help="write the extracted machine code here")
    parser.add_argument("--asm", help="write the llvm-mc disassembly here (needs --llvm-mc)")
    parser.add_argument("--llvm-mc", help="path to llvm-mc with the AMDGPU target")
    args = parser.parse_args()
    try:
        code, skipped = extract(open(args.container, "rb").read())
    except LayoutError as error:
        print(f"error: {args.container}: {error}", file=sys.stderr)
        return 2
    if args.bin:
        open(args.bin, "wb").write(code)
    status = 0
    summary = f"{len(code)} code bytes, {skipped} v_nop_xbox_data skipped"
    if args.llvm_mc:
        lines, problems = disassemble(code, args.llvm_mc)
        summary += f", {len(lines)} instructions"
        if problems or not lines or "s_endpgm" not in lines[-1]:
            summary += ", DECODE PROBLEMS"
            status = 1
        if args.asm:
            open(args.asm, "w").write("\n".join(lines + problems) + "\n")
    print(f"{args.container}: {summary}")
    return status


if __name__ == "__main__":
    sys.exit(main())
