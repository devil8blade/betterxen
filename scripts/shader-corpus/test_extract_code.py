# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests for extract_code.py on synthetic containers (no GDK or compiler needed)."""

import importlib.util
import struct
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location("extract_code", Path(__file__).with_name("extract_code.py"))
ec = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ec)

S_NOP = struct.pack("<I", 0xBF800000)  # public gfx10 encoding of s_nop 0
S_ENDPGM = struct.pack("<I", 0xBF810000)
XBOX_DATA = struct.pack("<III", 0xD580AA64, 0x03FF2159, 0x02274AC7)


def fspx(sections, total=None):
    """sections: list of (kind, bytes). Builds an FSPX payload with the parsed layout."""
    count = len(sections)
    offset = 16 + 16 * count
    table, body = b"", b""
    for kind, data in sections:
        table += struct.pack("<IIII", kind, offset + len(body), len(data), 0)
        body += data
    size = 16 + len(table) + len(body)
    return b"FSPX" + struct.pack("<III", 1, count, size if total is None else total) + table + body


def dxbc(chunks):
    """chunks: list of (fourcc, bytes)."""
    header = 32 + 4 * len(chunks)
    offsets, body = [], b""
    for fourcc, data in chunks:
        offsets.append(header + len(body))
        body += fourcc + struct.pack("<I", len(data)) + data
    total = header + len(body)
    head = b"DXBC" + b"\0" * 16 + struct.pack("<III", 1, total, len(chunks))
    return head + b"".join(struct.pack("<I", o) for o in offsets) + body


class ExtractTests(unittest.TestCase):
    def test_extracts_code_after_xbox_data_nops(self):
        code = XBOX_DATA * 3 + S_NOP + S_ENDPGM + b"\0" * 8
        container = dxbc([(b"DXIL", b"x" * 12), (b"ACCC", fspx([(0, b"a" * 28), (6, code), (7, b"b" * 8)]))])
        extracted, skipped = ec.extract(container)
        self.assertEqual(skipped, 3)
        self.assertEqual(extracted, S_NOP + S_ENDPGM)

    def test_no_xbox_data_nops(self):
        container = dxbc([(b"ACCC", fspx([(6, S_NOP + S_ENDPGM)]))])
        self.assertEqual(ec.extract(container), (S_NOP + S_ENDPGM, 0))

    def test_missing_accc(self):
        with self.assertRaisesRegex(ec.LayoutError, "no ACCC chunk"):
            ec.extract(dxbc([(b"DXIL", b"x" * 4)]))

    def test_not_fspx(self):
        with self.assertRaisesRegex(ec.LayoutError, "does not start with FSPX"):
            ec.extract(dxbc([(b"ACCC", b"ABCD" + b"\0" * 12)]))

    def test_total_size_mismatch(self):
        with self.assertRaisesRegex(ec.LayoutError, "total size"):
            ec.extract(dxbc([(b"ACCC", fspx([(6, S_ENDPGM)], total=999))]))

    def test_section_inside_table_is_rejected(self):
        payload = bytearray(fspx([(6, S_ENDPGM)]))
        struct.pack_into("<I", payload, 16 + 4, 4)  # section offset points into the header
        with self.assertRaisesRegex(ec.LayoutError, "out of bounds"):
            ec.extract(dxbc([(b"ACCC", bytes(payload))]))

    def test_nonzero_reserved_word_is_rejected(self):
        payload = bytearray(fspx([(6, S_ENDPGM)]))
        struct.pack_into("<I", payload, 16 + 12, 1)
        with self.assertRaisesRegex(ec.LayoutError, "reserved word"):
            ec.extract(dxbc([(b"ACCC", bytes(payload))]))

    def test_needs_exactly_one_code_section(self):
        with self.assertRaisesRegex(ec.LayoutError, "found 0"):
            ec.extract(dxbc([(b"ACCC", fspx([(0, S_ENDPGM)]))]))
        with self.assertRaisesRegex(ec.LayoutError, "found 2"):
            ec.extract(dxbc([(b"ACCC", fspx([(6, S_ENDPGM), (6, S_ENDPGM)]))]))

    def test_code_without_endpgm(self):
        with self.assertRaisesRegex(ec.LayoutError, "no s_endpgm"):
            ec.extract(dxbc([(b"ACCC", fspx([(6, S_NOP * 4)]))]))


if __name__ == "__main__":
    unittest.main()
