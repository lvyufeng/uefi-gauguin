#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/MemoryMapHelperLib.h>
#include <Library/SerialPortLib.h>
#include <Library/TimerLib.h>

#include "FrameBuffer.h"
#include "Font.h"

inline
STATIC
UINT8*
DrawPixel (
  IN UINT8  *Pixels,
  IN UINT32  PixelColor,
  IN UINT32  FontScale)
{
  // Repeat Action
  for (UINT8 i = 0; i < FontScale; i++) {
    // Save Pixel Color
    UINT32 Color = PixelColor;

    // Go theu each Byte
    for (UINT8 j = 0; j < FB_BPP; j++) {
      // Update Pixel Color
      *Pixels++ = (UINT8)Color;

      // Shift Pixel Color
      Color >>= 8;
    }
  }

  return Pixels;
}

inline
STATIC
UINT8*
DrawRow (
  IN UINT8  *Pixels,
  IN UINT32  RowData,
  IN UINT32  FontScale,
  IN UINTN   Stride)
{
  // Repeat Action
  for (UINT8 i = 0; i < FontScale; i++) {
    // Save Row Data
    UINT32 Data = RowData;

    // Go thru each Pixel
    for (UINT8 j = 0; j < FONT_WIDTH; j++) {
      // Set Pixel Color
      UINT32 PixelColor = (Data & 1) ? 0xFFFFFFFF : 0x00000000;
      
      // Draw Pixel Color
      Pixels = DrawPixel (Pixels, PixelColor, FontScale);

      // Shift Row
      Data >>= 1;
    }

    // Append Stride
    Pixels += Stride;
  }
  
  return Pixels;
}

VOID
DrawGlyph (
  IN UINT8  *Pixels,
  IN UINT64  Glyph,
  IN UINT32  FontScale)
{
  // Calculate Stride
  UINTN Stride = (FB_WIDTH - (FONT_WIDTH * FontScale)) * FB_BPP;

  // Split Glyph
  UINT32 TopGlyph    = (UINT32)(Glyph >> 32);
  UINT32 BottomGlyph = (UINT32)Glyph;

  // Set Inital Row Data
  UINT32 RowData = TopGlyph;

  // Go thru each Row
  for (UINT8 Row = 0; Row < 12; Row++) {
    // Switch Row Data
    if (Row == 6) {
      RowData = BottomGlyph;
    }

    // Draw Row
    Pixels = DrawRow (Pixels, RowData, FontScale, Stride);

    // Shift to next Row
    RowData >>= FONT_WIDTH;
  }
}

VOID
AdvanceNewLine (
  IN EFI_PHYSICAL_ADDRESS       FbBase,
  IN UINT64                     FbLength,
  IN EFI_FRAME_BUFFER_POSITION *CurrentPosition,
  IN EFI_FRAME_BUFFER_POSITION  MaxPosition)
{
  // Wait
  MicroSecondDelay (FB_DELAY_US);

  // Update Position
  CurrentPosition->XPos  = 0;
  CurrentPosition->YPos += 1;

  // Verify Y Position
  if (CurrentPosition->YPos >= MaxPosition.YPos) {
    // Clear Frame Buffer
    ZeroMem ((VOID *)FbBase, FB_WIDTH * FB_HEIGHT * FB_BPP);

    // Reset X & Y Position
    CurrentPosition->XPos = 0;
    CurrentPosition->YPos = 0;
  }
}

UINT8
GetFontScale ()
{
  // Get Smaller Display Dimension
  UINT32 ShorterDimension = (FB_WIDTH < FB_HEIGHT) ? FB_WIDTH : FB_HEIGHT;

  // Return Calculated Font Scale
  return (ShorterDimension < 426) ? 1 : (UINT8)(ShorterDimension / 426);
}

VOID
GetFbPositions (
  IN  EFI_PHYSICAL_ADDRESS        FbBase,
  IN  UINT64                      FbLength,
  IN  UINT8                       FontScale,
  OUT EFI_FRAME_BUFFER_POSITION **CurrentPosition,
  OUT EFI_FRAME_BUFFER_POSITION  *MaxPosition)
{
  EFI_FRAME_BUFFER_POSITION LocalMaxPosition;

  // Get Current Position
  *CurrentPosition = (EFI_FRAME_BUFFER_POSITION *)(FbBase + FbLength - sizeof (EFI_FRAME_BUFFER_POSITION));

  // Calculate Max Positions
  LocalMaxPosition.XPos = FB_WIDTH  / ((FONT_WIDTH  + 1) * FontScale);
  LocalMaxPosition.YPos = FB_HEIGHT / ((FONT_HEIGHT - 4) * FontScale);

  // Pass Max Position
  *MaxPosition = LocalMaxPosition;
}

