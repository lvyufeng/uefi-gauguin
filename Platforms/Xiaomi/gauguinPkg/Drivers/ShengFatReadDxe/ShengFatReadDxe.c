/** @file
  A read-only FAT32 filesystem over the ramdisk published in memory.

  See ShengFatReadDxe.h for why this exists instead of FatPkg.

  Copyright (c) 2026, UniXNU contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "ShengFatReadDxe.h"

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/SimpleFileSystem.h>

//
// Every open file is one of these. EFI_FILE_PROTOCOL must be its first member so a pointer to
// the protocol is a pointer to the object.
//
typedef struct {
  EFI_FILE_PROTOCOL    File;
  FAT_VOLUME           *Volume;
  SHENG_FAT_OBJECT     Object;
} FAT_FILE;

STATIC EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  mSimpleFileSystemTemplate;
STATIC BOOLEAN                          mTemplatesReady;
STATIC BOOLEAN                          mTemplatesReadyFs;

STATIC
EFI_STATUS
EFIAPI
FatOpenVolume (
  IN  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *This,
  OUT EFI_FILE_PROTOCOL                **Root
  );

STATIC EFI_FILE_PROTOCOL  mRootTemplate;

STATIC UINT32  mOpenFileCount;

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

  //
  // Skip to the extension: everything after the first dot.
  //
  while ((Name[Index] != 0) && (Name[Index] != '.')) {
    Index++;
  }

  if (Name[Index] == '.') {
    Index++;
    Length = 8;
    while ((Name[Index] != 0) && (Length < 11)) {
      Wanted[Length++] = (CHAR8)Name[Index];
      Index++;
    }
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
// Find one entry in a directory, by name.
//
// Root is addressed by its first cluster; a subdirectory too. Returns the entry's cluster and
// size, and whether it is a directory.
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

  Cluster = DirectoryCluster;
  while (Cluster != FAT_END_OF_CHAIN) {
    Offset = ClusterToOffset (Volume, Cluster);
    if ((UINT64)Offset + Volume->BytesPerCluster > Volume->Size) {
      return EFI_VOLUME_CORRUPTED;
    }

    for (Index = 0; Index < Volume->BytesPerCluster; Index += sizeof (FAT_DIR_ENTRY)) {
      Entry = (FAT_DIR_ENTRY *)(Volume->Base + Offset + Index);

      if (Entry->Name[0] == 0x00) {
        //
        // End of this directory, and of the search.
        //
        return EFI_NOT_FOUND;
      }

      if (Entry->Name[0] == 0xE5) {
        continue;
      }

      if ((Entry->Attributes & FAT_ATTRIBUTE_LFN) == FAT_ATTRIBUTE_LFN) {
        continue;
      }

      if ((Entry->Attributes & FAT_ATTRIBUTE_VOLUME_ID) != 0) {
        continue;
      }

      if (NameMatches (Entry->Name, Name)) {
        CopyMem (Found, Entry, sizeof (*Found));
        return EFI_SUCCESS;
      }
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
  CHAR16         Component[13];
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
  // Anything that moves upwards starts again from the root.
  //
  if (StrStr (FileName, L"..") != NULL) {
    return EFI_NOT_FOUND;
  }

  Status = LookupPath (File->Volume, FileName, &Object);
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
  UINT64    Remaining;
  UINT64    Wanted;
  UINT8     *Out;
  UINT32    Index;

  if ((BufferSize == NULL) || (Buffer == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if ((File->Object.Attributes & FAT_ATTRIBUTE_DIRECTORY) != 0) {
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
  Cluster = File->Object.FirstCluster;
  Skip    = (UINT32)(File->Object.Position / File->Volume->BytesPerCluster);
  while ((Skip != 0) && (Cluster != FAT_END_OF_CHAIN)) {
    Cluster = NextCluster (File->Volume, Cluster);
    Skip--;
  }

  if (Cluster == FAT_END_OF_CHAIN) {
    return EFI_VOLUME_CORRUPTED;
  }

  Offset = (UINT32)(File->Object.Position % File->Volume->BytesPerCluster);
  Out    = (UINT8 *)Buffer;

  for (Index = 0; Index < Wanted; Index++) {
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

    *Out++ = File->Volume->Base[Chunk];
    Offset++;
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
    NameChars = StrLen (File->Object.Path) + 1;
    Needed    = SIZE_OF_EFI_FILE_INFO + NameChars * sizeof (CHAR16);

    if (*BufferSize < Needed) {
      *BufferSize = Needed;
      return EFI_BUFFER_TOO_SMALL;
    }

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
    StrCpyS (Info->FileName, NameChars, File->Object.Path);

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

  if (!mTemplatesReady) {
    EFI_FILE_PROTOCOL  Template = {
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
      NULL, NULL, NULL, NULL,
    };

    CopyMem (&mRootTemplate, &Template, sizeof (mRootTemplate));
    mTemplatesReady = TRUE;
  }

  if (!mTemplatesReadyFs) {
    mSimpleFileSystemTemplate.Revision   = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_REVISION;
    mSimpleFileSystemTemplate.OpenVolume = FatOpenVolume;
    mTemplatesReadyFs = TRUE;
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

/**
  Entry point. The volume cannot be mounted before the ramdisk exists, because where the
  ramdisk is comes from the boot image; ShengBootMemoryDxe calls ShengFatPublish for that.
**/
EFI_STATUS
EFIAPI
ShengFatReadDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  (VOID)ImageHandle;
  (VOID)SystemTable;
  return EFI_SUCCESS;
}
