/** @file
  Copy the firmware's log rings from RAM into the phone's logdump partition.

  Why this exists: the panel is this firmware's only live output, and a phone
  photographed by hand is a poor way to read a boot failure - the screen holds a
  handful of rows, it scrolls them away, and a run that stops early leaves
  nothing behind. The RAM rings hold the whole stream, but nothing can read them
  afterwards: TWRP's kernel has no CONFIG_DEVMEM (a mknod'd /dev/mem returns
  ENXIO), and by the time a recovery shell answers, XBL and ABL have run again
  over the region the rings live in.

  Flash is the one channel that survives that. `logdump` is the phone's own
  ramdump area, 0x04000000 bytes, and reads back as all zero on this device, so
  its first 256 KiB become a log that TWRP reads with one dd:

      dd if=/dev/block/by-name/logdump bs=1M count=1 | strings

  The rings cannot write here themselves, and the driver cannot read them from
  SEC: a SerialPortLib is BASE-class and runs before block devices exist. This
  driver runs in DXE, where the UFS driver has published EFI_BLOCK_IO_PROTOCOL
  for the logdump LUN, and copies the rings out on load and then on a timer - a
  timer rather than a hook, because a firmware that hangs is exactly the case
  worth reading and a hung driver never calls anything back.

  Two streams are copied, because they answer different questions:

    CONSOLE  DebugPrint, as FrameBufferSerialPortLib's P2LG ring. Every driver's
             own DEBUG output, in order.
    EVENTS   DxeMain's dispatcher and DefaultExceptionHandlerLib, as
             LogBufferLib's ring. ">> start #N <guid>" before a driver and
             "<< done <status>" after it, plus the fault registers. This is the
             stream that names the driver the boot died in, and it is written
             even when the console itself is the thing that is broken.

  Layout at offset 0 of the partition, little endian:

    0x000  magic   0x474F4C50 ("PLOG")
    0x004  version 2
    0x008  seq     how many copies were written this boot
    0x00C  total   bytes of index and records that follow
    0x010  records, each PLOG_RECORD { kind, base, length, reserved, text[] }

  Version 2 replaced a single header plus flat text: the block is always written
  as a whole 256 KiB, zero-padded, so a reader could not separate the current
  log from the stale tail of an earlier one. Each record now states its length.

  Copyright (c) 2026, the UniXNU contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/BlockIo.h>

#define PLOG_MAGIC        0x474F4C50u   // "PLOG"
#define PLOG_RING_MAGIC   0x474C3250u   // "P2LG", written by FrameBufferSerialPortLib
#define PLOG_LB_MAGIC     0x474E554C47554C55ULL  // "ULUGNULG", written by LogBufferLib
#define PLOG_HEADER_SIZE  0x10u
#define PLOG_TEXT_SIZE    0x0003FFF0u   // 255 KiB of text under a 256 KiB ceiling

//
// A PLOG v2 image is a small index followed by the two streams, each carried as its own
// self-describing record. Version 1 was a single 0x10-byte header and the P2LG ring's text
// in flat form, and the reader could not tell how much of the 255 KiB behind it was real:
// the block is zero-filled to a whole 256 KiB block write, so a stale sector from an earlier
// boot and a short current log look the same. Every record now states its own length.
//
//   0x000  magic    0x474F4C50 ("PLOG")
//   0x004  version  2
//   0x008  seq      how many copies were written this boot
//   0x00C  total    bytes of index + records that follow
//   0x010  records  PLOG_RECORD[], in the order below
//
#define PLOG_VERSION      2u

#pragma pack(1)

//
// One stream. Named by the address it was read from, so a dump says which region it came
// from instead of leaving the reader to match an offset against a source tree.
//
typedef struct {
  UINT32    Kind;       // PLOG_STREAM_*
  UINT32    Base;       // address the stream lives at, informational
  UINT32    Length;     // bytes of text that follow this record
  UINT32    Reserved;
  UINT32    Text[1];    // Length bytes at least, then padded to 4
} PLOG_RECORD;

#pragma pack()

#define PLOG_STREAM_CONSOLE  1u   // DebugPrint stream, FrameBufferSerialPortLib's P2LG ring
#define PLOG_STREAM_EVENTS   2u   // Driver dispatch and exception events, LogBufferLib's ring

//
// The rings' addresses, as their writers define them. Written as constants rather than as PCDs:
// this driver must not be able to fail on a missing token when the thing it is supposed to do
// is make other failures readable, and a PCD lookup that asserts does exactly that.
//
#define PLOG_RING_BASE    0xAFBFC000ULL   // P2_LOG_SINK_BASE in FrameBufferSerialPortLib
#define PLOG_EVENTS_BASE  0x9FFF7000ULL   // LOG_BUFFER_BASE in LogBufferLib

//
// The index is at most five words; everything after it is records, each 0x10 plus its text.
//
#define PLOG_MAX_TEXT     (PLOG_TEXT_SIZE - 5 * sizeof (UINT32))

//
// LogBufferLib's header: magic, version, head, total, capacity, then the text.
//
#define PLOG_EVENTS_HEADER  0x14u

STATIC EFI_EVENT  mTimer;
STATIC UINT32     mSeq;
STATIC UINT8     *mSector;
STATIC UINT32     mLastLength;

/**
  Return the block device whose device path names the logdump partition.

  The UFS driver publishes one EFI_BLOCK_IO_PROTOCOL per physical LUN and each
  device path renders the LUN's name, so the partition is found by matching that
  text rather than by a hard-coded handle or LUN number.

  @param[out] BlockIo  The logdump block device. Not modified on failure.

  @retval EFI_SUCCESS    A device naming logdump was found.
  @retval EFI_NOT_FOUND  No block device carries that name.
**/
STATIC
EFI_STATUS
FindLogdump (
  OUT EFI_BLOCK_IO_PROTOCOL  **BlockIo
  )
{
  EFI_STATUS                Status;
  UINTN                     HandleCount;
  EFI_HANDLE               *Handles;
  UINTN                     Index;
  EFI_BLOCK_IO_PROTOCOL    *Candidate;
  EFI_BLOCK_IO_PROTOCOL    *Fallback;
  EFI_DEVICE_PATH_PROTOCOL *DevicePath;
  CHAR16                   *Text;

  Handles  = NULL;
  Fallback = NULL;
  Status   = gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &HandleCount, &Handles);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (Index = 0; Index < HandleCount; Index++) {
    Status = gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&Candidate);
    if (EFI_ERROR (Status) || (Candidate->Media == NULL) || !Candidate->Media->MediaPresent) {
      continue;
    }

    Status = gBS->HandleProtocol (Handles[Index], &gEfiDevicePathProtocolGuid, (VOID **)&DevicePath);
    if (EFI_ERROR (Status)) {
      continue;
    }

    Text = ConvertDevicePathToText (DevicePath, FALSE, FALSE);
    if (Text == NULL) {
      continue;
    }

    if (StrStr (Text, L"logdump") != NULL) {
      *BlockIo = Candidate;
      FreePool (Text);
      FreePool (Handles);
      return EFI_SUCCESS;
    }

    // Remember the largest device seen: with no name match, that is the best guess at the data LUN.
    if ((Fallback == NULL) || (Candidate->Media->LastBlock > Fallback->Media->LastBlock)) {
      Fallback = Candidate;
    }

    FreePool (Text);
  }

  FreePool (Handles);
  if (Fallback != NULL) {
    *BlockIo = Fallback;
    return EFI_SUCCESS;
  }

  return EFI_NOT_FOUND;
}

