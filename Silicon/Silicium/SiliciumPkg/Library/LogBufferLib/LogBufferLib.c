/** @file
  Ring buffer in the Log Buffer memory region, so a boot that dies on the panel can still be
  read back afterwards.

  The panel can show about forty lines and scrolls far faster than a camera can follow, so
  every debugging round has meant a video and a manual transcription, with several wrong
  conclusions when a line was misread. The region at 0x9FFF7000 is already reserved as
  EfiRuntimeServicesData and nothing was using it; this writes the interesting events there
  instead, and a recovery image can dump it with a single adb pull.

  Keep this allocation free and interrupt safe: it is called from the exception handler.

  Copyright (c) 2025, the UniXNU contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>

//
// Must match the "Log Buffer" entry in MemoryMapLib.c. If that region moves, this has to move
// with it, or the log lands somewhere the recovery image will not look.
//
#define LOG_BUFFER_BASE   ((UINTN)0x9FFF7000)
#define LOG_BUFFER_SIZE   ((UINTN)0x00008000)

#define LOG_MAGIC         0x474E554C47554C55ULL  // "ULUGNULG" - a fixed tag so the dump tool can tell a real log from noise
#define LOG_VERSION       1

#pragma pack(1)
typedef struct {
  UINT64    Magic;
  UINT32    Version;
  UINT32    Head;      // next byte to write, wraps at Capacity
  UINT32    Total;     // bytes ever written, so a full buffer is still ordered
  UINT32    Capacity;
  CHAR8     Data[0];
} LOG_BUFFER_HEADER;
#pragma pack()

#define LOG_HEADER_SIZE   ((UINT32)(sizeof (LOG_BUFFER_HEADER)))
#define LOG_TEXT_CAPACITY ((UINT32)(LOG_BUFFER_SIZE - LOG_HEADER_SIZE))

STATIC
LOG_BUFFER_HEADER *
LogHeader (
  VOID
  )
{
  LOG_BUFFER_HEADER  *Header;

  Header = (LOG_BUFFER_HEADER *)LOG_BUFFER_BASE;
  if ((Header->Magic != LOG_MAGIC) || (Header->Capacity != LOG_TEXT_CAPACITY)) {
    ZeroMem (Header, LOG_BUFFER_SIZE);
    Header->Version  = LOG_VERSION;
    Header->Head     = 0;
    Header->Total    = 0;
    Header->Capacity = LOG_TEXT_CAPACITY;
    Header->Magic    = LOG_MAGIC;
  }

  return Header;
}

/**
  Append raw bytes to the buffer, wrapping when full.

  @param[in]  Text   Bytes to append.
  @param[in]  Count  How many.
**/
VOID
LogBufferWrite (
  IN CONST CHAR8  *Text,
  IN UINTN        Count
  )
{
  LOG_BUFFER_HEADER  *Header;
  UINT32             Index;

  if ((Text == NULL) || (Count == 0)) {
    return;
  }

  Header = LogHeader ();
  for (Index = 0; Index < Count; Index++) {
    Header->Data[Header->Head] = Text[Index];
    Header->Head              += 1;
    if (Header->Head >= Header->Capacity) {
      Header->Head = 0;
    }

    Header->Total += 1;
  }
}

/**
  Append a NUL-terminated ASCII string.
**/
VOID
LogBufferWriteString (
  IN CONST CHAR8  *Text
  )
{
  if (Text != NULL) {
    LogBufferWrite (Text, AsciiStrLen (Text));
  }
}

/**
  Append one hexadecimal 64-bit value, as 0x + 16 digits.
**/
VOID
LogBufferWriteHex (
  IN UINT64  Value
  )
{
  CHAR8  Buffer[19];
  UINTN  Index;

  Buffer[0] = '0';
  Buffer[1] = 'x';
  for (Index = 0; Index < 16; Index++) {
    Buffer[2 + Index] = "0123456789ABCDEF"[((Value >> (60 - 4 * Index)) & 0xF)];
  }

  LogBufferWrite (Buffer, sizeof (Buffer));
}

/**
  Append one decimal 64-bit value. Small widths only: this is for counts and sizes.
**/
VOID
LogBufferWriteDec (
  IN UINT64  Value
  )
{
  CHAR8  Buffer[21];
  UINTN  Index;
  UINTN  Start;

  Index = sizeof (Buffer);
  Start = Index;
  do {
    Buffer[--Index] = (CHAR8)('0' + (Value % 10));
    Value          /= 10;
  } while (Value != 0);

  LogBufferWrite (&Buffer[Index], Start - Index);
}

/**
  Mark the buffer so the dump tool starts here even if the image was flashed again.
**/
VOID
LogBufferInit (
  VOID
  )
{
  LOG_BUFFER_HEADER  *Header;

  Header = LogHeader ();
  //
  // Route every DebugPrint into the buffer as well; see DebugLib's ConOut hook. The panel keeps
  // showing it, but now a full boot is recoverable without a camera.
  //
  Header->Total = 0;
  Header->Head  = 0;
}