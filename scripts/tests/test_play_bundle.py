import importlib.util
from pathlib import Path
import struct
import unittest

SPEC = importlib.util.spec_from_file_location("play_bundle", Path(__file__).parents[1] / "check_play_bundle.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def elf(alignment=16_384, relro_end=16_384):
    data = bytearray(256)
    data[:6] = b"\x7fELF\x02\x01"
    struct.pack_into("<H", data, 18, 183)
    struct.pack_into("<Q", data, 32, 64)
    struct.pack_into("<HH", data, 54, 56, 2)
    struct.pack_into("<IIQQQQQQ", data, 64, 1, 5, 0, 0, 0, len(data), 16_384, alignment)
    struct.pack_into("<IIQQQQQQ", data, 120, 0x6474E552, 4, 0, 0, 0, 0, relro_end, 1)
    return bytes(data)


class NativeAlignmentTests(unittest.TestCase):
    def test_accepts_16k_library(self):
        MODULE.check_elf(elf())

    def test_rejects_4k_load_segment(self):
        with self.assertRaisesRegex(ValueError, "LOAD segment"):
            MODULE.check_elf(elf(alignment=4096))

    def test_rejects_relro_boundary_that_would_fault_on_16k(self):
        with self.assertRaisesRegex(ValueError, "RELRO"):
            MODULE.check_elf(elf(relro_end=4096))

    def test_accepts_short_relro_when_it_owns_the_entire_load_segment(self):
        data = bytearray(elf(relro_end=4096))
        struct.pack_into("<Q", data, 64 + 40, 4096)  # LOAD MemSiz equals RELRO MemSiz.
        MODULE.check_elf(bytes(data))

    def test_rejects_writable_neighbor_in_the_short_relro_final_page(self):
        data = bytearray(elf(relro_end=4096)) + bytearray(16_384)
        struct.pack_into("<Q", data, 64 + 40, 4096)
        struct.pack_into("<H", data, 56, 3)
        struct.pack_into("<IIQQQQQQ", data, 176, 1, 6, 8192, 8192, 0, 256, 4096, 16_384)
        with self.assertRaisesRegex(ValueError, "RELRO"):
            MODULE.check_elf(bytes(data))

    def test_rejects_truncated_headers(self):
        with self.assertRaisesRegex(ValueError, "headers"):
            MODULE.check_elf(elf()[:128])

    def test_rejects_wrong_architecture(self):
        data = bytearray(elf())
        struct.pack_into("<H", data, 18, 62)
        with self.assertRaisesRegex(ValueError, "arm64"):
            MODULE.check_elf(bytes(data))


if __name__ == "__main__":
    unittest.main()
