"""Compile actual DarwinBoot ParseKernel and feed actual FAT payload bytes.

Run: python3 -m unittest discover -s tests -p 'test_*payload.py' -v
Host-only, read-only verification; no firmware/device execution.
"""

import ctypes
import os
from pathlib import Path
import shutil
import struct
import subprocess
import unittest

import test_sheng_fat_names as sheng
from test_sheng_fat_names import PAYLOAD, Volume, extract_function, extract_struct


DARWIN_SOURCE = Path(os.environ.get(
    "DARWINBOOT_SOURCE",
    str(sheng.REPO.parent / "UniXNU/drivers/DarwinBoot/DarwinBootPkg/Driver/DarwinBoot.c"),
))
EFI_SUCCESS = 0
EFI_LOAD_ERROR = 0x8000000000000001
EFI_INVALID_PARAMETER = 0x8000000000000002
EFI_UNSUPPORTED = 0x8000000000000003


def parser_source(source):
    """Host build of the actual ParseKernel plus one out-parameter entry point.

    The caller picks the starting KERNEL bytes (0xA5 when poison is set, zero
    otherwise). The zeroed start is for the success-path tests. The 0xA5 start is
    what the refused-header tests use: a refused header must still come back with
    all-zero diagnostics, which is only possible if ParseKernel clears the
    structure itself rather than relying on its caller.
    """
    prelude = r'''
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef uint8_t UINT8;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef size_t UINTN;
typedef uint64_t EFI_STATUS;
typedef int BOOLEAN;
typedef void VOID;
#define STATIC static
#define CONST const
#define EFIAPI
#define TRUE 1
#define FALSE 0
#define EFI_SUCCESS 0
#define EFI_LOAD_ERROR UINT64_C(0x8000000000000001)
#define EFI_INVALID_PARAMETER UINT64_C(0x8000000000000002)
#define EFI_UNSUPPORTED UINT64_C(0x8000000000000003)
#define MAX_UINT64 UINT64_MAX
#define EFI_PAGE_MASK UINT64_C(4095)
#define SIZE_2MB UINT64_C(0x200000)
#define SIZE_256MB UINT64_C(0x10000000)
#define WINDOW_SIZE SIZE_256MB
#define MAX_SEGMENTS 64
#define MIN(A, B) ((A) < (B) ? (A) : (B))
#define MAX(A, B) ((A) > (B) ? (A) : (B))
#define ZeroMem(P, N) memset((P), 0, (N))
#define SetMem(P, N, V) memset((P), (V), (N))
static UINT32 ReadUnaligned32(const UINT32 *p) {
  UINT32 value; memcpy(&value, p, sizeof(value)); return value;
}
static UINT64 ReadUnaligned64(const UINT64 *p) {
  UINT64 value; memcpy(&value, p, sizeof(value)); return value;
}
'''
    # Real U32/U64 functions call memcpy-based host ReadUnaligned stubs.
    actual = "\n".join((extract_struct(source, "SEGMENT"), extract_struct(source, "KERNEL"),
                         extract_function(source, "U32"), extract_function(source, "U64"),
                         extract_function(source, "ParseKernel")))
    wrapper = r'''
/*
 * Out-parameter entry point for the real ParseKernel. The EFI_STATUS goes back
 * through *StatusOut rather than as the return value, so the full 64-bit status
 * is observed as the actual value with nothing between the firmware function
 * and the test.
 *
 * The caller chooses the starting KERNEL bytes: poison != 0 fills it with 0xA5,
 * otherwise it is zeroed. Success-path tests use the zeroed start; the
 * refused-header tests use the 0xA5 start, so all-zero diagnostics after a
 * refusal can only come from ParseKernel clearing the structure itself.
 */
VOID host_parse (CONST UINT8 *bytes, UINTN size, UINT64 summary[4],
                 EFI_STATUS *status_out, int poison) {
  KERNEL kernel;
  if (poison) {
    SetMem(&kernel, sizeof(kernel), 0xA5);
  } else {
    ZeroMem(&kernel, sizeof(kernel));
  }
  *status_out = ParseKernel(bytes, size, &kernel);
  summary[0] = kernel.Count;
  summary[1] = kernel.Base;
  summary[2] = kernel.End;
  summary[3] = kernel.Entry;
}
'''
    return "\n".join((prelude, actual, wrapper))


