from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from usb_boot_mainline import (
    C906LBringupError,
    C906L_CAP_SHMEM_HEARTBEAT,
    C906L_RESET_BIT,
    C906L_RESET_REG,
    C906L_SEC_ENABLE_BIT,
    C906L_SEC_SYS_REG,
    C906L_STATE_RUNNING,
    C906L_VECTOR_HIGH_REG,
    C906L_VECTOR_LOW_REG,
    apply_c906l_reset_sequence,
    decode_c906l_status,
    parse_uboot_crc32,
    parse_uboot_words,
    select_c906l_handoff_mode,
    validate_c906l_layout,
)


class UBootOutputTests(unittest.TestCase):
    def test_crc32_accepts_fastboot_console_prefix(self):
        output = (
            "(bootloader) crc32 for 88000000 ... 88002fff ==> 961ca69b\n"
            "OKAY [  0.001s]\n"
        )
        self.assertEqual(
            parse_uboot_crc32(output, 0x88000000, 0x3000),
            0x961CA69B,
        )

    def test_crc32_rejects_result_for_another_range(self):
        with self.assertRaisesRegex(C906LBringupError, "found 0"):
            parse_uboot_crc32(
                "CRC32 for 82000000 ... 820fffff ==> 12345678\n",
                0x88000000,
                0x3000,
            )

    def test_status_dump_decodes_little_endian_u64_fields(self):
        output = "\n".join([
            "(bootloader) 88100000: 4d564b4e 00000001 00000040 00000002",
            "(bootloader) 88100010: 00000007 00000000 55667788 11223344",
            "(bootloader) 88100020: 00000003 00000000 aabbccdd 12345678",
            "(bootloader) 88100030: 76543210 fedcba98 00000000 00000000",
        ])
        words = parse_uboot_words(output, 0x88100000, 16)
        status = decode_c906l_status(words)
        self.assertEqual(status.abi_major, 1)
        self.assertEqual(status.abi_minor, 0)
        self.assertEqual(status.state, C906L_STATE_RUNNING)
        self.assertEqual(status.generation, 7)
        self.assertEqual(status.heartbeat, 0x1122334455667788)
        self.assertEqual(status.capabilities, C906L_CAP_SHMEM_HEARTBEAT | 1)
        self.assertEqual(status.last_request, 0x12345678AABBCCDD)
        self.assertEqual(status.last_response, 0xFEDCBA9876543210)
        self.assertIsNone(status.readiness_error())

    def test_status_dump_must_be_contiguous(self):
        with self.assertRaisesRegex(C906LBringupError, "omitted"):
            parse_uboot_words(
                "88100000: 4d564b4e 00000001 00000040 00000002\n",
                0x88100000,
                16,
            )

    def test_status_rejects_missing_selected_peripheral_capability(self):
        status = decode_c906l_status([
            0x4D564B4E, 1, 64, C906L_STATE_RUNNING,
            1, 0, 1, 0, 3, 0, 0, 0, 0, 0, 0, 0,
        ])
        self.assertRegex(status.readiness_error(7), "missing.*0x4")

    def test_running_status_rejects_incompatible_abi(self):
        words = [
            0x4D564B4E, 0x00000002, 64, C906L_STATE_RUNNING,
            1, 0, 1, 0, C906L_CAP_SHMEM_HEARTBEAT, 0, 0, 0, 0, 0, 0, 0,
        ]
        status = decode_c906l_status(words)
        self.assertRegex(status.readiness_error(), "unsupported status ABI")