VOID
WriteFrameBuffer (
  IN EFI_PHYSICAL_ADDRESS FbBase,
  IN UINT64               FbLength,
  IN UINT8                Character)
{
  EFI_FRAME_BUFFER_POSITION *CurrentPosition;
  EFI_FRAME_BUFFER_POSITION  MaxPosition;
  UINT8                     *Pixels;

  // Set Glyph Font
  STATIC CONST UINT64 GlyphFont[] = GLYPH_FONT;

  // Set Glyph Font Scale
  UINT8 GlyphFontScale = GetFontScale ();

  // Get Frame Buffer Positions
  GetFbPositions (FbBase, FbLength, GlyphFontScale, &CurrentPosition, &MaxPosition);

  // Skip Invalid ACSII Characters
  if (Character >= 127) {
    return;
  }

  // Handle Control Characters
  if (Character < 32) {
    if (Character == '\n') {
      AdvanceNewLine (FbBase, FbLength, CurrentPosition, MaxPosition);
    } else if (Character == '\r') {
      CurrentPosition->XPos = 0;
    }

    return;
  }

  // Skip leading Spaces
  if (CurrentPosition->XPos == 0 && Character == ' ') {
    return;
  }

  // Translate ACSII Character
  UINT64 Glyph = GlyphFont[Character - 32];

  // Calculate Glyph Dimension
  UINTN GlyphWidth  = (FONT_WIDTH  + 1) * GlyphFontScale;
  UINTN GlyphHeight = (FONT_HEIGHT - 4) * GlyphFontScale;

  // Set Glyph Pixels
  Pixels  = (VOID *)FbBase;
  Pixels += (UINTN)CurrentPosition->YPos * GlyphHeight * (FB_WIDTH * FB_BPP);
  Pixels += (UINTN)CurrentPosition->XPos * GlyphWidth  * FB_BPP;

  // Draw Glyph
  DrawGlyph (Pixels, Glyph, GlyphFontScale);

  // Increase X Position
  CurrentPosition->XPos++;

  // Check Max X Position
  if (CurrentPosition->XPos >= MaxPosition.XPos) {
    AdvanceNewLine (FbBase, FbLength, CurrentPosition, MaxPosition);
  }
}

EFI_STATUS
GetFrameBufferMemory (
  OUT EFI_PHYSICAL_ADDRESS *Base,
  OUT UINT64               *Length)
{
  EFI_MEMORY_REGION_DESCRIPTOR FbRegion;

  // Locate "Display Reserved" Memory Region
  LocateMemoryRegionByName ("Display Reserved", &FbRegion);
  LocateMemoryRegionByName ("Display_Reserved", &FbRegion);
  if (!FbRegion.Address && !FbRegion.Length) {
    return EFI_NOT_FOUND;
  }

  // Pass Frame Buffer Memory
  *Base   = FbRegion.Address;
  *Length = FbRegion.Length;

  return EFI_SUCCESS;
}

//
// P2 diagnostic log sink.
//
// The platform maps SerialPortLib to this library, so every DEBUG() in the
// firmware is drawn into the framebuffer and nowhere else. That is fine under
// QEMU, whose console *is* the framebuffer, and useless on the phone: a remote
// host cannot read a screen and a reset destroys it, which is why no device run
// of this payload has ever left a record of why it stopped.
//
// This appends the same bytes to a ring buffer in the reserved "DBI Dump"
// region, near its high end. Layout: three u32 of header (magic, next-write
// offset, wrap count) then the ring. Read it back with `pmemsave` at the
// mirror, or from a bootloader/TWRP that can reach physical RAM, and the last
// boot's log is there.
//
// Why DBI Dump and not the first address that was tried: the sink was originally
// placed at 0xA9C00000, 124 MB into the "Kernel" region, on the reasoning that
// nothing writes there after the firmware stops. That reasoning is wrong for the
// case the sink exists to serve. Reaching RAM on a phone that will not boot means
// rebooting into TWRP, and TWRP loads the Android kernel into exactly that region
// before the log can be read - so the first version would have been destroyed by
// the act of going to look at it. "DBI Dump" (0xAFAA0000 + 0x01400000) is
// declared `RtData` in this platform's own MemoryMapLib, which keeps it out of
// the firmware heap, and 0xAFBFC000 sits above the "Kernel" region TWRP loads
// into. That is the whole of what this address buys.
//
// It is NOT established that the bytes survive the round trip, and two earlier
// drafts of this comment claimed more than that:
//   - "preserved through ResetToUefi" named a mechanism that appears nowhere in
//     this tree. `RtData` is a mark handed to the HOB; nothing here reads it,
//     and nothing here says XBL leaves "DBI Dump" alone. The region is XBL's own
//     ramdump area, so it may equally well be re-initialised on the way back in.
//   - "the top of DRAM" is false. The device's own DT `memory` node has three
//     banks and the top is 0x280000000; 0xAFBFC000 is ~256 MB under the end of
//     the *first* bank. The high end of DBI Dump was picked so that a cold power
//     cycle has the least chance of a carve-out re-allocating the header, and
//     that is a guess, not a property.
// The warm-reset arm cannot be tested at all here: the recovery route the sink
// exists to serve is Power+VolumeUp from fully off. If a real read comes back
// with the wrong magic, suspect this before suspecting the image.
//
// Size and address are deliberately compile-time: SEC and PEI write here before
// any memory-region lookup works, and a sink that only starts working in DXE
// would miss exactly the failures worth reading.
//
#define P2_LOG_SINK_BASE  0xAFBFC000ULL
#define P2_LOG_SINK_SIZE  0x00010000
#define P2_LOG_MAGIC      0x474C3250u  // "P2LG"

