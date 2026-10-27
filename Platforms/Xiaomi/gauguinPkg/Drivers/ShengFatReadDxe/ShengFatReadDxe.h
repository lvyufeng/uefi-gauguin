/** @file
  A read-only FAT32 filesystem over the ramdisk this platform publishes in memory.

  The firmware already has FatPkg, and on this board it never runs: the disk drivers are
  loaded and never dispatched, so DiskIo and UnicodeCollation2 never appear, Fat's own
  dependency expression never becomes true, and a published ramdisk is left with no filesystem
  on it. UniXNU finds exactly one block device and zero filesystems, falls back to its built-in
  configuration, and offers nothing to boot.

  Rather than keep tuning a dispatcher that is not dispatching, this driver implements the
  little that UniXNU actually asks of a volume: EFI_SIMPLE_FILE_SYSTEM_PROTOCOL with OpenVolume,
  and EFI_FILE_PROTOCOL with Open, Read, GetInfo, GetPosition, SetPosition and Close, read only.
  It parses the FAT32 structures itself, straight out of the memory the ramdisk occupies, and
  needs no block device, no DiskIo and no collation table.

  Copyright (c) 2026, UniXNU contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef SHENG_FAT_READ_H
#define SHENG_FAT_READ_H

#include <Uefi.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/LoadedImage.h>
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

typedef struct {
  UINT8     Sequence;
  UINT8     Name1[10];
  UINT8     Attributes;
  UINT8     Type;
  UINT8     Checksum;
  UINT16    Name2[3];
  UINT16    FirstClusterLow;
  UINT16    Name3[6];
} FAT_LFN_ENTRY;

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

#endif