class LayoutTests(unittest.TestCase):
    def test_known_safe_layout(self):
        validate_c906l_layout(
            firmware_size=0x3000,
            run_address=0x88000000,
            shmem_address=0x88100000,
            scratch_address=0x82000000,
            scratch_size=0x100000,
        )

    def test_rejects_scratch_smaller_than_cache_eviction_minimum(self):
        with self.assertRaisesRegex(C906LBringupError, "at least"):
            validate_c906l_layout(
                0x3000, 0x88000000, 0x88100000,
                0x82000000, 0x40000,
            )

    def test_rejects_overlapping_ranges(self):
        with self.assertRaisesRegex(C906LBringupError, "overlap"):
            validate_c906l_layout(
                0x200000, 0x88000000, 0x88100000,
                0x82000000, 0x100000,
            )

    def test_rejects_mmio_as_scratch(self):
        with self.assertRaisesRegex(C906LBringupError, "outside SG2002 DRAM"):
            validate_c906l_layout(
                0x3000, 0x88000000, 0x88100000,
                0x03000000, 0x100000,
            )


class ResetSequenceTests(unittest.TestCase):
    def test_handoff_mode_accepts_held_reset(self):
        self.assertEqual(
            select_c906l_handoff_mode(0, False, False), "held-reset"
        )

    def test_handoff_mode_accepts_fsbl_release_from_this_transfer(self):
        self.assertEqual(
            select_c906l_handoff_mode(C906L_RESET_BIT, True, False),
            "fsbl-started",
        )

    def test_handoff_mode_rejects_ambiguous_running_core(self):
        with self.assertRaisesRegex(C906LBringupError, "ambiguous attach"):
            select_c906l_handoff_mode(C906L_RESET_BIT, False, False)

    def test_handoff_mode_allows_explicit_running_attach(self):
        self.assertEqual(
            select_c906l_handoff_mode(C906L_RESET_BIT, False, True),
            "explicit-attach",
        )

    def test_vendor_order_preserves_unrelated_register_bits(self):
        registers = {
            C906L_RESET_REG: 0xA5,
            C906L_SEC_SYS_REG: 0x100,
            C906L_VECTOR_LOW_REG: 0,
            C906L_VECTOR_HIGH_REG: 0xFFFFFFFF,
        }
        operations = []

        def read_u32(address):
            operations.append(("read", address))
            return registers[address]

        def write_u32(address, value):
            operations.append(("write", address, value))
            registers[address] = value

        apply_c906l_reset_sequence(0x88000000, read_u32, write_u32)

        self.assertEqual(registers[C906L_RESET_REG], 0xA5 | C906L_RESET_BIT)
        self.assertEqual(
            registers[C906L_SEC_SYS_REG], 0x100 | C906L_SEC_ENABLE_BIT
        )
        self.assertEqual(registers[C906L_VECTOR_LOW_REG], 0x88000000)
        self.assertEqual(registers[C906L_VECTOR_HIGH_REG], 0)
        self.assertEqual(operations, [
            ("read", C906L_RESET_REG),
            ("write", C906L_RESET_REG, 0xA5 & ~C906L_RESET_BIT),
            ("read", C906L_RESET_REG),
            ("read", C906L_SEC_SYS_REG),
            ("write", C906L_SEC_SYS_REG, 0x100 | C906L_SEC_ENABLE_BIT),
            ("read", C906L_SEC_SYS_REG),
            ("write", C906L_VECTOR_LOW_REG, 0x88000000),
            ("write", C906L_VECTOR_HIGH_REG, 0),
            ("read", C906L_VECTOR_LOW_REG),
            ("read", C906L_VECTOR_HIGH_REG),
            ("read", C906L_RESET_REG),
            ("write", C906L_RESET_REG, 0xA5 | C906L_RESET_BIT),
            ("read", C906L_RESET_REG),
        ])

    def test_failed_reset_assertion_never_programs_or_releases_core(self):
        operations = []

        def read_u32(address):
            operations.append(("read", address))
            # Model hardware refusing to clear the release bit.
            return C906L_RESET_BIT

        def write_u32(address, value):
            operations.append(("write", address, value))

        with self.assertRaisesRegex(C906LBringupError, "did not assert"):
            apply_c906l_reset_sequence(0x88000000, read_u32, write_u32)
        self.assertEqual(operations, [
            ("read", C906L_RESET_REG),
            ("write", C906L_RESET_REG, 0),
            ("read", C906L_RESET_REG),
        ])

if __name__ == "__main__":
    unittest.main()