STATIC
VOID
P2LogSink (
  IN CONST UINT8 *Buffer,
  IN UINTN        NumberOfBytes)
{
  UINT32 *Header;
  UINT8  *Ring;
  UINTN   RingSize;
  UINT32  Offset;

  if (NumberOfBytes == 0) {
    return;
  }

  Header   = (UINT32 *)P2_LOG_SINK_BASE;
  Ring     = (UINT8 *)(P2_LOG_SINK_BASE + 16);
  RingSize = P2_LOG_SINK_SIZE - 16;

  // Initialise the header on the first write of a power cycle. The magic is the
  // test, so stale RAM is overwritten rather than mistaken for a log.
  if (Header[0] != P2_LOG_MAGIC) {
    Header[0] = P2_LOG_MAGIC;
    Header[1] = 0;
    Header[2] = 0;
    Header[3] = P2_LOG_SINK_SIZE;
  }

  Offset = Header[1];
  for (UINTN i = 0; i < NumberOfBytes; i++) {
    Ring[Offset] = Buffer[i];
    Offset = (Offset + 1) % RingSize;
  }

  Header[1] = Offset;
  Header[2]++;

  WriteBackInvalidateDataCacheRange ((VOID *)P2_LOG_SINK_BASE, P2_LOG_SINK_SIZE);
}

UINTN
EFIAPI
SerialPortWrite (
  IN UINT8 *Buffer,
  IN UINTN  NumberOfBytes)
{
  EFI_STATUS           Status;
  EFI_PHYSICAL_ADDRESS FbBase;
  UINT64               FbLength;

  // Mirror the stream into memory first: this path must not depend on the
  // framebuffer being locatable, because a payload that dies early is exactly
  // the one whose log is wanted.
  P2LogSink (Buffer, NumberOfBytes);

  // Get Frame Buffer Memory
  Status = GetFrameBufferMemory (&FbBase, &FbLength);
  if (EFI_ERROR (Status)) {
    return NumberOfBytes;
  }

  // Get Interrupts State
  BOOLEAN InterruptState = ArmGetInterruptState ();

  // Disable Interrupts
  if (InterruptState) {
    ArmDisableInterrupts ();
  }

  // Write Debug Message to Frame Buffer
  for (UINTN i = 0; i < NumberOfBytes; i++) {
    WriteFrameBuffer (FbBase, FbLength, *Buffer++);
  }

  // Enable Interrupts
  if (InterruptState) {
    ArmEnableInterrupts ();
  }

  // Flush Frame Buffer
  WriteBackInvalidateDataCacheRange ((VOID *)FbBase, FbLength);

  return NumberOfBytes;
}

UINTN
EFIAPI
SerialPortRead (
  OUT UINT8 *Buffer,
  IN  UINTN  NumberOfBytes)
{
  return 0;
}

BOOLEAN
EFIAPI
SerialPortPoll ()
{
  return FALSE;
}

EFI_STATUS
EFIAPI
SerialPortSetControl (IN UINT32 Control)
{
  return EFI_UNSUPPORTED;
}

EFI_STATUS
EFIAPI
SerialPortGetControl (OUT UINT32 *Control)
{
  return EFI_UNSUPPORTED;
}

EFI_STATUS
EFIAPI
SerialPortSetAttributes (
  IN OUT UINT64             *BaudRate,
  IN OUT UINT32             *ReceiveFifoDepth,
  IN OUT UINT32             *Timeout,
  IN OUT EFI_PARITY_TYPE    *Parity,
  IN OUT UINT8              *DataBits,
  IN OUT EFI_STOP_BITS_TYPE *StopBits)
{
  return EFI_UNSUPPORTED;
}

EFI_STATUS
EFIAPI
SerialPortInitialize ()
{
  return EFI_SUCCESS;
}
