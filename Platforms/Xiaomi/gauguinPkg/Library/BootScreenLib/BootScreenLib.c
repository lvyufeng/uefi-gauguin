/** @file
  Draw the dispatcher's progress onto the panel, for the case where nothing else works.

  Why a library and not a driver: the interesting moment is a driver that never
  returns, and a driver that never returns cannot call anything. So the writer has
  to live in the caller that survives - DxeMain's dispatcher - and it has to work
  without any protocol, any event, any timer and any allocation.

  It draws straight into the framebuffer ABL leaves scanning out. That memory is
  being scanned out by the panel's own hardware until something reprograms the DSI
  host, so a word written there appears whether or not a single DXE protocol exists.

  The screen is a scrolling list of the last rows written. A boot that stops leaves
  the rows of whatever ran just before it visible, which is the answer.

  Copyright (c) 2026, the UniXNU contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BootScreenLib.h>

//
// The buffer ABL leaves scanning out: cont_splash_region,
//   reg = <0x0 0xa0000000 0x0 0x2300000>
// in this device's own device tree. Constants, not a memory-map lookup: the whole
// point is to work when lookups do not.
//
#define PANEL_BASE       0xA0000000ULL
#define PANEL_WIDTH      1080u
#define PANEL_HEIGHT     2400u
#define PANEL_BPP        4u

#define COLOR_BACKGROUND 0x00101010u
#define COLOR_TEXT       0x00FFFFFFu
#define COLOR_DONE       0x0000C040u
#define COLOR_FAULT      0x00FF2020u
#define COLOR_CURRENT    0x0000C0FFu

#define STATUS_TOP       1400u   // where the scrolling list begins
#define ROW_HEIGHT       110u
#define ROW_GAP          14u
#define ROW_LEFT         40u
#define MAX_ROWS         8

#define GLYPH_W          5u
#define GLYPH_H          7u
#define GLYPH_SCALE      9u

STATIC UINT32  mRow;       // rows drawn since the last clear
STATIC UINT32  mTotal;     // rows ever drawn, so the screen can say which is which

//
// Only what a "#NN GUIDAAAA" line needs.
//
typedef struct {
  CHAR8    Character;
  UINT8    Rows[GLYPH_H];
} BOOT_SCREEN_GLYPH;

STATIC CONST BOOT_SCREEN_GLYPH  mGlyphs[] = {
  { '0', { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E } },
  { '1', { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E } },
  { '2', { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F } },
  { '3', { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E } },
  { '4', { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 } },
  { '5', { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E } },
  { '6', { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E } },
  { '7', { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 } },
  { '8', { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E } },
  { '9', { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C } },
  { 'A', { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
  { 'B', { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E } },
  { 'C', { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E } },
  { 'D', { 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E } },
  { 'E', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F } },
  { 'F', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 } },
  { 'G', { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F } },
  { 'H', { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
  { 'I', { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E } },
  { 'J', { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C } },
  { 'K', { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 } },
  { 'L', { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F } },
  { 'M', { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 } },
  { 'N', { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 } },
  { 'O', { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
  { 'P', { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 } },
  { 'Q', { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D } },
  { 'R', { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 } },
  { 'S', { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E } },
  { 'T', { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
  { 'U', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
  { 'V', { 0x11, 0x11, 0x11, 0x11, 0x0A, 0x0A, 0x04 } },
  { 'W', { 0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11 } },
  { 'X', { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 } },
  { 'Y', { 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 } },
  { 'Z', { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F } },
  { '-', { 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 } },
  { '#', { 0x0A, 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x0A } },
  { ' ', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } },
};

STATIC
CONST BOOT_SCREEN_GLYPH *
FindGlyph (
  IN CHAR8  Character
  )
{
  UINTN  Index;

  for (Index = 0; Index < ARRAY_SIZE (mGlyphs); Index++) {
    if (mGlyphs[Index].Character == Character) {
      return &mGlyphs[Index];
    }
  }

  return NULL;
}

STATIC
VOID
FillBand (
  IN UINT32  Top,
  IN UINT32  Height,
  IN UINT32  Colour
  )
{
  UINT32  *Row;
  UINT32   Y;
  UINT32   X;
  UINT32   Last;

  if ((Top >= PANEL_HEIGHT) || (Height == 0)) {
    return;
  }

  Last = Top + Height;
  if (Last > PANEL_HEIGHT) {
    Last = PANEL_HEIGHT;
  }

  for (Y = Top; Y < Last; Y++) {
    Row = (UINT32 *)(UINTN)(PANEL_BASE + (UINT64)Y * PANEL_WIDTH * PANEL_BPP);
    for (X = 0; X < PANEL_WIDTH; X++) {
      Row[X] = Colour;
    }
  }
}

STATIC
VOID
DrawGlyph (
  IN UINT32                        Left,
  IN UINT32                        Top,
  IN CONST BOOT_SCREEN_GLYPH      *Glyph,
  IN UINT32                        Colour
  )
{
  UINT32   Row;
  UINT32   Column;
  UINT32   X;
  UINT32   Y;
  UINT32   Bits;
  UINT32  *Line;

  if (Glyph == NULL) {
    return;
  }

  for (Row = 0; Row < GLYPH_H; Row++) {
    Bits = Glyph->Rows[Row];
    for (Column = 0; Column < GLYPH_W; Column++) {
      if ((Bits & (0x10u >> Column)) == 0) {
        continue;
      }

      for (Y = 0; Y < GLYPH_SCALE; Y++) {
        if ((Top + Row * GLYPH_SCALE + Y) >= PANEL_HEIGHT) {
          return;
        }

        Line = (UINT32 *)(UINTN)(PANEL_BASE + (UINT64)(Top + Row * GLYPH_SCALE + Y) * PANEL_WIDTH * PANEL_BPP);
        for (X = 0; X < GLYPH_SCALE; X++) {
          if ((Left + Column * GLYPH_SCALE + X) < PANEL_WIDTH) {
            Line[Left + Column * GLYPH_SCALE + X] = Colour;
          }
        }
      }
    }
  }
}

STATIC
VOID
DrawText (
  IN UINT32       Left,
  IN UINT32       Top,
  IN CONST CHAR8  *Text,
  IN UINT32       Colour
  )
{
  UINT32  X;
  UINTN   Index;

  if (Text == NULL) {
    return;
  }

  X = Left;
  for (Index = 0; Text[Index] != '\0'; Index++) {
    if ((X + GLYPH_W * GLYPH_SCALE) >= PANEL_WIDTH) {
      return;
    }

    DrawGlyph (X, Top, FindGlyph (Text[Index]), Colour);
    X += (GLYPH_W + 1) * GLYPH_SCALE;
  }
}

/**
  Append a decimal number and return the position after it.
**/
STATIC
UINTN
FormatNumber (
  IN  UINTN  Value,
  OUT CHAR8  *Text,
  IN  UINTN  Position
  )
{
  CHAR8  Digits[20];
  UINTN  Count;
  UINTN  Index;

  Count = 0;
  do {
    Digits[Count++] = (CHAR8)('0' + (Value % 10));
    Value         /= 10;
  } while ((Value != 0) && (Count < sizeof (Digits)));

  for (Index = 0; Index < Count; Index++) {
    Text[Position++] = Digits[Count - 1 - Index];
  }

  return Position;
}