/**
  Unwrap one ring into the output buffer and append it as a record.

  Both rings store a rotating buffer plus a byte count, and neither has room for the answer
  "how much of this is stale". The count is kept for the same reason: a ring that wrapped
  leaves exactly Capacity bytes of order in the buffer, oldest first from the head, and a ring
  that did not wrapped leaves Head bytes starting at 0. LogBufferLib stores that byte count as
  Total and the position as Head; P2LG stores the position in word 1 and the count in word 2.

  @param[in]     Kind    PLOG_STREAM_* for the record.
  @param[in]     Base    Address the ring lives at, copied into the record.
  @param[in]     Head    Next byte to write.
  @param[in]     Total   Bytes ever written.
  @param[in]     Text    The ring's text area.
  @param[in]     Capacity Size of that area.
  @param[in,out] Offset  Where the record goes; advanced past it.

  @return TRUE when a record was written.
**/
STATIC
BOOLEAN
AppendRecord (
  IN     UINT32  Kind,
  IN     UINT32  Base,
  IN     UINT32  Head,
  IN     UINT32  Total,
  IN     UINT8   *Text,
  IN     UINT32  Capacity,
  IN OUT UINT32  *Offset
  )
{
  PLOG_RECORD  *Record;
  UINT32        Length;
  UINT32        First;

  if ((Total == 0) || (Capacity == 0) || (Head >= Capacity)) {
    return FALSE;
  }

  Length = (Total < Capacity) ? Head : Capacity;
  if (Length == 0) {
    return FALSE;
  }

  if ((Length + sizeof (PLOG_RECORD)) > (PLOG_MAX_TEXT - *Offset)) {
    Length = PLOG_MAX_TEXT - *Offset - sizeof (PLOG_RECORD);
    if ((INT32)Length <= 0) {
      return FALSE;
    }
  }

  Record           = (PLOG_RECORD *)(mSector + PLOG_HEADER_SIZE + *Offset);
  Record->Kind     = Kind;
  Record->Base     = Base;
  Record->Length   = Length;
  Record->Reserved = 0;

  //
  // A ring that wrapped is ordered from Head, the oldest byte; one that did not starts at 0.
  //
  if (Total < Capacity) {
    CopyMem (Record->Text, Text, Length);
  } else {
    First = Capacity - Head;
    if (First > Length) {
      First = Length;
    }

    CopyMem ((UINT8 *)Record->Text, Text + Head, First);
    if (Length > First) {
      CopyMem ((UINT8 *)Record->Text + First, Text, Length - First);
    }
  }

  // The text is a byte count, not a word count; keep the next record 4-byte aligned.
  *Offset += sizeof (PLOG_RECORD) + ((Length + 3) & ~3u);
  return TRUE;
}

