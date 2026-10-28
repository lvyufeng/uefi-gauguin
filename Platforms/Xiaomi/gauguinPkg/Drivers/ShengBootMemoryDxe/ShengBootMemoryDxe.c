/** @file
  Memory and RAM disk from the device tree the previous stage handed over.

  The previous stage (a Linux-style boot loader) passes the physical address
  of a flattened device tree in x0, which SEC keeps in a HOB. Its /memory
  nodes list the real DRAM and its /reserved-memory nodes list what the
  secure side owns. Without the vendor memory manager the firmware only knows
  the few regions of its own memory map, so:

  * DRAM that no firmware region claims and no reserved-memory node covers is
    added to the GCD as system memory, up to a fixed amount;
  * the initrd the loader placed in memory (/chosen linux,initrd-start/-end)
    is published as a read-only block device, so a FAT image in the boot
    image's ramdisk shows up as a normal volume.

  Nothing here depends on a hardware address; every range comes from the DT.
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/FdtLib.h>
#include <Guid/Fdt.h>
#include <Library/HobLib.h>
#include <Library/BootScreenLib.h>

//
// Where this driver leaves its verdict for the menu to read. Must stay inside the "Log Buffer"
// mapping in MemoryMapLib.c and clear of the ring that starts at its base.
//
#define ShengMemSlot  0x9FFFE000ULL

STATIC UINT64  gSecEntryArg0Shadow;
#include <Library/LogBufferLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DiskIo.h>
#include <Protocol/DiskIo2.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/UnicodeCollation.h>
#include <Protocol/DevicePath.h>
#include <Guid/FileInfo.h>
#include <Guid/FileSystemInfo.h>

//
// FAT32 on-disk structures. Only the fields this reader uses are named.
//
#pragma pack(1)

typedef struct {
  UINT8     Jump[3];
  UINT8     OemName[8];
  UINT16    BytesPerSector;
  UINT8     SectorsPerCluster;
  UINT16    ReservedSectors;
  UINT8     NumberOfFats;
  UINT16    RootEntryCount;
  UINT16    TotalSectors16;
  UINT8     Media;
  UINT16    FatSize16;
  UINT16    SectorsPerTrack;
  UINT16    NumberOfHeads;
  UINT32    HiddenSectors;
  UINT32    TotalSectors32;
  UINT32    FatSize32;
  UINT16    ExtFlags;
  UINT16    FileSystemVersion;
  UINT32    RootCluster;
  UINT16    FileSystemInfo;
  UINT16    BackupBootSector;
  UINT8     Reserved[12];
  UINT8     DriveNumber;
  UINT8     Reserved1;
  UINT8     BootSignature;
  UINT32    VolumeId;
  UINT8     VolumeLabel[11];
  UINT8     FileSystemType[8];
} FAT_BOOT_SECTOR;

typedef struct {
  UINT8     Name[11];
  UINT8     Attributes;
  UINT8     NtReserved;
  UINT8     CreationTenths;
  UINT16    CreationTime;
  UINT16    CreationDate;
  UINT16    LastAccessDate;
  UINT16    FirstClusterHigh;
  UINT16    WriteTime;
  UINT16    WriteDate;
  UINT16    FirstClusterLow;
  UINT32    FileSize;
} FAT_DIR_ENTRY;

// VFAT LFN entries contain 5 + 6 + 2 UTF-16 code units: three in the first
// group of ten bytes (five code units, little endian), six in the two groups
// that follow (three each, packing to sixteen bits), and two in the tail dword
// (offsets 28 and 30). Offset 26 is FirstClusterLow, required to be zero and
// never part of the name. The entry is packed, so the first group is read as
// bytes and the remaining groups as packed little-endian fields.
typedef struct {
  UINT8     Sequence;
  UINT8     Name1[10];
  UINT8     Attributes;
  UINT8     Type;
  UINT8     Checksum;
  UINT16    Name2[3];
  UINT16    Name3[3];
  UINT16    Cluster;
  UINT32    Tail;           // code units 11 and 12 at offsets 28 and 30
} FAT_LFN_ENTRY;

STATIC_ASSERT (sizeof (FAT_LFN_ENTRY) == 32, "a long-name entry is one directory entry");

#pragma pack()

#define FAT_ATTRIBUTE_DIRECTORY  0x10
#define FAT_ATTRIBUTE_VOLUME_ID  0x08
#define FAT_ATTRIBUTE_LFN        0x0F

#define FAT_FREE_CLUSTER         0x00000000u
#define FAT_BAD_CLUSTER          0x0FFFFFF7u
#define FAT_END_OF_CHAIN         0x0FFFFFF8u
#define FAT_CLUSTER_MASK         0x0FFFFFFFu

//
// A mounted volume. UniXNU opens files by path, so the reader walks the directory tree from
// RootCluster each time rather than keeping an open-directory object per handle.
//
typedef struct {
  UINT8     *Base;            // first byte of the ramdisk
  UINT64    Size;             // its size in bytes
  UINT32    BytesPerCluster;
  UINT32    FatOffset;        // byte offset of the first FAT copy
  UINT32    DataOffset;       // byte offset of cluster 2
  UINT32    RootCluster;
  UINT32    MaxCluster;       // one past the last usable cluster
  EFI_HANDLE DeviceHandle;
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  SimpleFileSystem;
} FAT_VOLUME;

//
// A position in the volume's directory tree plus, for a file, an offset into it.
//
#define SHENG_FAT_MAX_PATH  260

typedef struct {
  UINT32    ObjectId;
  CHAR16    Path[SHENG_FAT_MAX_PATH];   // absolute, backslash separated, for GetInfo
  UINT8     Attributes;
  UINT32    FirstCluster;
  UINT64    FileSize;
  UINT64    Position;
} SHENG_FAT_OBJECT;

#ifndef FDT_MAGIC
#define FDT_MAGIC  0xD00DFEED
#endif

#define MAX_RANGES        64
#define MIN_PIECE_SIZE    SIZE_16MB
#define ADD_LIMIT         SIZE_1GB
#define ADD_BELOW         SIZE_4GB     // stay inside what the MMU setup maps

typedef struct {
  UINT64  Base;
  UINT64  End;       // exclusive
} RANGE;

STATIC
UINT64
ReadCells (
  IN CONST VOID  *Data,
  IN UINT32      Cells
  )
{
  if (Cells == 2) {
    return Fdt64ToCpu (ReadUnaligned64 (Data));
  }

  return Fdt32ToCpu (ReadUnaligned32 (Data));
}

STATIC
UINT32
GetCells (
  IN CONST VOID  *Fdt,
  IN INT32       Node,
  IN CONST CHAR8 *Name,
  IN UINT32      Default
  )
{
  CONST VOID  *Prop;
  INT32       Length;

  Prop = FdtGetProp (Fdt, Node, Name, &Length);
  if ((Prop == NULL) || (Length != sizeof (UINT32))) {
    return Default;
  }

  return Fdt32ToCpu (ReadUnaligned32 (Prop));
}

STATIC
UINTN
CollectReg (
  IN     CONST VOID  *Fdt,
  IN     INT32       Node,
  IN     UINT32      AddressCells,
  IN     UINT32      SizeCells,
  IN OUT RANGE       *Ranges,
  IN     UINTN       Count
  )
{
  CONST UINT8  *Reg;
  INT32        Length;
  UINT32       Entry;
  INT32        Offset;
  UINT64       Base;
  UINT64       Size;

  Reg = FdtGetProp (Fdt, Node, "reg", &Length);
  if (Reg == NULL) {
    return Count;
  }

  Entry = (AddressCells + SizeCells) * sizeof (UINT32);
  for (Offset = 0; (Offset + (INT32)Entry <= Length) && (Count < MAX_RANGES); Offset += Entry) {
    Base = ReadCells (Reg + Offset, AddressCells);
    Size = ReadCells (Reg + Offset + AddressCells * sizeof (UINT32), SizeCells);
    if ((Size != 0) && (Base + Size > Base)) {
      Ranges[Count].Base = Base;
      Ranges[Count].End  = Base + Size;
      Count++;
    }
  }

  return Count;
}

/**
  Remove every part of Pieces[] that lies inside Cut.
**/
STATIC
UINTN
Subtract (
  IN OUT RANGE  *Pieces,
  IN     UINTN  Count,
  IN     RANGE  Cut
  )
{
  UINTN  Index;
  UINTN  Result;
  RANGE  Out[MAX_RANGES];

  Result = 0;
  for (Index = 0; Index < Count; Index++) {
    if ((Cut.End <= Pieces[Index].Base) || (Cut.Base >= Pieces[Index].End)) {
      if (Result < MAX_RANGES) {
        Out[Result++] = Pieces[Index];
      }

      continue;
    }

    if ((Cut.Base > Pieces[Index].Base) && (Result < MAX_RANGES)) {
      Out[Result].Base  = Pieces[Index].Base;
      Out[Result].End   = Cut.Base;
      Result++;
    }

    if ((Cut.End < Pieces[Index].End) && (Result < MAX_RANGES)) {
      Out[Result].Base  = Cut.End;
      Out[Result].End   = Pieces[Index].End;
      Result++;
    }
  }

  CopyMem (Pieces, Out, Result * sizeof (RANGE));
  return Result;
}