/**
  Append the first four hexadecimal digits of a value, which is enough to tell one
  Qualcomm driver GUID from another at a glance.
**/
STATIC
UINTN
FormatHex (
  IN  UINT32  Value,
  OUT CHAR8   *Text,
  IN  UINTN   Position
  )
{
  UINTN  Index;

  for (Index = 0; Index < 4; Index++) {
    Text[Position++] = "0123456789ABCDEF"[(Value >> (12 - 4 * Index)) & 0xF];
  }

  return Position;
}

/**
  Clear the panel and paint the header.
**/
VOID
EFIAPI
BootScreenClear (
  VOID
  )
{
  FillBand (0, PANEL_HEIGHT, COLOR_BACKGROUND);
  DrawText (ROW_LEFT, 120, (CHAR8 *)"BOOT SCREEN", COLOR_TEXT);
  DrawText (ROW_LEFT, 120 + ROW_HEIGHT, (CHAR8 *)"DISPATCH LOG", COLOR_TEXT);
  mRow   = 0;
  mTotal = 0;
}

/**
  Write one dispatch event.

  The list scrolls: once MAX_ROWS are on the panel, the next event re-renders the
  window of the last MAX_ROWS events it knows about. Re-rendering rather than
  scrolling the pixels keeps this a straight loop with no memory movement, which is
  what an exception path or a hung driver's frame needs.

  @param[in]  Count  Which driver this is, 1-based, in dispatch order.
  @param[in]  Data1  The driver's GUID's first field: enough to identify it.
  @param[in]  Status EFI_SUCCESS when the driver returned, anything else on failure.
  @param[in]  Kind   BootScreenKindStarted or BootScreenKindDone.
**/
VOID
EFIAPI
BootScreenEvent (
  IN UINTN             Count,
  IN UINT32            Data1,
  IN UINTN             Status,
  IN BOOT_SCREEN_KIND  Kind
  )
{
  CHAR8   Line[40];
  UINTN   Length;
  UINT32  Row;
  UINT32  Top;
  UINT32  Colour;

  Length       = 0;
  Line[Length++] = '#';
  Length       = FormatNumber (Count, Line, Length);
  Line[Length++] = ' ';
  Length       = FormatHex (Data1, Line, Length);
  Line[Length++] = ' ';
  if (Kind == BootScreenKindDone) {
    Line[Length++] = (Status == 0) ? (CHAR8)'O' : (CHAR8)'F';
    Line[Length++] = (Status == 0) ? (CHAR8)'K' : (CHAR8)'!';
  } else {
    Line[Length++] = (CHAR8)'.';
    Line[Length++] = (CHAR8)'.';
    Line[Length++] = (CHAR8)'.';
  }

  Line[Length] = '\0';

  Row = mRow;
  if (Row >= MAX_ROWS) {
    //
    // Full: move the window up by one row, then draw at the bottom.
    //
    // The previous version set Row to MAX_ROWS - 1 and left it at that, which does not scroll
    // - it overwrites the last row on every event. On a boot that dispatches ninety drivers
    // and then reports a parked one, every earlier event was erased before it could be read,
    // and the panel ended up showing only the final line. The rows are copied as pixels, four
    // bytes at a time, because there is no second buffer to shuffle text between.
    //
    CopyMem (
      (VOID *)(UINTN)(PANEL_BASE + (UINT64)STATUS_TOP * PANEL_WIDTH * PANEL_BPP),
      (VOID *)(UINTN)(PANEL_BASE + (UINT64)(STATUS_TOP + ROW_HEIGHT + ROW_GAP) * PANEL_WIDTH * PANEL_BPP),
      (UINTN)((MAX_ROWS - 1) * (ROW_HEIGHT + ROW_GAP) * PANEL_WIDTH * PANEL_BPP)
      );

    Row = MAX_ROWS - 1;
  }

  Top    = STATUS_TOP + Row * (ROW_HEIGHT + ROW_GAP);
  Colour = (Kind == BootScreenKindDone) ?
           ((Status == 0) ? COLOR_DONE : COLOR_FAULT) :
           COLOR_CURRENT;

  FillBand (Top, ROW_HEIGHT, COLOR_BACKGROUND);
  FillBand (Top, 8, Colour);
  DrawText (ROW_LEFT, Top + 20, Line, Colour);

  mRow = Row + 1;
  mTotal++;
}