def require_source(path, variable):
    """Skip the class when the C it compiles is not in this checkout.

    Missing source is not a verdict on the firmware: the class is skipped with
    the path and the override named, never reported as a pass.
    """
    if not path.is_file():
        raise unittest.SkipTest(f"DarwinBoot source {path} is not present "
                                f"(set {variable}); firmware ParseKernel NOT verified")


class DarwinBootPayloadTests(unittest.TestCase):
    # Reuse the actual Sheng directory lookup wrapper without inheriting its tests.
    lookup = sheng.ShengFatNamesTests.lookup

    @classmethod
    def setUpClass(cls):
        # Cheap pre-checks before compiling anything; no firmware is touched.
        require_source(DARWIN_SOURCE, "DARWINBOOT_SOURCE")
        require_source(sheng.SOURCE, "SHENG_FAT_SOURCE")
        sheng.ShengFatNamesTests.setUpClass.__func__(cls)
        directory = Path(cls.temporary.name)
        c_file = directory / "darwinboot_parser_host.c"
        c_file.write_text(parser_source(DARWIN_SOURCE.read_text()), encoding="utf-8")
        library = directory / "darwinboot_parser_host.so"
        command = [shutil.which("clang"), "-std=c11", "-shared", "-fPIC", "-O0",
                   "-Wall", "-Wextra", str(c_file), "-o", str(library)]
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(f"DarwinBoot host compile failed ({result.returncode}):\n"
                                 f"{' '.join(command)}\n{result.stdout}{result.stderr}")
        cls.parser = ctypes.CDLL(str(library))
        cls.parser.host_parse.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t,
                                          ctypes.POINTER(ctypes.c_uint64),
                                          ctypes.POINTER(ctypes.c_uint64), ctypes.c_int]
        cls.parser.host_parse.restype = None
        cls.synthetic = cls.synthetic_kernel()
        # The artifact is read only when it is actually there (see the guard on
        # the payload tests); without it the synthetic ParseKernel tests still run.
        if PAYLOAD.is_file():
            cls.kernel_bytes, cls.adt_bytes = cls.payload_files()
        else:
            cls.kernel_bytes = cls.adt_bytes = None

    @staticmethod
    def synthetic_kernel():
        """Minimal little-endian ARM64 Mach-O whose bytes the real parser accepts.

        Built here, byte by byte, so these tests say nothing on their own about
        whether the on-disk payload is real; the payload is checked separately.
        """
        def command(code, size, body):
            """Mach-O load command: command, cmdsize, then body bytes.

            The layout is written out literally below, order by order, so a
            plausible-looking but wrong field order cannot slip through: the
            command header is two words, then one write per field.
            """
            head = struct.pack("<I", code) + struct.pack("<I", size)
            assert len(head) + len(body) == size, (len(head), len(body), size)
            return head + body

        # LC_SEGMENT_64 (0x19), cmdsize 72 + one 80-byte section. Offsets as the
        # parser reads them (U64 at +24/+32/+40/+48, U32 at +60/+64):
        #   +8 segname[16] | +24 vmaddr | +32 vmsize | +40 fileoff | +48 filesize
        #   +60 protection (initprot) | +64 nsects
        # The parser counts a segment as executable only when the entry lies
        # strictly inside vmaddr..vmaddr+filesize and the segment maps r-x, so
        # vmaddr 0x100000, filesize 0x1000, entry 0x100800 with fileoff 0. The
        # entry is deliberately not 0x100000 + 0x1000 (one past the fragment).
        # Protection bit 4 (r-x) is what makes the entry land in file content.
        segment_body = (
            bytes(16)                                     # segname
            + struct.pack("<Q", 0x100000)                 # vmaddr
            + struct.pack("<Q", 0x2000)                   # vmsize: also covers the entry
            + struct.pack("<Q", 0)                        # fileoff
            + struct.pack("<Q", 0x1000)                   # filesize: inside the image
            + struct.pack("<I", 0)                        # maxprot
            + struct.pack("<I", 5)                        # initprot: r-x
            + struct.pack("<I", 1)                        # nsects
            + bytes(72 - 68)                              # flags + pad to 72
            + bytes(80)                                   # the section it declares
        )
        segment = command(0x19, 72 + 80, segment_body)
        # LC_UNIXTHREAD (5), cmdsize 288: flavor 6, count 68, then the arm64
        # thread state; the parser reads the entry from +272 of the command.
        entry = bytearray(288)
        struct.pack_into("<2I", entry, 0, 5, 288)
        struct.pack_into("<2I", entry, 8, 6, 68)
        struct.pack_into("<Q", entry, 272, 0x100800)
        entry = bytes(entry)
        # Two commands, sizeofcmds exactly covering them: the parser walks
        # exactly ncmds commands and then requires Offset == End.
        commands = segment + entry
        # magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved
        # (U32(Data + 16) is ncmds and U32(Data + 20) is sizeofcmds to the parser).
        header = struct.pack("<8I", 0xFEEDFACF, 0x0100000C, 0, 2, 2, len(commands), 0, 0)
        image = header + commands + bytes(0x1000 - len(header) - len(commands))
        assert (len(segment), len(entry), len(commands), len(image)) == (152, 288, 440, 0x1000)
        return image

    @classmethod
    def payload_files(cls):
        volume = Volume.payload(PAYLOAD)
        case = cls()
        directory_cluster = volume.root
        for component in ("EFI", "UniXNU", "Payloads"):
            result = case.lookup(volume, component, directory_cluster)
            if result[0] != 0:
                raise AssertionError(f"Actual FAT lookup {component!r} failed: {result}")
            directory_cluster = result[1]
        files = []
        for name in ("kernel.macho", "kernel.macho.adt"):
            result = case.lookup(volume, name, directory_cluster)
            if result[0] != 0:
                raise AssertionError(f"Actual FAT lookup {name!r} failed: {result}")
            content, _chain = volume.read_chain(result[1], result[2])
            files.append(content)
        return files

    def require_payload(self):
        """Skip only the tests whose bytes come from payload.img (not pxes the real ones)."""
        if self.kernel_bytes is None:
            raise unittest.SkipTest(f"payload artifact {PAYLOAD} is not present "
                                    "(set SHENG_FAT_PAYLOAD); real-payload ParseKernel NOT verified")

    def parse(self, content, poison=False):
        """Run the real ParseKernel; the KERNEL is 0xA5-filled when poison=True."""
        data = (ctypes.c_uint8 * len(content)).from_buffer_copy(content) if content is not None \
            else ctypes.POINTER(ctypes.c_uint8)()
        summary = (ctypes.c_uint64 * 4)()
        status = ctypes.c_uint64()
        self.parser.host_parse(data, len(content) if content is not None else 64,
                               summary, ctypes.byref(status), int(poison))
        return status.value, tuple(summary)

    # -- real payload, pinned bytes (skipped without the artifact) -------------

    def test_actual_payload_kernel_parses(self):
        self.require_payload()
        status, summary = self.parse(self.kernel_bytes)
        count, base, end, entry = summary
        self.assertEqual(status, 0, f"actual ParseKernel rejected payload: status={status:#x} summary={summary}")
        self.assertGreater(count, 0)
        self.assertLess(base, end)
        self.assertLessEqual(base, entry)
        self.assertLess(entry, end)
        self.assertLessEqual(end - base, 0x10000000 - 0x200000)
        print(f"ParseKernel payload: EFI_SUCCESS segments={count} base={base:#x} "
              f"end={end:#x} entry={entry:#x}", flush=True)

    def test_actual_payload_adt_is_not_a_kernel(self):
        self.require_payload()
        status, summary = self.parse(self.adt_bytes)
        self.assertEqual(status, EFI_UNSUPPORTED,
                         f"ADT unexpectedly treated as kernel: status={status:#x} summary={summary}")

    def test_actual_payload_truncated_commands_rejected(self):
        self.require_payload()
        command_bytes = struct.unpack_from("<I", self.kernel_bytes, 20)[0]
        for size in (32, 39, 32 + command_bytes - 1):
            with self.subTest(truncated_size=size):
                status, summary = self.parse(self.kernel_bytes[:size])
                self.assertEqual(status, EFI_LOAD_ERROR,
                                 f"truncated commands accepted: size={size} status={status:#x} summary={summary}")
        # The bytes exist, but the first declared command overruns sizeofcmds.
        malformed = bytearray(self.kernel_bytes)
        struct.pack_into("<I", malformed, 36, (command_bytes + 8 + 7) & ~7)
        status, summary = self.parse(malformed)
        self.assertEqual(status, EFI_LOAD_ERROR,
                         f"overlong command accepted: status={status:#x} summary={summary}")

    # -- synthetic bytes: no payload artifact needed ---------------------------

    def test_synthetic_kernel_parses(self):
        status, summary = self.parse(self.synthetic)
        self.assertEqual(status, EFI_SUCCESS, f"synthetic kernel rejected: {summary}")
        # One segment: vmaddr 0x100000, vmsize 0x2000 (End = vmaddr + vmsize),
        # entry 0x100800 from the LC_UNIXTHREAD command.
        self.assertEqual(summary, (1, 0x100000, 0x102000, 0x100800))

    def test_synthetic_bad_header_unsupported(self):
        header = self.synthetic[4:8]                       # cputype of a non-ARM64 image
        status, summary = self.parse(header)
        self.assertEqual(status, EFI_UNSUPPORTED, f"header accepted: status={status:#x} summary={summary}")
        self.assertEqual(summary, (0, 0, 0, 0))

    def test_refused_header_leaves_summary_zero_from_poisoned_kernel(self):
        """The bug this covers: a caller-side clear hid ParseKernel not clearing.

        The KERNEL starts as 0xA5 and the call is refused by the header check. A
        passing run therefore proves ParseKernel itself zeroes the structure on
        entry, not the wrapper.
        """
        for label, content in (("adt", struct.pack("<16I", 5, 7, *(0,) * 14)),
                               ("short", self.synthetic[:31]),
                               ("header", self.synthetic[4:8])):
            with self.subTest(bytes=label):
                status, summary = self.parse(content, poison=True)
                self.assertIn(status, (EFI_UNSUPPORTED, EFI_LOAD_ERROR),
                              f"{label}: refused header must be an error, got {status:#x}")
                self.assertEqual(summary, (0, 0, 0, 0),
                                 f"{label}: refusal left stale fields (uninitialized diagnostics): {summary}")

    def test_null_data_rejected_as_invalid_parameter(self):
        """A NULL image must be refused, not dereferenced.

        Size is >= 32 on purpose: the length check alone would not stop the call,
        so only an explicit Data == NULL guard keeps this from reading address 0.
        The poisoned KERNEL proves the refusal also clears the diagnostics.
        """
        status, summary = self.parse(None, poison=True)
        self.assertEqual(status, EFI_INVALID_PARAMETER,
                         f"NULL data not refused with EFI_INVALID_PARAMETER: status={status:#x}")
        self.assertEqual(summary, (0, 0, 0, 0),
                         f"NULL data left stale diagnostics: {summary}")

    def test_poisoned_kernel_still_parses_synthetic_image(self):
        """Poisoning must not change what a successful parse reports."""
        clean = self.parse(self.synthetic)
        poisoned = self.parse(self.synthetic, poison=True)
        self.assertEqual(poisoned, clean, f"parse depends on caller-side clearing: {poisoned} != {clean}")


if __name__ == "__main__":
    unittest.main(verbosity=2)
