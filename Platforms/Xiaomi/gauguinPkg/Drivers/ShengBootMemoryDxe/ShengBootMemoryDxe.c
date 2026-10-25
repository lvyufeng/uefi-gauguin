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
#include <Library/HobLib.h>
#include <Library/BootScreenLib.h>
#include <Library/LogBufferLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>

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

  Hob = GetFirstGuidHob (&gSiliciumEntryArgHobGuid);
  if (Hob == NULL) {
    DEBUG ((DEBUG_ERROR, "SHENGMEM: no entry argument HOB\n"));
    LogBufferWriteString ("SHENGMEM: no entry argument HOB\n");
    BootScreenEvent (90, 0x1111, 0, BootScreenKindDone);
    return EFI_SUCCESS;
  }

  Dtb = *(UINT64 *)GET_GUID_HOB_DATA (Hob);

  //
  // Publish the payload from the boot image header, not from a device tree.
  //
  // BootShim replaces ABL's x0 with the address it was itself loaded at, so this is the start
  // of the boot image. Its header declares the kernel size, the ramdisk size and the page
  // size; the ramdisk sits one page past the header plus enough pages for the kernel. Every
  // number comes from the header the packer wrote, so nothing here is guessed.
  //
  // This path exists because the device tree is out of reach on this board. ABL points at
  // 0xA59FE000 and the boot image sits at 0xA2408000; both were unreadable for the same
  // reason - neither address had a descriptor, so the MMU never mapped it, and a read faulted
  // instead of returning. The map now describes the first 15 MiB of that gap, which is what
  // makes the header readable at all.
  //
  {
    UINT32  *Header;
    UINT32  KernelSize;
    UINT32  RamdiskSize;
    UINT32  PageSize;
    UINT64  RamdiskStart;

    Header = (UINT32 *)(UINTN)Dtb;

    DEBUG ((DEBUG_ERROR, "SHENGMEM: boot image at 0x%lx\n", Dtb));

    if ((Header[0] != 0x4F444E41u) || (Header[1] != 0x214449u)) {
      DEBUG ((
        DEBUG_ERROR,
        "SHENGMEM:   not an android header (%08x %08x), nothing to publish\n",
        Header[0],
        Header[1]
        ));
      LogBufferWriteString ("SHENGMEM: NOT-ANDROID ");
      LogBufferWriteHex (Header[0]);
      LogBufferWriteString (" ");
      LogBufferWriteHex (Header[1]);
      LogBufferWriteString ("\n");
      //
      // Tell the panel too. The console scrolls these lines away long before the menu is
      // drawn, and the menu saying "0 block device(s), 0 filesystem(s)" does not say which of
      // the exits below was taken. The strip survives the clear.
      //
      BootScreenEvent (91, Header[0], Header[1], BootScreenKindDone);
      return EFI_SUCCESS;
    }

    KernelSize  = Header[2];
    RamdiskSize = Header[4];
    PageSize    = Header[9];

    if ((PageSize < 0x200) || ((PageSize & (PageSize - 1)) != 0) || (RamdiskSize == 0)) {
      DEBUG ((
        DEBUG_ERROR,
        "SHENGMEM:   header says page %u ramdisk %u, refusing\n",
        PageSize,
        RamdiskSize
        ));
      LogBufferWriteString ("SHENGMEM: BAD-SIZES ");
      LogBufferWriteDec (PageSize);
      LogBufferWriteString (" ");
      LogBufferWriteDec (RamdiskSize);
      LogBufferWriteString ("\n");
      BootScreenEvent (92, PageSize, RamdiskSize, BootScreenKindDone);
      return EFI_SUCCESS;
    }

    RamdiskStart = Dtb + (UINT64)PageSize * (1 + ((UINT64)KernelSize + PageSize - 1) / PageSize);

    DEBUG ((
      DEBUG_ERROR,
      "SHENGMEM:   kernel %u ramdisk %u page %u -> ramdisk at 0x%lx\n",
      KernelSize,
      RamdiskSize,
      PageSize,
      RamdiskStart
      ));

    LogBufferWriteString ("SHENGMEM: OK ramdisk at ");
    LogBufferWriteHex (RamdiskStart);
    LogBufferWriteString (" size ");
    LogBufferWriteDec (RamdiskSize);
    LogBufferWriteString ("\n");

    PublishRamdisk (RamdiskStart, RamdiskStart + RamdiskSize);
    BootScreenEvent (93, (UINT32)RamdiskStart, RamdiskStart + RamdiskSize, BootScreenKindDone);
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
