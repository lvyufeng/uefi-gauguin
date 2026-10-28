"""Host regression tests for the actual Sheng FAT directory-name implementation.

Run: python3 -m unittest discover -s tests -p test_sheng_fat_names.py -v
No firmware build or device access is needed. Only a temporary host shared library
is compiled; payload.img is opened read-only. Override SHENG_FAT_SOURCE or SHENG_FAT_PAYLOAD when testing a different checkout/artifact.

Only the real-payload test needs payload.img; every synthetic-volume test runs
without the artifact (and the test is skipped, with the reason, when it is
absent rather than silently passing).
"""

import ctypes
import hashlib
import math
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[1]
# Defaults are resolved relative to this checkout, never from a hard-coded home
# directory. REPO.parent is the sibling checkouts (UniXNU/, uefi-gauguin/ ...).
DEFAULT_PAYLOAD = REPO.parent / "UniXNU/out/gauguin/payload/payload.img"
SOURCE = Path(os.environ.get(
    "SHENG_FAT_SOURCE",
    str(REPO / "Platforms/Xiaomi/gauguinPkg/Drivers/ShengBootMemoryDxe/ShengBootMemoryDxe.c"),
))
# SHENG_FAT_PAYLOAD still overrides, but an empty value counts as unset.
PAYLOAD = Path(os.environ.get("SHENG_FAT_PAYLOAD") or DEFAULT_PAYLOAD)
LFN_OFFSETS = (1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30)
# Pinned from read-only traversal of the restored bak2 payload.img, NOT the
# stale payload.json or the different standalone out/gauguin/kernel artifact.
PAYLOAD_KERNEL_SHA256 = "6cc5894a917d0a93c117ace3338eda493b1a93cd85132cabda304eb4e00259d3"
PAYLOAD_ADT_SHA256 = "65d42111e9f45588a6ac244ed66a28da35715072cfff699a07a10567f8768726"


def extract_struct(source, name):
    """Copy a real typedef, including changed LFN member names/layout."""
    match = re.search(r"typedef\s+struct\s*\{[^{}]*\}\s*" + re.escape(name) + r"\s*;", source)
    if match is None:
        raise AssertionError(f"Cannot extract {name} from {SOURCE}")
    return match.group(0)


def extract_function(source, name):
    match = re.search(r"\bSTATIC\s+\w+\s+" + re.escape(name) + r"\s*\(", source)
    if match is None:
        raise AssertionError(f"Cannot extract actual {name} from {SOURCE}")
    start = source.index("{", match.end())
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