STATIC
VOID
AddMemory (
  IN CONST VOID  *Fdt
  )
{
  EFI_STATUS                       Status;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  *Map;
  UINTN                            MapCount;
  INT32                            Root;
  INT32                            Node;
  INT32                            Reserved;
  INT32                            Length;
  CONST CHAR8                      *Type;
  UINT32                           AddressCells;
  UINT32                           SizeCells;
  RANGE                            Banks[MAX_RANGES];
  RANGE                            Holes[MAX_RANGES];
  RANGE                            Pieces[MAX_RANGES];
  UINTN                            BankCount;
  UINTN                            HoleCount;
  UINTN                            PieceCount;
  UINTN                            ReservedCount;
  RANGE                            ReservedRanges[MAX_RANGES];
  UINTN                            Index;
  UINTN                            Other;
  UINT64                           Added;
  UINT64                           Base;
  UINT64                           End;

  Root         = FdtPathOffset (Fdt, "/");
  AddressCells = GetCells (Fdt, Root, "#address-cells", 2);
  SizeCells    = GetCells (Fdt, Root, "#size-cells", 2);

  BankCount = 0;
  for (Node = FdtFirstSubnode (Fdt, Root); Node >= 0; Node = FdtNextSubnode (Fdt, Node)) {
    Type = FdtGetProp (Fdt, Node, "device_type", &Length);
    if ((Type != NULL) && (AsciiStrCmp (Type, "memory") == 0)) {
      BankCount = CollectReg (Fdt, Node, AddressCells, SizeCells, Banks, BankCount);
    }
  }

  ReservedCount = 0;
  Reserved      = FdtPathOffset (Fdt, "/reserved-memory");
  if (Reserved >= 0) {
    UINT32  ResAddress = GetCells (Fdt, Reserved, "#address-cells", AddressCells);
    UINT32  ResSize    = GetCells (Fdt, Reserved, "#size-cells", SizeCells);

    for (Node = FdtFirstSubnode (Fdt, Reserved); Node >= 0; Node = FdtNextSubnode (Fdt, Node)) {
      ReservedCount = CollectReg (Fdt, Node, ResAddress, ResSize, ReservedRanges, ReservedCount);
    }
  }

  DEBUG ((DEBUG_ERROR, "SHENGMEM: DT has %u memory ranges and %u reserved ranges\n", (UINT32)BankCount, (UINT32)ReservedCount));
  if (BankCount == 0) {
    return;
  }

  DEBUG ((DEBUG_ERROR, "SHENGMEM: gDS=%p, about to call GetMemorySpaceMap\n", gDS));
  Status = gDS->GetMemorySpaceMap (&MapCount, &Map);
  DEBUG ((DEBUG_ERROR, "SHENGMEM: GetMemorySpaceMap = %r, count %u\n", Status, (UINT32)MapCount));
  if (EFI_ERROR (Status)) {
    return;
  }

  //
  // Holes: DRAM the GCD does not know yet.
  //
  HoleCount = 0;
  for (Index = 0; Index < BankCount; Index++) {
    for (Other = 0; Other < MapCount; Other++) {
      if (Map[Other].GcdMemoryType != EfiGcdMemoryTypeNonExistent) {
        continue;
      }

      Base = MAX (Banks[Index].Base, Map[Other].BaseAddress);
      End  = MIN (Banks[Index].End, Map[Other].BaseAddress + Map[Other].Length);
      if ((Base < End) && (HoleCount < MAX_RANGES)) {
        Holes[HoleCount].Base = Base;
        Holes[HoleCount].End  = End;
        HoleCount++;
      }
    }
  }

  FreePool (Map);

  //
  // Minus everything the DT reserves; what is left is added right away.
  //
  Added = 0;
  for (Index = 0; (Index < HoleCount) && (Added < ADD_LIMIT); Index++) {
    Pieces[0]  = Holes[Index];
    PieceCount = 1;
    for (Other = 0; Other < ReservedCount; Other++) {
      PieceCount = Subtract (Pieces, PieceCount, ReservedRanges[Other]);
    }

    for (Other = 0; (Other < PieceCount) && (Added < ADD_LIMIT); Other++) {
      Base = ALIGN_VALUE (Pieces[Other].Base, SIZE_2MB);
      End  = Pieces[Other].End & ~((UINT64)SIZE_2MB - 1);
      if ((End <= Base) || (End - Base < MIN_PIECE_SIZE) || (End > ADD_BELOW)) {
        continue;
      }

      End = MIN (End, Base + (ADD_LIMIT - Added));

      //
      // Add and map before anything can allocate from it.
      //
      Status = gDS->AddMemorySpace (
                      EfiGcdMemoryTypeSystemMemory,
                      Base,
                      End - Base,
                      EFI_MEMORY_UC | EFI_MEMORY_WC | EFI_MEMORY_WT | EFI_MEMORY_WB
                      );
      if (!EFI_ERROR (Status)) {
        Status = gDS->SetMemorySpaceAttributes (Base, End - Base, EFI_MEMORY_WB);
        if (EFI_ERROR (Status)) {
          gDS->RemoveMemorySpace (Base, End - Base);
        } else {
          Added += End - Base;
        }
      }

      DEBUG ((DEBUG_ERROR, "SHENGMEM: system memory 0x%lx-0x%lx %r\n", Base, End - 1, Status));
    }
  }

  DEBUG ((DEBUG_ERROR, "SHENGMEM: added 0x%lx bytes of system memory\n", Added));
}

#define RAM_BLOCK_SIZE  512

#pragma pack(1)
typedef struct {
  VENDOR_DEVICE_PATH          Vendor;
  EFI_DEVICE_PATH_PROTOCOL    End;
} RAM_BLOCK_DEVICE_PATH;
#pragma pack()

STATIC RAM_BLOCK_DEVICE_PATH  mDevicePath = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP, { (UINT8)sizeof (VENDOR_DEVICE_PATH), (UINT8)(sizeof (VENDOR_DEVICE_PATH) >> 8) } },
    { 0xA9C6827C, 0x1BD8, 0x410C, { 0x88, 0x5A, 0xC4, 0x65, 0x00, 0xF7, 0xD8, 0x36 } }
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

STATIC UINT8                    *mRamBase;
STATIC EFI_BLOCK_IO_MEDIA       mMedia;