/**
  Write the log rings, if the firmware left any, to the logdump partition.

  Best effort by design: every failure is silent, because this runs from a timer while the
  firmware may be in the middle of failing, and a log writer that asserts or blocks turns a
  readable hang into an unreadable one.
**/
STATIC
VOID
FlushLog (
  VOID
  )
{
  EFI_STATUS             Status;
  EFI_BLOCK_IO_PROTOCOL *BlockIo;
  UINT32                *Header;
  UINT64                 RingBase;
  UINT32                 Offset;
  BOOLEAN                Any;

  if (mSector == NULL) {
    return;
  }

  if (EFI_ERROR (FindLogdump (&BlockIo))) {
    return;
  }

  Offset = 0;
  Any    = FALSE;

  //
  // The DebugPrint stream. Its ring is 0xAFBFC000, which no memory-map entry names: on this
  // board that range is ordinary DRAM that DXE may hand out, so the header is read without
  // trusting it, and a header it did not write is skipped rather than believed.
  //
  RingBase = PLOG_RING_BASE;
  Header   = (UINT32 *)(UINTN)RingBase;
  if ((Header[0] == PLOG_RING_MAGIC) && (Header[3] == 0x00010000u)) {
    Any |= AppendRecord (
             PLOG_STREAM_CONSOLE,
             (UINT32)RingBase,
             Header[1],
             Header[2],
             (UINT8 *)(UINTN)(RingBase + 16),
             0x00010000u - 16,
             &Offset
             );
  }

  //
  // The dispatch and exception events. This is the stream that answers "which driver killed
  // the boot": DxeMain writes ">> start #N <guid>" before each driver and "<< done <status>"
  // after it, and DefaultExceptionHandlerLib writes the fault. Whatever is last in here when
  // the boot dies is the culprit.
  //
  RingBase = PLOG_EVENTS_BASE;
  Header   = (UINT32 *)(UINTN)RingBase;
  if (*((UINT64 *)(UINTN)RingBase) == PLOG_LB_MAGIC) {
    Any |= AppendRecord (
             PLOG_STREAM_EVENTS,
             (UINT32)RingBase,
             Header[2],
             Header[3],
             (UINT8 *)(UINTN)(RingBase + PLOG_EVENTS_HEADER),
             Header[4],
             &Offset
             );
  }

  if (!Any) {
    return;
  }

  // Nothing changed since the last copy: the sector already holds it, and rewriting turns a
  // hang in the middle of the write into a lost log instead of a stale-but-complete one.
  if ((Offset == mLastLength) && (mSeq != 0)) {
    return;
  }

  ZeroMem (mSector, PLOG_HEADER_SIZE + PLOG_TEXT_SIZE);
  *(UINT32 *)(mSector + 0x00) = PLOG_MAGIC;
  *(UINT32 *)(mSector + 0x04) = PLOG_VERSION;
  *(UINT32 *)(mSector + 0x08) = ++mSeq;
  *(UINT32 *)(mSector + 0x0C) = Offset;

  Status = BlockIo->WriteBlocks (
                      BlockIo,
                      BlockIo->Media->MediaId,
                      0,
                      PLOG_HEADER_SIZE + PLOG_TEXT_SIZE,
                      mSector
                      );
  if (!EFI_ERROR (Status)) {
    mLastLength = Offset;
  }
}

STATIC
VOID
EFIAPI
PlogTimer (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  FlushLog ();
}

/**
  Start copying the log ring into logdump.

  @param[in] ImageHandle  The image handle.
  @param[in] SystemTable  The system table.

  @retval EFI_SUCCESS  The log writer is running.
**/
EFI_STATUS
EFIAPI
PlogEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  mSector = AllocateZeroPool (PLOG_HEADER_SIZE + PLOG_TEXT_SIZE);
  if (mSector == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  // Once on load, then every two seconds; the first call is what makes a boot
  // that stops right here still readable.
  FlushLog ();

  // 250 ms, not seconds: the timer is the only thing that keeps copying while some other driver
  // blocks forever, and the copy itself is a 256 KiB block write that costs little.
  if (!EFI_ERROR (gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK, PlogTimer, NULL, &mTimer))) {
    gBS->SetTimer (mTimer, TimerPeriodic, 250 * 1000);
  }

  return EFI_SUCCESS;
}