def host_source(source):
    start_marker = "// Compare a raw 11-byte directory name"
    end_marker = "// Resolve a path"
    start = source.index(start_marker)
    end = source.index(end_marker, start)
    region = source[start:end]
    for function in ("NameMatches", "LongNameMatches", "FindInDirectory"):
        if not re.search(r"\b" + function + r"\s*\(", region):
            raise AssertionError(f"Actual {function} absent from extraction region")
    prelude = r'''
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <wchar.h>
typedef uint8_t UINT8;
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef size_t UINTN;
typedef intptr_t INTN;
typedef char CHAR8;
typedef uint16_t CHAR16;
typedef int BOOLEAN;
typedef uint64_t EFI_STATUS;
typedef void VOID;
typedef void *EFI_HANDLE;
typedef struct { int unused; } EFI_DEVICE_PATH_PROTOCOL;
typedef struct { int unused; } EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;
#define STATIC static
#define CONST const
#define IN
#define OUT
#define OPTIONAL
#define TRUE 1
#define FALSE 0
#define EFI_SUCCESS 0
#define EFI_NOT_FOUND 14
#define EFI_VOLUME_CORRUPTED 10
#define EFI_ERROR(Status) ((Status) != EFI_SUCCESS)
#define ARRAY_SIZE(A) (sizeof(A) / sizeof((A)[0]))
#define STATIC_ASSERT(C, M) _Static_assert(C, M)
#define CopyMem(D, S, N) memcpy((D), (S), (N))
#define ZeroMem(D, N) memset((D), 0, (N))
#define SetMem(D, N, V) memset((D), (V), (N))
#define CompareMem(A, B, N) memcmp((A), (B), (N))
#define FAT_ATTRIBUTE_DIRECTORY 0x10
#define FAT_ATTRIBUTE_VOLUME_ID 0x08
#define FAT_ATTRIBUTE_LFN 0x0F
#define FAT_FREE_CLUSTER 0x00000000u
#define FAT_BAD_CLUSTER 0x0FFFFFF7u
#define FAT_END_OF_CHAIN 0x0FFFFFF8u
#define FAT_CLUSTER_MASK 0x0FFFFFFFu
#define SHENG_FAT_MAX_PATH 260
'''
    structs = "\n#pragma pack(push, 1)\n" + "\n".join(
        extract_struct(source, name) for name in ("FAT_DIR_ENTRY", "FAT_LFN_ENTRY")
    ) + "\n#pragma pack(pop)\n" + extract_struct(source, "FAT_VOLUME")
    wrappers = r'''
_Static_assert(sizeof(FAT_DIR_ENTRY) == 32, "directory entry size");
_Static_assert(sizeof(FAT_LFN_ENTRY) == 32, "LFN entry size");
_Static_assert(sizeof(wchar_t) == 2, "host uses UEFI-sized wide literals");
int host_short(const UINT8 *raw, const CHAR16 *name) {
  return NameMatches(raw, name);
}
int host_long(const CHAR16 *stored, const CHAR16 *name) {
  return LongNameMatches(stored, name);
}
UINT64 host_lookup(UINT8 *base, UINT64 size, UINT32 bpc, UINT32 fat,
                   UINT32 data, UINT32 root, UINT32 max_cluster,
                   UINT32 directory, const CHAR16 *name, UINT8 *result) {
  FAT_VOLUME volume;
  ZeroMem(&volume, sizeof(volume));
  volume.Base = base;
  volume.Size = size;
  volume.BytesPerCluster = bpc;
  volume.FatOffset = fat;
  volume.DataOffset = data;
  volume.RootCluster = root;
  volume.MaxCluster = max_cluster;
  return FindInDirectory(&volume, directory, name, (FAT_DIR_ENTRY *)result);
}
'''
    return "\n".join((prelude, structs, extract_function(source, "ClusterToOffset"),
                      extract_function(source, "NextCluster"), region, wrappers))