STATIC
EFI_STATUS
EFIAPI
RamBlockReset (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN BOOLEAN                ExtendedVerification
  )
{
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
RamBlockRead (
  IN  EFI_BLOCK_IO_PROTOCOL  *This,
  IN  UINT32                 MediaId,
  IN  EFI_LBA                Lba,
  IN  UINTN                  BufferSize,
  OUT VOID                   *Buffer
  )
{
  if (Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (MediaId != mMedia.MediaId) {
    return EFI_MEDIA_CHANGED;
  }

  if ((BufferSize % RAM_BLOCK_SIZE) != 0) {
    return EFI_BAD_BUFFER_SIZE;
  }

  if ((Lba > mMedia.LastBlock) || ((BufferSize / RAM_BLOCK_SIZE) > (mMedia.LastBlock + 1 - Lba))) {
    return EFI_INVALID_PARAMETER;
  }

  CopyMem (Buffer, mRamBase + Lba * RAM_BLOCK_SIZE, BufferSize);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
RamBlockWrite (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN UINT32                 MediaId,
  IN EFI_LBA                Lba,
  IN UINTN                  BufferSize,
  IN VOID                   *Buffer
  )
{
  return EFI_WRITE_PROTECTED;
}

STATIC
EFI_STATUS
EFIAPI
RamBlockFlush (
  IN EFI_BLOCK_IO_PROTOCOL  *This
  )
{
  return EFI_SUCCESS;
}

STATIC EFI_BLOCK_IO_PROTOCOL  mBlockIo = {
  EFI_BLOCK_IO_PROTOCOL_REVISION,
  &mMedia,
  RamBlockReset,
  RamBlockRead,
  RamBlockWrite,
  RamBlockFlush
};

/**
  Publish a RAM range as the read-only block device UniXNU reads its payload from.

  Shared by both callers: the /chosen node of a device tree, and the boot image header. The
  second exists because the first cannot work here - see ShengBootMemoryDxeEntry.

  @param[in]  Start  First byte of the ramdisk.
  @param[in]  End    One past the last byte.
**/
/** @file
  A read-only FAT32 filesystem over the ramdisk published in memory.

  See ShengFatReadDxe.h for why this exists instead of FatPkg.

  Copyright (c) 2026, UniXNU contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/


//
// Every open file is one of these. EFI_FILE_PROTOCOL must be its first member so a pointer to
// the protocol is a pointer to the object.
//
typedef struct {
  EFI_FILE_PROTOCOL    File;
  FAT_VOLUME           *Volume;
  SHENG_FAT_OBJECT     Object;
  //
  // Where the last read left off, in the chain.
  //
  // Reaching a position used to mean walking FirstCluster forward one link at a time, every
  // call, from the start. The kernel is fifty-seven megabytes and is read in eight megabyte
  // pieces, so the sixth piece walked forty-two thousand links that the fifth had already
  // walked - and by the last piece the read had become quadratic in the file size. The screen
  // stopped after "DB08 allocated", which read as a hang. Keeping the cluster under the cursor
  // turns each read into one walk of the links it actually needs.
  //
  UINT32               CursorCluster;
  UINT64               CursorPosition;
} FAT_FILE;

STATIC UINT32  mOpenFileCount;

//
// Prototypes for the file protocol handlers, so the templates at the end of this file can name
// them without a forward declaration per function.
//
STATIC EFI_SIMPLE_FILE_SYSTEM_PROTOCOL mSimpleFileSystemTemplate;
STATIC EFI_FILE_PROTOCOL               mRootTemplate;

//
// ---------------------------------------------------------------------------
// On-disk parsing
// ---------------------------------------------------------------------------
//

STATIC
UINT32
ClusterToOffset (
  IN FAT_VOLUME  *Volume,
  IN UINT32      Cluster
  )
{
  return Volume->DataOffset + (Cluster - 2) * Volume->BytesPerCluster;
}

//
// The next cluster in a chain, or FAT_END_OF_CHAIN when the chain stops.
//
STATIC
UINT32
NextCluster (
  IN FAT_VOLUME  *Volume,
  IN UINT32      Cluster
  )
{
  UINT32  Offset;
  UINT32  Value;

  Offset = Volume->FatOffset + Cluster * 4;
  if (Offset + 4 > Volume->Size) {
    return FAT_END_OF_CHAIN;
  }

  CopyMem (&Value, Volume->Base + Offset, sizeof (Value));
  Value &= FAT_CLUSTER_MASK;

  if ((Value < 2) || (Value >= Volume->MaxCluster) || (Value == FAT_BAD_CLUSTER)) {
    return FAT_END_OF_CHAIN;
  }

  return Value;
}

//
// Compare a raw 11-byte directory name with a caller-supplied name.
//
// The on-disk name is space padded and has no dot; the caller's name is a normal "NAME.EXT". The
// comparison folds case, because everything UniXNU asks for is lower case and the volume stores
// upper.
//
STATIC
BOOLEAN
NameMatches (
  IN CONST UINT8   *Raw,
  IN CONST CHAR16  *Name
  )
{
  CHAR8   Wanted[12];
  UINTN   Length;
  UINTN   Index;
  CHAR8   Ch;
  UINT8   Stored;

  if ((Raw[0] == 0x00) || (Raw[0] == 0xE5)) {
    return FALSE;
  }

  //
  // The name as it is stored, before any 8.3 reshaping: eleven characters, space padded.
  //
  // A caller that has the on-disk name already - the short name a directory listing showed it -
  // passes "KERNEL~1MAC", which is eleven characters and has no dot. Treating that as a name
  // plus extension turned it into "KERNEL~1   " and it matched nothing, so a request that
  // named the file exactly failed with EFI_NOT_FOUND.
  //
  {
    CHAR8   Exact[12];
    UINTN   At;

    ZeroMem (Exact, sizeof (Exact));
    SetMem (Exact, 11, ' ');

    for (At = 0; (At < 11) && (Name[At] != 0); At++) {
      Exact[At] = (CHAR8)Name[At];
      if ((Exact[At] >= 'a') && (Exact[At] <= 'z')) {
        Exact[At] = (CHAR8)(Exact[At] - 'a' + 'A');
      }
    }

    if ((Name[At] == 0) && (At == 11)) {
      UINTN  Matches = 0;

      for (At = 0; At < 11; At++) {
        UINT8  Stored = Raw[At];

        if ((Stored >= 'a') && (Stored <= 'z')) {
          Stored = (UINT8)(Stored - 'a' + 'A');
        }

        if (Stored == (UINT8)Exact[At]) {
          Matches++;
        }
      }

      if (Matches == 11) {
        return TRUE;
      }
    }
  }

  //
  // Build the on-disk form of the wanted name: eight characters, a dot if there is an
  // extension, then three.
  //
  ZeroMem (Wanted, sizeof (Wanted));
  SetMem (Wanted, 11, ' ');

  Length = 0;
  for (Index = 0; (Name[Index] != 0) && (Length < 8); Index++) {
    Ch = (CHAR8)Name[Index];
    if (Ch == '.') {
      break;
    }

    Wanted[Length++] = Ch;
  }

  // Do not silently truncate a long basename into an unrelated 8.3 file.
  if ((Name[Index] != 0) && (Name[Index] != '.')) {
    return FALSE;
  }

  if (Name[Index] == '.') {
    Index++;
    Length = 8;
    while ((Name[Index] != 0) && (Length < 11)) {
      Wanted[Length++] = (CHAR8)Name[Index];
      Index++;
    }
  }

  if (Name[Index] != 0) {
    return FALSE;
  }

  for (Index = 0; Index < 11; Index++) {
    Stored = Raw[Index];
    Ch     = Wanted[Index];

    if ((Stored >= 'a') && (Stored <= 'z')) {
      Stored = (UINT8)(Stored - 'a' + 'A');
    }

    if ((Ch >= 'a') && (Ch <= 'z')) {
      Ch = (CHAR8)(Ch - 'a' + 'A');
    }

    if (Stored != (UINT8)Ch) {
      return FALSE;
    }
  }

  return TRUE;
}

//
// Assemble the long name that precedes a directory entry.
//
// The volume stores "unixnu.toml" as LFN entries followed by the 8.3 name UNIXNU~1.TOM, and
// UniXNU asks for the long name. Matching on the short name alone therefore finds nothing on a
// volume that mounts perfectly - which is exactly what happened: one filesystem, and still no
// entries, because every name UniXNU asks for is longer than 8.3.
//
// A long name is stored in sequence-numbered entries, each holding 13 UTF-16 characters, in
// reverse order: the entry with the highest sequence number comes first and the one with bit 6
// of its sequence byte set is the last.
//
STATIC
VOID
AssembleLongName (
  IN  FAT_LFN_ENTRY  *Entry,
  OUT CHAR16         *Name,
  IN  UINTN          NameChars
  )
{
  UINTN   Sequence;
  UINTN   Index;
  UINTN   Out;
  UINT16  Chars[13];
  UINT16  Char;

  Sequence = Entry->Sequence & 0x1F;
  if (Sequence == 0) {
    return;
  }

  Out = (Sequence - 1) * 13;
  if (Out + 13 > NameChars) {
    return;
  }

  //
  // The thirteen characters of one entry, in the order FAT stores them: five in Name1, then
  // six in Name3, then two in Name2. Name1 is a byte array and the others are UTF-16, so the
  // first five are assembled from bytes and the rest read straight through.
  //
  for (Index = 0; Index < 5; Index++) {
    Chars[Index] = (UINT16)(Entry->Name1[Index * 2] | ((UINT16)Entry->Name1[Index * 2 + 1] << 8));
  }

  for (Index = 0; Index < 3; Index++) {
    Chars[5 + Index] = Entry->Name2[Index];
  }

  for (Index = 0; Index < 3; Index++) {
    Chars[8 + Index] = Entry->Name3[Index];
  }

  // Tail holds both code units at byte offsets 28 and 30.
  Chars[11] = (UINT16)Entry->Tail;
  Chars[12] = (UINT16)(Entry->Tail >> 16);

  // Entries arrive highest ordinal first. Never terminate a lower chunk:
  // doing so would erase the continuation already stored by the higher one.
  for (Index = 0; Index < 13; Index++) {
    Char = Chars[Index];
    Name[Out + Index] = (CHAR16)Char;
  }
}

STATIC
UINT8
ShortNameChecksum (IN CONST UINT8 *Raw)
{
  UINT8 Sum = 0;
  UINTN Index;
  for (Index = 0; Index < 11; Index++) {
    Sum = (UINT8)(((Sum & 1) << 7) + (Sum >> 1) + Raw[Index]);
  }
  return Sum;
}

// Validate termination/padding and the 255-code-unit VFAT limit before
// exposing the assembled name to an unbounded string comparison.
STATIC
BOOLEAN
FinishLongName (IN OUT CHAR16 *Name, IN UINTN Count)
{
  UINTN Index;
  UINTN Length = Count;
  BOOLEAN Ended = FALSE;
  for (Index = 0; Index < Count; Index++) {
    if (!Ended) {
      if (Name[Index] == 0) {
        Length = Index;
        Ended = TRUE;
      } else if (Name[Index] == 0xFFFF) {
        return FALSE;
      }
    } else if (Name[Index] != 0xFFFF) {
      return FALSE;
    }
  }
  if ((Length == 0) || (Length > 255)) {
    return FALSE;
  }
  Name[Length] = 0;
  return TRUE;
}

//
// Case-insensitive comparison of a stored long name with what the caller asked for.
//
STATIC
BOOLEAN
LongNameMatches (
  IN CONST CHAR16  *Stored,
  IN CONST CHAR16  *Wanted
  )
{
  UINTN  Index;
  CHAR16 A;
  CHAR16 B;

  for (Index = 0; ; Index++) {
    A = Stored[Index];
    B = Wanted[Index];

    if ((A >= L'a') && (A <= L'z')) {
      A = (CHAR16)(A - L'a' + L'A');
    }

    if ((B >= L'a') && (B <= L'z')) {
      B = (CHAR16)(B - L'a' + L'A');
    }

    if (A != B) {
      return FALSE;
    }

    if (A == 0) {
      return TRUE;
    }
  }
}

//
// Find one entry in a directory, by name.
//
// Root is addressed by its first cluster; a subdirectory too. Returns the entry's cluster and
// size, and whether it is a directory. Long names are collected as they are walked and used
// first; the 8.3 name is the fallback for entries that have none.
//
STATIC
EFI_STATUS
FindInDirectory (
  IN  FAT_VOLUME       *Volume,
  IN  UINT32           DirectoryCluster,
  IN  CONST CHAR16     *Name,
  OUT FAT_DIR_ENTRY    *Found
  )
{
  UINT32         Cluster;
  UINT32         Offset;
  UINT32         Index;
  FAT_DIR_ENTRY  *Entry;
  CHAR16         LongName[261];
  BOOLEAN        HasLong;
  UINT8          Expected;
  UINT8          Checksum;
  UINTN          LongCount;
  UINTN          Visited;

  Cluster = DirectoryCluster;
  ZeroMem (LongName, sizeof (LongName));
  HasLong = FALSE;
  Expected = 0;
  Checksum = 0;
  LongCount = 0;
  Visited = 0;

  while (Cluster != FAT_END_OF_CHAIN) {
    if ((Cluster < 2) || (Cluster >= Volume->MaxCluster) ||
        (++Visited > Volume->MaxCluster)) {
      return EFI_VOLUME_CORRUPTED;
    }
    Offset = ClusterToOffset (Volume, Cluster);
    if ((UINT64)Offset + Volume->BytesPerCluster > Volume->Size) {
      return EFI_VOLUME_CORRUPTED;
    }

    for (Index = 0; Index < Volume->BytesPerCluster; Index += sizeof (FAT_DIR_ENTRY)) {
      Entry = (FAT_DIR_ENTRY *)(Volume->Base + Offset + Index);

      if (Entry->Name[0] == 0x00) {
        return EFI_NOT_FOUND;
      }

      if (Entry->Name[0] == 0xE5) {
        HasLong = FALSE;
        continue;
      }

      if (Entry->Attributes == FAT_ATTRIBUTE_LFN) {
        FAT_LFN_ENTRY *Lfn = (FAT_LFN_ENTRY *)Entry;
        UINT8 Ordinal = Lfn->Sequence & 0x1F;
        if ((Lfn->Sequence & 0x40) != 0) {
          ZeroMem (LongName, sizeof (LongName));
          HasLong = TRUE;
          Expected = Ordinal;
          Checksum = Lfn->Checksum;
          LongCount = (UINTN)Ordinal * 13;
        }
        if (!HasLong || (Ordinal == 0) || (Ordinal > 20) ||
            ((Lfn->Sequence & 0xA0) != 0) || (Ordinal != Expected) ||
            (Lfn->Type != 0) || (Lfn->Cluster != 0) ||
            (Lfn->Checksum != Checksum)) {
          HasLong = FALSE;
          continue;
        }
        AssembleLongName (Lfn, LongName, ARRAY_SIZE (LongName));
        Expected--;
        continue;
      }

      if ((Entry->Attributes & FAT_ATTRIBUTE_VOLUME_ID) != 0) {
        HasLong = FALSE;
        continue;
      }

      if (HasLong && (Expected == 0) &&
          (ShortNameChecksum (Entry->Name) == Checksum) &&
          FinishLongName (LongName, LongCount) && LongNameMatches (LongName, Name)) {
        CopyMem (Found, Entry, sizeof (*Found));
        return EFI_SUCCESS;
      }

      if (NameMatches (Entry->Name, Name)) {
        CopyMem (Found, Entry, sizeof (*Found));
        return EFI_SUCCESS;
      }

      ZeroMem (LongName, sizeof (LongName));
      HasLong = FALSE;
    }

    Cluster = NextCluster (Volume, Cluster);
  }

  return EFI_NOT_FOUND;
}

//
// Resolve a path against the volume, starting at the root.
//
// Accepts backslashes and forward slashes, leading separators, and "." components. ".." is
// handled by restarting from the root, which is not what FAT means but is what a reader that
// only ever walks down needs.
//
STATIC
EFI_STATUS
LookupPath (
  IN  FAT_VOLUME        *Volume,
  IN  CONST CHAR16      *Path,
  OUT SHENG_FAT_OBJECT  *Object
  )
{
  //
  // Long enough for a 255-character name: FAT allows names far longer than 8.3, and a buffer
  // of thirteen silently truncated "DarwinBoot.efi" to "DarwinBoot.ef" - which then matched
  // nothing. "unixnu.toml" is eleven characters and happened to fit, which is why the
  // configuration file was found and the driver beside it was not.
  //
  CHAR16         Component[SHENG_FAT_MAX_PATH];
  UINTN          Length;
  UINTN          Index;
  UINT32         Cluster;
  FAT_DIR_ENTRY  Entry;
  EFI_STATUS     Status;

  ZeroMem (Object, sizeof (*Object));
  Object->Path[0]      = L'\\';
  Object->Path[1]      = 0;
  Object->Attributes   = FAT_ATTRIBUTE_DIRECTORY;
  Object->FirstCluster = Volume->RootCluster;
  Cluster              = Volume->RootCluster;
  Index                = 0;

  while (Path[Index] != 0) {
    if ((Path[Index] == L'\\') || (Path[Index] == L'/')) {
      Index++;
      continue;
    }

    //
    // One component.
    //
    Length = 0;
    while ((Path[Index] != 0) && (Path[Index] != L'\\') && (Path[Index] != L'/')) {
      if (Length + 1 < ARRAY_SIZE (Component)) {
        Component[Length++] = Path[Index];
      }

      Index++;
    }

    Component[Length] = 0;

    if ((Length == 1) && (Component[0] == L'.')) {
      continue;
    }

    Status = FindInDirectory (Volume, Cluster, Component, &Entry);

    //
    // Record which component was looked for and what came back. A file opened under the wrong
    // name looks exactly like a file opened correctly until something reports its size.
    //
    // Its own counter, not Index: reusing the walk's own cursor left it pointing past the end
    // of the name that was being walked, so every path after the first component was skipped.
    //
    {
      UINT64  *Slot = (UINT64 *)(UINTN)ShengMemSlot;
      UINTN   Trace;

      for (Trace = 0; (Trace < 16) && (Component[Trace] != 0); Trace++) {
        ((CHAR16 *)(UINTN)(Slot + 24))[Trace] = Component[Trace];
      }

      ((CHAR16 *)(UINTN)(Slot + 24))[Trace] = 0;
      Slot[16] = (UINT64)Status;
      Slot[17] = (UINT64)Entry.FileSize;
    }

    if (EFI_ERROR (Status)) {
      return Status;
    }

    Cluster = ((UINT32)Entry.FirstClusterHigh << 16) | Entry.FirstClusterLow;

    //
    // Append to the recorded path. A directory entry with its attribute set but a zero first
    // cluster is the volume's own root, which has no cluster of its own in FAT32.
    //
    if (Cluster == 0) {
      Cluster = Volume->RootCluster;
    }

    Length = StrLen (Object->Path);
    if (Length > 1) {
      Object->Path[Length++] = L'\\';
      Object->Path[Length]   = 0;
    }

    StrCatS (Object->Path, ARRAY_SIZE (Object->Path), Component);

    Object->Attributes   = Entry.Attributes;
    Object->FirstCluster = Cluster;
    Object->FileSize     = Entry.FileSize;
  }

  Object->Position = 0;
  return EFI_SUCCESS;
}

//
// ---------------------------------------------------------------------------
// EFI_FILE_PROTOCOL
// ---------------------------------------------------------------------------
//

STATIC
EFI_FILE_PROTOCOL *
NewFile (
  IN FAT_VOLUME        *Volume,
  IN CONST SHENG_FAT_OBJECT  *Object
  )
{
  FAT_FILE  *File;

  File = AllocateZeroPool (sizeof (*File));
  if (File == NULL) {
    return NULL;
  }

  CopyMem (&File->File, &mRootTemplate, sizeof (File->File));
  File->Volume = Volume;
  CopyMem (&File->Object, Object, sizeof (File->Object));
  mOpenFileCount++;
  return &File->File;
}

STATIC
EFI_STATUS
EFIAPI
FatOpen (
  IN     EFI_FILE_PROTOCOL  *This,
  OUT    EFI_FILE_PROTOCOL  **NewHandle,
  IN     CHAR16             *FileName,
  IN     UINT64             OpenMode,
  IN     UINT64             Attributes
  )
{
  FAT_FILE           *File = (FAT_FILE *)This;
  SHENG_FAT_OBJECT   Object;
  EFI_FILE_PROTOCOL  *Next;
  EFI_STATUS         Status;

  (VOID)Attributes;

  if ((OpenMode & ~(EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE)) != 0) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // Read only: a caller that wants to create or write is told so here rather than later.
  //
  if ((OpenMode & (EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE)) != 0) {
    return EFI_WRITE_PROTECTED;
  }

  if (FileName == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // Anything that moves upwards starts again from the root; a reader that only walks down
  // treats it as absent rather than resolving it.
  //
  if (StrStr (FileName, L"..") != NULL) {
    return EFI_NOT_FOUND;
  }

  //
  // Leave a trace of the last Open, where the menu can read it.
  //
  // LoadImage asks for a driver by device path and reports only EFI_NOT_FOUND, which does not
  // say whether the name was wrong, the path reached this code at all, or the walk failed.
  // Recording the name and the outcome separates those.
  //
  {
    UINT64  *Slot = (UINT64 *)(UINTN)ShengMemSlot;
    UINTN   Index;

    for (Index = 0; (Index < 16) && (FileName[Index] != 0); Index++) {
      ((CHAR16 *)(UINTN)(Slot + 20))[Index] = FileName[Index];
    }

    ((CHAR16 *)(UINTN)(Slot + 20))[Index] = 0;
    Slot[19] = (Slot[19] & 0xFFFF0000ULL) | (Index & 0xFFFF);
  }

  Status = LookupPath (File->Volume, FileName, &Object);
  ((UINT64 *)(UINTN)ShengMemSlot)[18] = (UINT64)Status;
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Next = NewFile (File->Volume, &Object);
  if (Next == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  *NewHandle = Next;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
FatClose (
  IN EFI_FILE_PROTOCOL  *This
  )
{
  FAT_FILE  *File = (FAT_FILE *)This;

  if (mOpenFileCount != 0) {
    mOpenFileCount--;
  }

  if ((EFI_FILE_PROTOCOL *)File != &mRootTemplate) {
    FreePool (File);
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
FatDelete (
  IN EFI_FILE_PROTOCOL  *This
  )
{
  (VOID)This;
  return EFI_WRITE_PROTECTED;
}

STATIC
EFI_STATUS
EFIAPI
FatRead (
  IN     EFI_FILE_PROTOCOL  *This,
  IN OUT UINTN              *BufferSize,
  OUT    VOID               *Buffer
  )
{
  FAT_FILE  *File = (FAT_FILE *)This;
  UINT32    Cluster;
  UINT32    Skip;
  UINT32    Offset;
  UINT32    Chunk;
  UINT32    Count;
  UINT64    Remaining;
  UINT64    Wanted;
  UINT8     *Out;
  UINT32    Index;

  if ((BufferSize == NULL) || (Buffer == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if ((File->Object.Attributes & FAT_ATTRIBUTE_DIRECTORY) != 0) {
    //
    // A read of something the walk decided was a directory. Record what it was: the name that
    // was opened, the attribute it came back with, and its size, so the wrong entry can be
    // identified rather than guessed at.
    //
    UINT64  *Slot = (UINT64 *)(UINTN)ShengMemSlot;

    Slot[21] = (UINT64)File->Object.Attributes;
    Slot[22] = (UINT64)File->Object.FileSize;
    Slot[23] = (UINT64)File->Object.FirstCluster;
    return EFI_UNSUPPORTED;
  }

  if (File->Object.Position >= File->Object.FileSize) {
    *BufferSize = 0;
    return EFI_SUCCESS;
  }

  Remaining = File->Object.FileSize - File->Object.Position;
  Wanted    = *BufferSize;
  if (Wanted > Remaining) {
    Wanted = Remaining;
  }

  //
  // Walk the chain to the cluster holding the current position, then copy forwards.
  //
  // The walk starts at the cursor when the cursor is behind the position and closer than the
  // head of the chain is; otherwise it starts at the first cluster.
  //
  if ((File->CursorCluster != 0) && (File->CursorPosition <= File->Object.Position)) {
    Cluster = File->CursorCluster;
    Skip    = (UINT32)((File->Object.Position - File->CursorPosition) / File->Volume->BytesPerCluster);
  } else {
    Cluster = File->Object.FirstCluster;
    Skip    = (UINT32)(File->Object.Position / File->Volume->BytesPerCluster);
  }

  while ((Skip != 0) && (Cluster != FAT_END_OF_CHAIN)) {
    Cluster = NextCluster (File->Volume, Cluster);
    Skip--;
  }

  if (Cluster == FAT_END_OF_CHAIN) {
    return EFI_VOLUME_CORRUPTED;
  }

  Offset = (UINT32)(File->Object.Position % File->Volume->BytesPerCluster);
  Out    = (UINT8 *)Buffer;

  //
  // A cluster at a time, not a byte at a time.
  //
  // The kernel is fifty-seven megabytes, and copying it one byte per loop - each iteration
  // recomputing the cluster's offset and re-testing it against the volume size - took long
  // enough that the screen looked frozen. CopyMem moves a whole cluster per step and the
  // arithmetic happens once per cluster instead of once per byte.
  //
  // Index counts what has been copied; the remainder of a cluster is the unit here, so a
  // partial cluster at the end is handled by taking the smaller of the two counts.
  //
  Index = 0;
  while ((UINT64)Index < Wanted) {
    if (Offset >= File->Volume->BytesPerCluster) {
      Cluster = NextCluster (File->Volume, Cluster);
      if (Cluster == FAT_END_OF_CHAIN) {
        break;
      }

      Offset = 0;
    }

    Chunk = ClusterToOffset (File->Volume, Cluster) + Offset;
    if ((UINT64)Chunk >= File->Volume->Size) {
      break;
    }

    Count = File->Volume->BytesPerCluster - Offset;
    if (Count > Wanted - Index) {
      Count = (UINT32)(Wanted - Index);
    }

    if ((UINT64)Chunk + Count > File->Volume->Size) {
      Count = (UINT32)(File->Volume->Size - Chunk);
    }

    CopyMem (Out + Index, File->Volume->Base + Chunk, Count);
    Index  += Count;
    Offset += Count;
  }

  //
  // Remember the cluster this read ended on, and the file position that cluster starts at.
  //
  // The two have to describe the same place: the next read walks from CursorCluster a number
  // of links computed as (Position - CursorPosition) / BytesPerCluster, so storing the position
  // the read started at together with the cluster it ended on skips a whole piece of the file
  // on every call. That is what "Volume Corrupt" at 40 MiB was: the walk ran past the end of
  // the chain because it had been told the wrong place to start.
  //
  if (Index != 0) {
    File->CursorCluster  = Cluster;
    File->CursorPosition = File->Object.Position + Index - Offset;
  }

  *BufferSize            = Index;
  File->Object.Position += Index;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
FatWrite (
  IN     EFI_FILE_PROTOCOL  *This,
  IN OUT UINTN              *BufferSize,
  IN     VOID               *Buffer
  )
{
  (VOID)This;
  (VOID)BufferSize;
  (VOID)Buffer;
  return EFI_WRITE_PROTECTED;
}

STATIC
EFI_STATUS
EFIAPI
FatGetPosition (
  IN  EFI_FILE_PROTOCOL  *This,
  OUT UINT64             *Position
  )
{
  FAT_FILE  *File = (FAT_FILE *)This;

  if (Position == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *Position = File->Object.Position;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
FatSetPosition (
  IN EFI_FILE_PROTOCOL  *This,
  IN UINT64             Position
  )
{
  FAT_FILE  *File = (FAT_FILE *)This;

  File->Object.Position = Position;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
FatGetInfo (
  IN     EFI_FILE_PROTOCOL  *This,
  IN     EFI_GUID           *InformationType,
  IN OUT UINTN              *BufferSize,
  OUT    VOID               *Buffer
  )
{
  FAT_FILE         *File = (FAT_FILE *)This;
  EFI_FILE_INFO    *Info;
  UINTN            Needed;
  UINTN            NameChars;

  if ((InformationType == NULL) || (BufferSize == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (CompareGuid (InformationType, &gEfiFileInfoGuid)) {
    ((UINT64 *)(UINTN)ShengMemSlot)[24] = 1;
    NameChars = StrLen (File->Object.Path) + 1;
    ((UINT64 *)(UINTN)ShengMemSlot)[25] = (UINT64)NameChars;
    Needed    = SIZE_OF_EFI_FILE_INFO + NameChars * sizeof (CHAR16);
    ((UINT64 *)(UINTN)ShengMemSlot)[26] = (UINT64)Needed;
    ((UINT64 *)(UINTN)ShengMemSlot)[27] = (UINT64)*BufferSize;

    if (*BufferSize < Needed) {
      *BufferSize = Needed;
      return EFI_BUFFER_TOO_SMALL;
    }

    ((UINT64 *)(UINTN)ShengMemSlot)[28] = 2;

    if (Buffer == NULL) {
      return EFI_INVALID_PARAMETER;
    }

    Info = (EFI_FILE_INFO *)Buffer;
    ZeroMem (Info, Needed);
    Info->Size     = Needed;
    Info->Attribute = ((File->Object.Attributes & FAT_ATTRIBUTE_DIRECTORY) != 0)
                      ? EFI_FILE_DIRECTORY
                      : 0;
    Info->FileSize = File->Object.FileSize;
    Info->PhysicalSize = File->Object.FileSize;
    //
    // A plain copy, not StrCpyS.
    //
    // The length is already known and the destination was sized from it, so the safety checks
    // the safe-string helper performs add nothing - and on this build the call does not come
    // back, which is why reading the kernel stopped at "getinfo" with no error.
    //
    if (NameChars > 0) {
      CopyMem (Info->FileName, File->Object.Path, NameChars * sizeof (CHAR16));
    }

    //
    // What this reader told its caller about the file it just opened: the size and the
    // attribute are the two values a loader checks before reading, and a wrong pair here -
    // a size of zero in particular - makes it refuse without ever touching the cluster chain.
    //
    ((UINT64 *)(UINTN)ShengMemSlot)[14] = (UINT64)Info->FileSize;
    ((UINT64 *)(UINTN)ShengMemSlot)[15] = (UINT64)Info->Attribute;

    *BufferSize = Needed;
    return EFI_SUCCESS;
  }

  if (CompareGuid (InformationType, &gEfiFileSystemInfoGuid)) {
    EFI_FILE_SYSTEM_INFO  *VolumeInfo;

    Needed = SIZE_OF_EFI_FILE_SYSTEM_INFO + 12 * sizeof (CHAR16);

    if (*BufferSize < Needed) {
      *BufferSize = Needed;
      return EFI_BUFFER_TOO_SMALL;
    }

    if (Buffer == NULL) {
      return EFI_INVALID_PARAMETER;
    }

    VolumeInfo              = (EFI_FILE_SYSTEM_INFO *)Buffer;
    ZeroMem (VolumeInfo, Needed);
    VolumeInfo->Size        = Needed;
    VolumeInfo->ReadOnly    = TRUE;
    VolumeInfo->VolumeSize  = File->Volume->Size;
    VolumeInfo->FreeSpace   = 0;
    VolumeInfo->BlockSize   = File->Volume->BytesPerCluster;
    StrCpyS (VolumeInfo->VolumeLabel, 12, L"UNIXNU-GAU");

    *BufferSize = Needed;
    return EFI_SUCCESS;
  }

  return EFI_UNSUPPORTED;
}

STATIC
EFI_STATUS
EFIAPI
FatSetInfo (
  IN EFI_FILE_PROTOCOL  *This,
  IN EFI_GUID           *InformationType,
  IN UINTN              BufferSize,
  IN VOID               *Buffer
  )
{
  (VOID)This;
  (VOID)InformationType;
  (VOID)BufferSize;
  (VOID)Buffer;
  return EFI_WRITE_PROTECTED;
}

STATIC
EFI_STATUS
EFIAPI
FatFlush (
  IN EFI_FILE_PROTOCOL  *This
  )
{
  (VOID)This;
  return EFI_SUCCESS;
}

//
// ---------------------------------------------------------------------------
// EFI_SIMPLE_FILE_SYSTEM_PROTOCOL
// ---------------------------------------------------------------------------
//

STATIC
EFI_STATUS
EFIAPI
FatOpenVolume (
  IN  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *This,
  OUT EFI_FILE_PROTOCOL                **Root
  )
{
  FAT_VOLUME        *Volume;
  SHENG_FAT_OBJECT  Object;
  EFI_FILE_PROTOCOL *File;

  Volume = BASE_CR (This, FAT_VOLUME, SimpleFileSystem);

  ZeroMem (&Object, sizeof (Object));
  Object.Path[0]      = L'\\';
  Object.Path[1]      = 0;
  Object.Attributes   = FAT_ATTRIBUTE_DIRECTORY;
  Object.FirstCluster = Volume->RootCluster;

  File = NewFile (Volume, &Object);
  if (File == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  *Root = File;
  return EFI_SUCCESS;
}

//
// ---------------------------------------------------------------------------
// Mount
// ---------------------------------------------------------------------------
//

/**
  Mount a FAT32 image held in memory and publish it as a filesystem.

  @param[in]  Base      First byte of the image.
  @param[in]  Size      Its size in bytes.
  @param[in]  VolumeOut Receives the volume on success.

  @retval EFI_SUCCESS           The image is FAT32 and the volume is parsed.
  @retval EFI_VOLUME_CORRUPTED  The image is not a FAT32 volume this reader understands.
**/
EFI_STATUS
EFIAPI
ShengFatMount (
  IN  UINT8        *Base,
  IN  UINT64       Size,
  OUT FAT_VOLUME   **VolumeOut
  )
{
  FAT_BOOT_SECTOR  *Boot;
  FAT_VOLUME       *Volume;
  UINT32           ReservedSectors;
  UINT32           FatSectors;
  UINT32           DataSectors;
  UINT32           Clusters;

  if ((Base == NULL) || (Size < sizeof (FAT_BOOT_SECTOR))) {
    return EFI_VOLUME_CORRUPTED;
  }

  Boot = (FAT_BOOT_SECTOR *)Base;

  //
  // The boot sector ends with 0x55 0xAA. The struct above stops short of it on purpose: the
  // signature sits at offset 510 and the struct is the meaningful 64 bytes of the header.
  //
  {
    UINT16  Signature;

    CopyMem (&Signature, Base + 510, sizeof (Signature));
    if (Signature != 0xAA55) {
      return EFI_VOLUME_CORRUPTED;
    }
  }

  if ((Boot->BytesPerSector != 512) && (Boot->BytesPerSector != 1024) &&
      (Boot->BytesPerSector != 2048) && (Boot->BytesPerSector != 4096))
  {
    return EFI_VOLUME_CORRUPTED;
  }

  if (Boot->SectorsPerCluster == 0) {
    return EFI_VOLUME_CORRUPTED;
  }

  //
  // FAT32 is the variant with a 32-bit FAT size and a root cluster; FAT12/16 keep the root
  // directory in a fixed area and are not what this platform writes.
  //
  if ((Boot->FatSize32 == 0) || (Boot->RootCluster < 2)) {
    return EFI_VOLUME_CORRUPTED;
  }

  ReservedSectors = Boot->ReservedSectors;
  FatSectors      = Boot->FatSize32;

  Volume = AllocateZeroPool (sizeof (*Volume));
  if (Volume == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Volume->Base            = Base;
  Volume->Size            = Size;
  Volume->BytesPerCluster = (UINT32)Boot->BytesPerSector * Boot->SectorsPerCluster;
  Volume->FatOffset       = ReservedSectors * Boot->BytesPerSector;
  Volume->RootCluster     = Boot->RootCluster;
  Volume->DataOffset      = (ReservedSectors + Boot->NumberOfFats * FatSectors) * Boot->BytesPerSector;

  if (Volume->DataOffset >= Size) {
    FreePool (Volume);
    return EFI_VOLUME_CORRUPTED;
  }

  DataSectors     = (UINT32)((Size - Volume->DataOffset) / Boot->BytesPerSector);
  Clusters        = DataSectors / Boot->SectorsPerCluster;
  Volume->MaxCluster = Clusters + 2;

  if ((Volume->MaxCluster <= 2) || (Volume->RootCluster >= Volume->MaxCluster)) {
    FreePool (Volume);
    return EFI_VOLUME_CORRUPTED;
  }

  CopyMem (&Volume->SimpleFileSystem, &mSimpleFileSystemTemplate, sizeof (Volume->SimpleFileSystem));

  *VolumeOut = Volume;
  return EFI_SUCCESS;
}

/**
  Mount the image and publish it as a filesystem, as one call.

  ShengBootMemoryDxe publishes the ramdisk and calls this; the volume is handed to the
  firmware as EFI_SIMPLE_FILE_SYSTEM_PROTOCOL on the same handle, so anything looking for a
  filesystem - UniXNU does - finds it exactly where the block device is.

  @param[in]  Base      First byte of the FAT32 image.
  @param[in]  Size      Its size in bytes.
  @param[in]  Handle    Handle to attach the filesystem to.
  @param[in]  DevicePath The handle's device path, kept for the volume record.

  @retval EFI_SUCCESS  The volume is published.
**/
EFI_STATUS
EFIAPI
ShengFatPublish (
  IN UINT8                     *Base,
  IN UINT64                    Size,
  IN EFI_HANDLE                Handle,
  IN EFI_DEVICE_PATH_PROTOCOL  *DevicePath
  )
{
  FAT_VOLUME  *Volume;
  EFI_STATUS  Status;

  Status = ShengFatMount (Base, Size, &Volume);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SHENGFAT: not a FAT32 volume (%r)\n", Status));
    return Status;
  }

  Volume->DeviceHandle = Handle;
  Volume->DevicePath   = DevicePath;

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle,
                  &gEfiSimpleFileSystemProtocolGuid,
                  &Volume->SimpleFileSystem,
                  NULL
                  );
  DEBUG ((
    DEBUG_ERROR,
    "SHENGFAT: %u byte clusters, root cluster %u, %u file reports %r\n",
    Volume->BytesPerCluster,
    Volume->RootCluster,
    mOpenFileCount,
    Status
    ));

  return Status;
}

STATIC
VOID
PublishRamdisk (
  IN UINT64  Start,
  IN UINT64  End
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  Handle;

  if ((End <= Start) || (((End - Start) % RAM_BLOCK_SIZE) != 0)) {
    DEBUG ((DEBUG_ERROR, "SHENGMEM: ramdisk 0x%lx-0x%lx is not a whole number of sectors\n", Start, End));
    return;
  }

  mRamBase                = (UINT8 *)(UINTN)Start;
  mMedia.MediaId          = 1;
  mMedia.RemovableMedia   = FALSE;
  mMedia.MediaPresent     = TRUE;
  mMedia.LogicalPartition = FALSE;
  mMedia.ReadOnly         = TRUE;
  mMedia.WriteCaching     = FALSE;
  mMedia.BlockSize        = RAM_BLOCK_SIZE;
  mMedia.IoAlign          = 1;
  mMedia.LastBlock        = (End - Start) / RAM_BLOCK_SIZE - 1;

  Handle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle,
                  &gEfiDevicePathProtocolGuid,
                  &mDevicePath,
                  &gEfiBlockIoProtocolGuid,
                  &mBlockIo,
                  NULL
                  );
  DEBUG ((DEBUG_ERROR, "SHENGMEM: ramdisk block device 0x%lx size 0x%lx %r\n", Start, End - Start, Status));

  //
  // Ask for the filesystem now, while the firmware still owns the machine.
  //
  // Fat is listed in APRIORI right after this driver and still ends up in the dispatcher's
  // "left over" list, so nothing ever mounts the ramdisk and UniXNU reports one block device
  // and no filesystems. Connecting here does not depend on the dispatcher reaching Fat: the
  // handle exists at this point, and the binding is what Fat does when handed one.
  //
  // Non-recursive on purpose - the children of a ramdisk this driver synthesised are not a
  // tree the dispatcher can walk.
  //
  if (!EFI_ERROR (Status)) {
    //
    // Mount the volume here, in this driver, with a reader of our own.
    //
    // FatPkg is packed and never dispatched on this board: DiskIo and UnicodeCollation2 never
    // appear, so Fat's dependency expression never becomes true and a published ramdisk is
    // left with no filesystem on it. Every attempt to reach Fat - ConnectController from here,
    // ConnectController from the menu, explicit depex - ended the same way, with one block
    // device and zero filesystems.
    //
    // ShengFatReadDxe parses FAT32 out of these same bytes and installs
    // EFI_SIMPLE_FILE_SYSTEM_PROTOCOL on this handle. Nothing has to be dispatched for it.
    //
    Status = ShengFatPublish (mRamBase, End - Start, Handle, (EFI_DEVICE_PATH_PROTOCOL *)&mDevicePath);
    ((UINT64 *)(UINTN)ShengMemSlot)[6] = (UINT64)Status;

    //
    // Install the device tree as a configuration table.
    //
    // The boot image carries one, and UniXNU looks for it there when an entry asks for the
    // "firmware" tree. Without it, starting such an entry ends with EFI_NOT_FOUND before the
    // kernel is touched at all - which is exactly what the menu reported for a payload it had
    // otherwise read correctly.
    //
    // It travels inside the kernel blob, after the ramdisk: the bootloader keeps no copy of the
    // boot image, so the decompressed kernel is the only place the tree reaches memory. The
    // ramdisk's own size says where it starts, and the tree's header says how long it is.
    //
    {
      UINT64          DtbAddress;
      UINT32          Magic;
      UINT32          TotalSize;
      EFI_STATUS      Table;

      DtbAddress = Start + (UINT64)((mMedia.LastBlock + 1) * RAM_BLOCK_SIZE);
      Magic      = *(UINT32 *)(UINTN)DtbAddress;

      if (Magic == FDT_MAGIC) {
        TotalSize = *(UINT32 *)(UINTN)(DtbAddress + 4);

        Table = gBS->InstallConfigurationTable (
                       &gFdtTableGuid,
                       (VOID *)(UINTN)DtbAddress
                       );
        ((UINT64 *)(UINTN)ShengMemSlot)[12] = (UINT64)Table;
        ((UINT64 *)(UINTN)ShengMemSlot)[13] = TotalSize;
        DEBUG ((DEBUG_ERROR, "SHENGMEM: device tree at 0x%lx, %u bytes, table %r\n", DtbAddress, TotalSize, Table));
      } else {
        ((UINT64 *)(UINTN)ShengMemSlot)[12] = 0xFDFDFDFD;
        DEBUG ((DEBUG_ERROR, "SHENGMEM: no device tree at 0x%lx (word 0x%x)\n", DtbAddress, Magic));
      }
    }

    //
    // Look for the configuration file right here, through the same code the menu will use.
    //
    // A volume that mounts and yields no entries has two possible causes - names that do not
    // match, or a walk that does not work - and they look identical from the menu. Finding the
    // file from the publisher separates them and leaves the answer where the menu can show it.
    //
    {
      FAT_VOLUME       *Volume;
      SHENG_FAT_OBJECT  Object;
      EFI_STATUS        Lookup;

      if (!EFI_ERROR (ShengFatMount (mRamBase, End - Start, &Volume))) {
        FAT_DIR_ENTRY  Probe;

        //
        // Each component in turn, so a failure names the step rather than the whole path.
        //
        Lookup = FindInDirectory (Volume, Volume->RootCluster, L"EFI", &Probe);
        ((UINT64 *)(UINTN)ShengMemSlot)[10] = (UINT64)Lookup;

        if (!EFI_ERROR (Lookup)) {
          UINT32  Next;

          Next   = ((UINT32)Probe.FirstClusterHigh << 16) | Probe.FirstClusterLow;
          Lookup = FindInDirectory (Volume, Next, L"UniXNU", &Probe);
          ((UINT64 *)(UINTN)ShengMemSlot)[11] = (UINT64)Lookup;
        }

        Lookup = LookupPath (Volume, L"\\EFI\\UniXNU\\unixnu.toml", &Object);
        ((UINT64 *)(UINTN)ShengMemSlot)[7] = (UINT64)Lookup;
        ((UINT64 *)(UINTN)ShengMemSlot)[8] = Object.FileSize;
        FreePool (Volume);
      } else {
        ((UINT64 *)(UINTN)ShengMemSlot)[7] = 0xEEEE0000;
      }
    }
  }
}

STATIC
VOID
PublishInitrd (
  IN CONST VOID  *Fdt
  )
{
  INT32       Chosen;
  CONST VOID  *StartProp;
  CONST VOID  *EndProp;
  INT32       StartLength;
  INT32       EndLength;
  UINT64      Start;
  UINT64      End;

  Chosen = FdtPathOffset (Fdt, "/chosen");
  if (Chosen < 0) {
    DEBUG ((DEBUG_ERROR, "SHENGMEM: no /chosen\n"));
    return;
  }

  StartProp = FdtGetProp (Fdt, Chosen, "linux,initrd-start", &StartLength);
  EndProp   = FdtGetProp (Fdt, Chosen, "linux,initrd-end", &EndLength);
  if ((StartProp == NULL) || (EndProp == NULL) ||
      ((StartLength != 4) && (StartLength != 8)) || ((EndLength != 4) && (EndLength != 8)))
  {
    DEBUG ((DEBUG_ERROR, "SHENGMEM: no initrd in /chosen\n"));
    return;
  }

  Start = ReadCells (StartProp, StartLength / 4);
  End   = ReadCells (EndProp, EndLength / 4);
  if ((End <= Start) || (((End - Start) % RAM_BLOCK_SIZE) != 0)) {
    DEBUG ((DEBUG_ERROR, "SHENGMEM: initrd 0x%lx-0x%lx is not a whole number of sectors\n", Start, End));
    return;
  }

  PublishRamdisk (Start, End);
}

/**
  Test whether an address is described as conventional system memory.

  The GCD is the firmware's own view of what the MMU maps as memory, and every
  range this platform declares reaches it through MemoryMapLib, so this answers
  "can I read this without faulting" without guessing at bank boundaries.

  @param[in]  Address  The address to test.

  @retval TRUE   The address lies in a range the GCD holds as conventional memory.
  @retval FALSE  It does not, or the GCD could not be asked.
**/
STATIC
BOOLEAN
IsMappedSystemMemory (
  IN UINT64  Address
  )
{
  EFI_STATUS            Status;
  UINTN                 NumberOfDescriptors;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  *Descriptor;
  UINTN                 Index;
  BOOLEAN               Found;

  Status = gDS->GetMemorySpaceMap (&NumberOfDescriptors, &Descriptor);
  if (EFI_ERROR (Status) || (Descriptor == NULL)) {
    return FALSE;
  }

  Found = FALSE;
  for (Index = 0; Index < NumberOfDescriptors; Index++) {
    if ((Descriptor[Index].GcdMemoryType == EfiGcdMemoryTypeSystemMemory) &&
        (Address >= Descriptor[Index].BaseAddress) &&
        (Address < (Descriptor[Index].BaseAddress + Descriptor[Index].Length)))
    {
      Found = TRUE;
      break;
    }
  }

  FreePool (Descriptor);
  return Found;
}

/**
  Find the device tree when the address ABL passed is not one.

  ABL hands x0 to SEC verbatim, and on this phone it is not the device tree: the
  recorded value is 0xA59FE000, a hole in the low bank where nothing is mapped. The
  tree itself is in memory all the same - ABL leaves it in the boot image it loaded -
  so the fallback is to look for its header where a bootloader would put it.

  The header is checked by FdtCheckHeader, not by matching a magic number by hand: a
  stray 0xD00DFEED in a splash frame must not be mistaken for the tree.

  Candidates are the two places this board has seen a bootloader place the tree: the
  low bank the firmware is already running in, and the 4 GiB bank where BootShim
  loads the boot image and its ramdisk.

  @param[out] DtbOut  Receives the tree's address when one is found.

  @retval TRUE   A device tree was found.
  @retval FALSE  None of the candidates holds one.
**/
EFI_STATUS
EFIAPI
ShengBootMemoryDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_HOB_GUID_TYPE  *Hob;
  UINT64             Dtb;

  //
  // Publish this driver's result where nothing else writes.
  //
  // The Log Buffer cannot carry it: it is a ring, and by the time the menu can show its tail
  // the dispatcher has logged another twenty drivers, so the one line that says what happened
  // here is always gone. ShengMemSlot is the last 4 KiB of the same mapping, which nothing in
  // the image writes to, so the record survives to be read from the menu.
  //
  // Layout: [0] stage, [1] x0 as the shim passed it, [2] first word of the boot image header,
  // [3] second word, [4] ramdisk start, [5] ramdisk size, [6] status from publishing.
  //
  {
    UINT64  *Slot = (UINT64 *)(UINTN)ShengMemSlot;

    Slot[0] = 1;
    Slot[1] = gSecEntryArg0Shadow;
  }

  BootScreenEvent (89, 0xABCD, 0, BootScreenKindStarted);

  Hob = GetFirstGuidHob (&gSiliciumEntryArgHobGuid);
  if (Hob == NULL) {
    DEBUG ((DEBUG_ERROR, "SHENGMEM: no entry argument HOB\n"));
    LogBufferWriteString ("SHENGMEM: no entry argument HOB\n");
    BootScreenEvent (90, 0x1111, 0, BootScreenKindDone);
    return EFI_SUCCESS;
  }

  Dtb = *(UINT64 *)GET_GUID_HOB_DATA (Hob);
  gSecEntryArg0Shadow = Dtb;
  ((UINT64 *)(UINTN)ShengMemSlot)[1] = Dtb;

  //
  // Publish the payload from the ramdisk's own bytes, not from a device tree.
  //
  // BootShim hands over the address where the kernel region ends. The kernel is the shim
  // followed by the firmware volume in one gzip stream, so after decompression the ramdisk -
  // which the boot image places after the kernel - should begin there.
  //
  // Whether it really does is what this reports. A FAT volume starts with 0x55 0xAA at
  // offset 510 and "FAT" at offset 82, and the payload ramdisk is exactly that, so those bytes
  // decide the question without any further guessing.
  //
  {
    UINT8   *Probe = (UINT8 *)(UINTN)Dtb;
    UINT16  Signature;
    UINT32  FatMark;

    ((UINT64 *)(UINTN)ShengMemSlot)[0] = 2;
    ((UINT64 *)(UINTN)ShengMemSlot)[2] = *(UINT32 *)Probe;
    ((UINT64 *)(UINTN)ShengMemSlot)[3] = *(UINT32 *)(Probe + 4);

    CopyMem (&Signature, Probe + 510, sizeof (Signature));
    CopyMem (&FatMark, Probe + 82, sizeof (FatMark));

    DEBUG ((
      DEBUG_ERROR,
      "SHENGMEM: ramdisk candidate 0x%lx: %08x %08x sig %04x fat %08x\n",
      Dtb,
      *(UINT32 *)Probe,
      *(UINT32 *)(Probe + 4),
      Signature,
      FatMark
      ));

    //
    // "FAT3", not "FAT1". The four bytes at offset 82 are FAT32's filesystem type string, and
    // comparing against the wrong one rejected the volume twice while the panel was showing a
    // perfectly good FAT32 boot sector - first word EB 58 90 4D, signature AA55.
    //
    if ((Signature == 0xAA55u) && (FatMark == 0x33544146u)) {
      //
      // The volume declares its size in the boot sector; trust that over any arithmetic here.
      //
      UINT32  SectorsPerCluster = Probe[13];
      UINT32  ReservedSectors;
      UINT32  NumberOfFats    = Probe[16];
      UINT32  SectorsPerFat;
      UINT32  TotalSectors;
      UINT64  DiskSize;

      CopyMem (&ReservedSectors, Probe + 14, 2);
      CopyMem (&SectorsPerFat, Probe + 36, 4);
      CopyMem (&TotalSectors, Probe + 32, 4);

      DiskSize = (UINT64)TotalSectors * 512;
      if (DiskSize == 0) {
        DiskSize = (UINT64)SectorsPerCluster * 512 * 0;
      }

      ((UINT64 *)(UINTN)ShengMemSlot)[0] = 5;
      ((UINT64 *)(UINTN)ShengMemSlot)[4] = Dtb;
      ((UINT64 *)(UINTN)ShengMemSlot)[5] = DiskSize;

      (VOID)NumberOfFats;
      (VOID)SectorsPerFat;
      (VOID)ReservedSectors;

      PublishRamdisk (Dtb, Dtb + DiskSize);
      return EFI_SUCCESS;
    }

    ((UINT64 *)(UINTN)ShengMemSlot)[0] = 3;
    return EFI_SUCCESS;
  }

  //
  // FdtCheckHeader dereferences the pointer before it can tell us anything, so a pointer the
  // MMU does not map takes the whole boot down with a data abort instead of being rejected.
  // ABL hands x0 to SEC verbatim, and on this phone it is not a device tree at all: the first
  // recorded run faulted at FAR 0xA59FE000 with "Translation fault, second level", which is a
  // hole in the low bank - below it the display buffer starts at 0xA0000000 and above it the
  // next mapped range starts at 0xAC000000, and nothing describes the 137 MiB between them.
  //
  // A "is it inside 4 GiB" test is not enough for that: 0xA59FE000 is inside 4 GiB and still
  // unmapped. Ask the GCD what is actually described, which is where the MMU's view comes
  // from, and require the whole device tree to be conventional memory there.
  //
  if (((Dtb & 3) != 0) || !IsMappedSystemMemory (Dtb)) {
    //
    // Stop here. Searching for the tree was tried twice and both attempts stopped the boot
    // dead, because "the GCD calls it system memory" does not mean "a read will return": the
    // device tree's /memory starts at 0x80000000 and DXE does not have that page mapped (the
    // first line of /proc/iomem on this phone is 0x80880000). An unmapped read is not an
    // exception the firmware can catch, so the search cannot even report that it failed - the
    // screen simply stops, which is worse than doing nothing.
    //
    // Doing nothing is what this driver does on this board for now, and it is not a dead end:
    // with the tree rejected, UniXNU falls back to its built-in configuration, which is enough
    // to reach the menu and to keep testing the rest of the boot. Publishing the payload volume
    // needs the tree's /chosen node, so it needs a real fix for the x0 the bootloader passes,
    // not a guess about where the tree might be.
    //
    DEBUG ((DEBUG_ERROR, "SHENGMEM: no usable device tree, skipping\n"));
    return EFI_SUCCESS;
  }

  if (FdtCheckHeader ((VOID *)(UINTN)Dtb) != 0) {
    DEBUG ((DEBUG_ERROR, "SHENGMEM: x0 0x%lx is not a device tree\n", Dtb));
    return EFI_SUCCESS;
  }

  DEBUG ((DEBUG_ERROR, "SHENGMEM: device tree at 0x%lx, %u bytes\n", Dtb, (UINT32)FdtTotalSize ((VOID *)(UINTN)Dtb)));

  DEBUG ((DEBUG_ERROR, "SHENGMEM: calling AddMemory\n"));
  AddMemory ((VOID *)(UINTN)Dtb);
  DEBUG ((DEBUG_ERROR, "SHENGMEM: AddMemory returned\n"));
  PublishInitrd ((VOID *)(UINTN)Dtb);
  DEBUG ((DEBUG_ERROR, "SHENGMEM: PublishInitrd returned\n"));
  return EFI_SUCCESS;
}

//
// The two protocol objects. They sit at the end of this file because their members are the
// handlers above, and one initialiser there is clearer than a prototype for each.
//
STATIC EFI_FILE_PROTOCOL  mRootTemplate = {
  EFI_FILE_PROTOCOL_REVISION,
  FatOpen,
  FatClose,
  FatDelete,
  FatRead,
  FatWrite,
  FatGetPosition,
  FatSetPosition,
  FatGetInfo,
  FatSetInfo,
  FatFlush,
  NULL,
  NULL,
  NULL,
  NULL,
};

STATIC EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  mSimpleFileSystemTemplate = {
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_REVISION,
  FatOpenVolume
};