def utf16(text):
    encoded = text.encode("utf-16-le") + b"\0\0"
    return (ctypes.c_uint16 * (len(encoded) // 2)).from_buffer_copy(encoded)


def checksum(raw):
    value = 0
    for byte in raw:
        value = (((value & 1) << 7) + (value >> 1) + byte) & 255
    return value


def short_entry(raw=b"LONGNA~1TXT", cluster=7, size=123, attributes=0x20):
    assert len(raw) == 11
    entry = bytearray(32)
    entry[:11] = raw
    entry[11] = attributes
    struct.pack_into("<H", entry, 20, cluster >> 16)
    struct.pack_into("<H", entry, 26, cluster & 65535)
    struct.pack_into("<I", entry, 28, size)
    return bytes(entry)


def long_entries(name, raw=b"LONGNA~1TXT"):
    """Generate VFAT wire bytes, independently of firmware struct definitions."""
    encoded = name.encode("utf-16-le")
    units = list(struct.unpack("<" + "H" * (len(encoded) // 2), encoded))
    count = math.ceil(len(units) / 13)
    if len(units) % 13:
        units += [0] + [0xFFFF] * (count * 13 - len(units) - 1)
    entries = []
    for sequence in range(count, 0, -1):
        entry = bytearray(32)
        entry[0] = sequence | (0x40 if sequence == count else 0)
        entry[11] = 0x0F
        entry[13] = checksum(raw)
        for offset, unit in zip(LFN_OFFSETS, units[(sequence - 1) * 13:sequence * 13]):
            struct.pack_into("<H", entry, offset, unit)
        entries.append(bytes(entry))
    return entries


class Volume:
    def __init__(self, image, bpc, fat, data, root, maximum):
        self.buffer = (ctypes.c_uint8 * len(image)).from_buffer_copy(image)
        self.geometry = (len(image), bpc, fat, data, root, maximum)
        self.root = root

    def read_chain(self, first_cluster, file_size):
        """Independent strict FAT32 reader; reject cycles, bounds and bad length.

        Walk through EOC even after file_size bytes to catch trailing corruption.
        The resulting bytes exclude final-cluster slack; the chain includes it.
        """
        size, bpc, fat, data, _root, maximum = self.geometry
        if file_size < 0 or bpc <= 0:
            raise ValueError("invalid file size or cluster geometry")
        view = memoryview(self.buffer).cast("B")
        chain = []
        visited = set()
        chunks = []
        cluster = first_cluster
        while cluster < 0x0FFFFFF8:
            if cluster < 2 or cluster >= maximum:
                raise ValueError(f"cluster {cluster:#x} outside usable bounds [2,{maximum})")
            if cluster in visited:
                raise ValueError(f"cycle at cluster {cluster} after {len(chain)} clusters")
            visited.add(cluster)
            chain.append(cluster)
            offset = data + (cluster - 2) * bpc
            fat_offset = fat + cluster * 4
            if offset < data or offset + bpc > size:
                raise ValueError(f"cluster {cluster} data outside image bounds")
            if fat_offset < fat or fat_offset + 4 > data or fat_offset + 4 > size:
                raise ValueError(f"cluster {cluster} FAT entry outside FAT/image bounds")
            chunks.append(bytes(view[offset:offset + bpc]))
            cluster = struct.unpack_from("<I", view, fat_offset)[0] & 0x0FFFFFFF
        expected = math.ceil(file_size / bpc)
        if len(chain) < expected:
            raise ValueError(f"short chain: {len(chain)} clusters, expected {expected} for {file_size} bytes")
        if len(chain) > expected:
            raise ValueError(f"overlong chain: {len(chain)} clusters, expected {expected}")
        return b"".join(chunks)[:file_size], chain

    @classmethod
    def synthetic(cls, entries, bpc=512):
        content = b"".join(entries) + bytes(32)  # directory end marker
        count = math.ceil(len(content) / bpc)
        data = 128
        image = bytearray(data + count * bpc)
        image[data:data + len(content)] = content
        for cluster in range(2, count + 2):
            following = cluster + 1 if cluster < count + 1 else 0x0FFFFFFF
            struct.pack_into("<I", image, cluster * 4, following)
        return cls(image, bpc, 0, data, 2, count + 2)

    @classmethod
    def payload(cls, path):
        image = path.read_bytes()
        bps = struct.unpack_from("<H", image, 11)[0]
        spc = image[13]
        reserved = struct.unpack_from("<H", image, 14)[0]
        fats = image[16]
        fat_sectors = struct.unpack_from("<I", image, 36)[0]
        root = struct.unpack_from("<I", image, 44)[0]
        sectors = struct.unpack_from("<I", image, 32)[0]
        data_sectors = reserved + fats * fat_sectors
        if bps != 512 or not spc or not fat_sectors or image[510:512] != b"\x55\xaa":
            raise AssertionError(f"Unexpected FAT32 geometry in {path}")
        return cls(image, bps * spc, reserved * bps, data_sectors * bps,
                   root, (sectors - data_sectors) // spc + 2)


class ShengFatNamesTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        clang = shutil.which("clang")
        if clang is None:
            raise RuntimeError("Host clang is required to test the actual firmware C")
        cls.temporary = tempfile.TemporaryDirectory(prefix="sheng-fat-names-")
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        c_file = directory / "sheng_fat_host.c"
        c_file.write_text(host_source(SOURCE.read_text()), encoding="utf-8")
        library = directory / "sheng_fat_host.so"
        command = [clang, "-std=c11", "-fshort-wchar", "-shared", "-fPIC", "-O0",
                   "-Wall", "-Wextra", str(c_file), "-o", str(library)]
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(f"Host compilation failed ({result.returncode}):\n"
                                 f"{' '.join(command)}\n{result.stdout}{result.stderr}")
        cls.library = ctypes.CDLL(str(library))
        byte_pointer = ctypes.POINTER(ctypes.c_uint8)
        wide_pointer = ctypes.POINTER(ctypes.c_uint16)
        cls.library.host_lookup.argtypes = [byte_pointer, ctypes.c_uint64] + [ctypes.c_uint32] * 6 + [wide_pointer, byte_pointer]
        cls.library.host_lookup.restype = ctypes.c_uint64
        cls.library.host_short.argtypes = [byte_pointer, wide_pointer]
        cls.library.host_short.restype = ctypes.c_int
        cls.library.host_long.argtypes = [wide_pointer, wide_pointer]
        cls.library.host_long.restype = ctypes.c_int

    def lookup(self, volume, name, directory=None):
        entry = (ctypes.c_uint8 * 32)()
        status = self.library.host_lookup(volume.buffer, *volume.geometry,
                                          volume.root if directory is None else directory,
                                          utf16(name), entry)
        raw = bytes(entry)
        cluster = (struct.unpack_from("<H", raw, 20)[0] << 16) | struct.unpack_from("<H", raw, 26)[0]
        size = struct.unpack_from("<I", raw, 28)[0]
        return status, cluster, size, raw[11]

    def assert_found(self, entries, name, bpc=512):
        result = self.lookup(Volume.synthetic(entries, bpc), name)
        self.assertEqual(result[:3], (0, 7, 123), f"lookup {name!r}: status/cluster/size={result[:3]}")

    def assert_missing(self, entries, name):
        result = self.lookup(Volume.synthetic(entries), name)
        self.assertEqual(result[0], 14, f"malformed LFN {name!r} accepted: {result}")

    @unittest.skipUnless(PAYLOAD.is_file(),
                         f"real payload artifact {PAYLOAD} is not present "
                         "(set SHENG_FAT_PAYLOAD); real-payload walk NOT verified")
    def test_actual_payload_root_walk_and_distinct_files(self):
        volume = Volume.payload(PAYLOAD)
        directory = volume.root
        for component in ("EFI", "UniXNU", "Payloads"):
            result = self.lookup(volume, component, directory)
            self.assertEqual(result[0], 0, f"real payload directory {component!r}: {result}")
            self.assertTrue(result[3] & 0x10, f"{component!r} is not a directory: {result}")
            directory = result[1]
        kernel = self.lookup(volume, "kernel.macho", directory)
        adt = self.lookup(volume, "kernel.macho.adt", directory)
        self.assertEqual(kernel[0], 0, f"real kernel lookup: {kernel}")
        self.assertEqual(adt[0], 0, f"real ADT lookup: {adt}")
        self.assertEqual(kernel[2], 57110528, f"real kernel wrong entry: {kernel}")
        self.assertEqual(adt[2], 12468, f"real ADT wrong entry: {adt}")
        self.assertGreaterEqual(kernel[1], 2)
        self.assertGreaterEqual(adt[1], 2)
        self.assertNotEqual(kernel[1], adt[1], f"kernel and ADT alias the same cluster: {kernel}, {adt}")
        kernel_bytes, kernel_chain = volume.read_chain(kernel[1], kernel[2])
        adt_bytes, adt_chain = volume.read_chain(adt[1], adt[2])
        self.assertEqual(len(kernel_chain), 55772)
        self.assertEqual(len(adt_chain), 13)
        self.assertTrue(set(kernel_chain).isdisjoint(adt_chain), "kernel and ADT chains overlap")
        self.assertEqual(struct.unpack_from("<II", kernel_bytes), (0xFEEDFACF, 0x0100000C),
                         "kernel must be a little-endian ARM64 Mach-O")
        # Apple flattened device-tree root: property count, child count,
        # then the fixed-width property name and length, followed by value.
        self.assertEqual(struct.unpack_from("<II", adt_bytes), (5, 7), "ADT root header")
        self.assertEqual(adt_bytes[8:40], b"name" + bytes(28), "ADT first root property")
        self.assertEqual(struct.unpack_from("<I", adt_bytes, 40)[0], 12, "ADT name property length")
        self.assertEqual(adt_bytes[44:56], b"device-tree\0", "ADT root name")
        self.assertEqual(hashlib.sha256(kernel_bytes).hexdigest(), PAYLOAD_KERNEL_SHA256,
                         "kernel bytes differ from known restored payload")
        self.assertEqual(hashlib.sha256(adt_bytes).hexdigest(), PAYLOAD_ADT_SHA256,
                         "ADT bytes differ from known restored payload")

    def test_strict_chain_reader_rejects_corruption(self):
        # Synthetic directory data doubles as a small file for reader tests.
        def image():
            return Volume.synthetic([short_entry()] * 3, bpc=64)
        good = image()
        content, chain = good.read_chain(2, 96)
        self.assertEqual(chain, [2, 3])
        self.assertEqual(len(content), 96)
        cases = (("cycle", 2, 2, "cycle"),
                 ("short", 2, 0x0FFFFFFF, "short chain"),
                 ("free", 2, 0, "outside usable bounds"),
                 ("bad", 2, 0x0FFFFFF7, "outside usable bounds"),
                 ("out-of-range", 2, 100, "outside usable bounds"))
        for label, cluster, following, message in cases:
            with self.subTest(corruption=label):
                volume = image()
                struct.pack_into("<I", volume.buffer, cluster * 4, following)
                with self.assertRaisesRegex(ValueError, message):
                    volume.read_chain(2, 96)
        with self.assertRaisesRegex(ValueError, "overlong chain"):
            good.read_chain(2, 32)
        bounded = image()
        size, bpc, fat, data, root, maximum = bounded.geometry
        bounded.geometry = (size - 1, bpc, fat, data, root, maximum)
        with self.assertRaisesRegex(ValueError, "data outside image bounds"):
            bounded.read_chain(2, 96)
        bounded = image()
        size, bpc, fat, data, root, maximum = bounded.geometry
        bounded.geometry = (size, bpc, data - 8, data, root, maximum)
        with self.assertRaisesRegex(ValueError, "FAT entry outside"):
            bounded.read_chain(2, 96)

    def test_lfn_malformed_terminator_and_padding(self):
        name = "kernel.macho"  # 12 code units, followed by the required NUL
        bad = bytearray(long_entries(name)[0])
        struct.pack_into("<H", bad, LFN_OFFSETS[12], 0xFFFF)
        self.assert_missing([bytes(bad), short_entry()], name)
        name = "unixnu.toml"  # 11 code units, NUL, then 0xFFFF
        for invalid_padding in (0, ord("X")):
            with self.subTest(padding=invalid_padding):
                bad = bytearray(long_entries(name)[0])
                struct.pack_into("<H", bad, LFN_OFFSETS[12], invalid_padding)
                self.assert_missing([bytes(bad), short_entry()], name)
        # Embedded NUL must not truncate an otherwise malformed LFN to a prefix.
        bad = bytearray(long_entries("kernel.macho")[0])
        struct.pack_into("<H", bad, LFN_OFFSETS[6], 0)
        self.assert_missing([bytes(bad), short_entry()], "kernel")

    def test_lfn_length_limit(self):
        name = "a" * 255
        self.assert_found(long_entries(name) + [short_entry()], name)
        for length in (256, 260, 261):
            with self.subTest(length=length):
                name = "a" * length
                self.assert_missing(long_entries(name) + [short_entry()], name)

    def test_lfn_type_cluster_and_ordinal_flags(self):
        name = "kernel.macho"
        for offset, value in ((12, 1), (26, 1), (27, 1), (0, 0x61), (0, 0xC1), (0, 0x40)):
            with self.subTest(offset=offset, value=value):
                bad = bytearray(long_entries(name)[0])
                bad[offset] = value
                self.assert_missing([bytes(bad), short_entry()], name)

    def test_volume_label_interrupts_lfn(self):
        name = "kernel.macho"
        label = short_entry(b"VOLUMELABEL", cluster=0, size=0, attributes=0x08)
        self.assert_missing(long_entries(name) + [label, short_entry()], name)
        name = "kernel.macho.adt"
        entries = long_entries(name)
        self.assert_missing(entries[:1] + [label] + entries[1:] + [short_entry()], name)

    def test_single_entry_lfn(self):
        name = "kernel.macho"
        self.assert_found(long_entries(name) + [short_entry()], name.upper())

    def test_single_entry_exact_thirteen_characters(self):
        name = "abcdefghi.txt"
        self.assertEqual(len(name), 13)
        self.assert_found(long_entries(name) + [short_entry()], name)

    def test_multi_entry_lfn(self):
        name = "kernel.macho.adt"
        self.assert_found(long_entries(name) + [short_entry()], name)

    def test_multi_entry_exact_twenty_six_characters(self):
        name = "abcdefghijklmnopqrstuv.txt"
        self.assertEqual(len(name), 26)
        self.assert_found(long_entries(name) + [short_entry()], name)

    def test_checksum_mismatch(self):
        name = "kernel.macho.adt"
        entries = long_entries(name)
        for index in range(len(entries)):
            with self.subTest(corrupt_piece=index):
                corrupt = list(entries)
                bad = bytearray(corrupt[index])
                bad[13] ^= 0x01
                corrupt[index] = bytes(bad)
                self.assert_missing(corrupt + [short_entry()], name)
        self.assert_missing(entries + [short_entry(b"OTHER~1 TXT")], name)

    def test_missing_sequence(self):
        name = "abcdefghijklmnopqrstuvwxyz012345.txt"
        entries = long_entries(name)
        self.assertEqual(len(entries), 3)
        for index in range(len(entries)):
            with self.subTest(missing_piece=index):
                self.assert_missing(entries[:index] + entries[index + 1:] + [short_entry()], name)
        # Missing highest/LAST entry must not expose the surviving name prefix.
        self.assert_missing(entries[1:] + [short_entry()], name[:26])

    def test_reordered_and_duplicate_sequences(self):
        name = "kernel.macho.adt"
        entries = long_entries(name)
        self.assert_missing(list(reversed(entries)) + [short_entry()], name)
        self.assert_missing(entries + entries[1:] + [short_entry()], name)
        bad = bytearray(entries[0])
        bad[0] &= ~0x40
        self.assert_missing([bytes(bad)] + entries[1:] + [short_entry()], name)

    def test_deleted_entry_interrupts_lfn(self):
        name = "kernel.macho"
        deleted = bytes([0xE5]) + bytes(31)
        self.assert_missing(long_entries(name) + [deleted, short_entry()], name)
        name = "kernel.macho.adt"
        entries = long_entries(name)
        self.assert_missing(entries[:1] + [deleted] + entries[1:] + [short_entry()], name)

    def test_lfn_continues_across_directory_cluster_boundary(self):
        name = "kernel.macho.adt"
        deleted = bytes([0xE5]) + bytes(31)
        # Cluster 2 ends with LAST|2; cluster 3 starts with sequence 1, then SFN.
        self.assert_found([deleted] + long_entries(name) + [short_entry()], name, bpc=64)

    def test_short_name_fallback_even_with_invalid_lfn(self):
        raw = b"ABC     TXT"
        self.assert_found([short_entry(raw)], "abc.txt")
        entries = long_entries("not-the-short-name.txt", raw)
        bad = bytearray(entries[0])
        bad[13] ^= 0x01
        self.assert_found([bytes(bad)] + entries[1:] + [short_entry(raw)], "ABC.TXT")
        self.assert_found([short_entry(b"KERNEL~1MAC")], "kernel~1mac")

    def test_short_name_rejects_truncation(self):
        cases = ((b"ABC     TXT", "ABC.TXTjunk"),
                 (b"ABCDEFGHTXT", "ABCDEFGHI.TXT"),
                 (b"ABC        ", "ABC.junk"),
                 (b"ABC     TXT", "ABC.TXT.more"))
        for raw, name in cases:
            with self.subTest(name=name):
                buffer = (ctypes.c_uint8 * 11).from_buffer_copy(raw)
                self.assertFalse(self.library.host_short(buffer, utf16(name)),
                                 f"actual NameMatches truncates {name!r} to {raw!r}")
                self.assert_missing([short_entry(raw)], name)

    def test_long_name_exact_case_insensitive_comparison(self):
        self.assertTrue(self.library.host_long(utf16("Kernel.Macho"), utf16("kernel.macho")))
        self.assertFalse(self.library.host_long(utf16("kernel.macho"), utf16("kernel.macho.adt")))
        self.assertFalse(self.library.host_long(utf16("kernel.macho.adt"), utf16("kernel.macho")))


if __name__ == "__main__":
    unittest.main(verbosity=2)
