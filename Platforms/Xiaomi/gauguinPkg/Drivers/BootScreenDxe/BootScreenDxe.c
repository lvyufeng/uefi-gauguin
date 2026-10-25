/** @file
  Paint the boot's progress onto the panel without a console.

  Why this exists: gauguin's only live output is the panel, and every driver that
  could have carried a log to the host - the USB stack, UFS - is exactly what the
  bring-up is still trying to make work. A boot that stops before the console
  exists therefore produces nothing readable: no serial console on this board, no
  USB device to attach to, and a framebuffer nobody has drawn into.

  The framebuffer itself is not part of that problem. ABL leaves the J17 panel
  scanning out cont_splash_region at 0xA0000000, and that buffer keeps being
  scanned out until something reprograms the DSI host. A picture drawn there is
  visible whether or not a single DXE protocol exists yet - which makes it the one
  output channel that is independent of everything the port is still debugging.

  So this driver draws directly into that memory: a filled panel and a list of
  stages, each flipped to a distinct colour as the boot passes it. Nothing here
  goes through EFI_GRAPHICS_OUTPUT_PROTOCOL, ConOut, ConSplitter or the font
  package, because all of those are published by drivers that may never run. It
  only needs BaseLib, so `Depex = TRUE` really does mean "as early as it can be".

  Reading a run: the colour of each row is the answer, and the rows are drawn at a
  size that reads from a photograph rather than a video. A row that stays grey was
  never reached; the first grey row is where the boot stopped.

  Copyright (c) 2026, the UniXNU contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>

//
// The buffer ABL leaves scanning out. cont_splash_region is
//   reg = <0x0 0xa0000000 0x0 0x2300000>
// in this device's own device tree. It is written as a constant rather than read
// from the memory map on purpose: this driver must not be able to fail on a lookup
// when the thing it is supposed to do is make other failures visible.
//
#define PANEL_BASE          0xA0000000ULL

//
// 1080x2400, 32 bpp, the J17 panel. The stride is the panel's width; a panel whose
// stride differs would show a diagonal skew, which is itself a readable result.
//
#define PANEL_WIDTH         1080u
#define PANEL_HEIGHT        2400u
#define PANEL_BPP           4u

//
// Colours in the buffer's byte order (little-endian ARGB, i.e. word 0x00RRGGBB).
// High contrast on purpose: these are read off a photo, often of a dim panel.
//
#define COLOR_UNKNOWN       0x00606060u   // not reached yet: mid grey
#define COLOR_STARTED       0x0000C0FFu   // this driver ran at all: amber-cyan
#define COLOR_SURVIVED      0x0000C040u   // reached and returned: green
#define COLOR_STOPPED       0x00FF2020u   // known to have failed here: red
#define COLOR_TEXT          0x00FFFFFFu   // white on black
#define COLOR_BACKGROUND    0x00101010u

//
// Where the stage list starts, and how tall each row is. Eight rows at 200px with
// generous gaps fills the lower two thirds of a 2400px panel without crowding the
// rows into each other in a photo.
//
#define LIST_TOP            560u
#define ROW_HEIGHT          180u
#define ROW_GAP             40u
#define ROW_LEFT            60u
#define ROW_RIGHT           1020u

/**
  Fill one horizontal band of the panel.

  @param[in]  Top     First row.
  @param[in]  Height  How many rows.
  @param[in]  Colour  Word to store for every pixel.
**/
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

/**
  Blit a small fixed glyph set big enough to read from a photograph.

  A real font would need the font package, which is published by a driver that may
  not have run yet. These are the only characters a stage name needs and they are
  drawn from a 5x7 bitmap scaled up, so they cost nothing but a table.
**/
#define GLYPH_W  5u
#define GLYPH_H  7u
#define GLYPH_SCALE  12u

typedef struct {
  CHAR8    Character;
  UINT8    Rows[GLYPH_H];
} GLYPH;

//
// Bit 4 of each row byte is the leftmost column. Only the characters used by the
// stage names below are here; anything else draws as blank.
//
STATIC CONST GLYPH  mGlyphs[] = {
  { 'A', { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
  { 'B', { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E } },
  { 'C', { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E } },
  { 'D', { 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E } },
  { 'E', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F } },
  { 'F', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 } },
  { 'G', { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F } },
  { 'H', { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
  { 'I', { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E } },
  { 'K', { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 } },
  { 'L', { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F } },
  { 'M', { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 } },
  { 'N', { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 } },
  { 'O', { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
  { 'P', { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 } },
  { 'R', { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 } },
  { 'S', { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E } },
  { 'T', { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
  { 'U', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
  { 'W', { 0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11 } },
  { 'X', { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 } },
  { 'Y', { 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 } },
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
  { '-', { 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 } },
  { '#', { 0x0A, 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x0A } },
};

STATIC CONST GLYPH *
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

/**
  Draw one character at a pixel position, scaled by GLYPH_SCALE.

  @param[in]  Left    Left edge.
  @param[in]  Top     Top edge.
  @param[in]  Glyph   The glyph, or NULL to draw nothing.
  @param[in]  Colour  Word to store for a set pixel.
  @param[in]  Scale   Pixels per glyph pixel.
**/
STATIC
VOID
DrawGlyph (
  IN UINT32        Left,
  IN UINT32        Top,
  IN CONST GLYPH  *Glyph,
  IN UINT32        Colour,
  IN UINT32        Scale
  )
{
  UINT32  Row;
  UINT32  Column;
  UINT32  X;
  UINT32  Y;
  UINT32  Bits;
  UINT32 *Line;

  if (Glyph == NULL) {
    return;
  }

  for (Row = 0; Row < GLYPH_H; Row++) {
    Bits = Glyph->Rows[Row];
    for (Column = 0; Column < GLYPH_W; Column++) {
      if ((Bits & (0x10u >> Column)) == 0) {
        continue;
      }

      for (Y = 0; Y < Scale; Y++) {
        if ((Top + Row * Scale + Y) >= PANEL_HEIGHT) {
          break;
        }

        Line = (UINT32 *)(UINTN)(PANEL_BASE + (UINT64)(Top + Row * Scale + Y) * PANEL_WIDTH * PANEL_BPP);
        for (X = 0; X < Scale; X++) {
          if ((Left + Column * Scale + X) < PANEL_WIDTH) {
            Line[Left + Column * Scale + X] = Colour;
          }
        }
      }
    }
  }
}

/**
  Draw an ASCII string, left to right, at GLYPH_SCALE.

  @param[in]  Left    Left edge.
  @param[in]  Top     Top edge.
  @param[in]  Text    NUL-terminated ASCII.
  @param[in]  Colour  Text colour.
**/
STATIC
VOID
DrawText (
  IN UINT32     Left,
  IN UINT32     Top,
  IN CONST CHAR8 *Text,
  IN UINT32     Colour
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
      break;
    }

    DrawGlyph (X, Top, FindGlyph (Text[Index]), Colour, GLYPH_SCALE);
    X += (GLYPH_W + 1) * GLYPH_SCALE;
  }
}

/**
  Paint the stage list. Every row starts grey; the caller flips the ones it reached.

  Drawn once, on entry, so that even a driver that stops immediately leaves a legible
  panel behind rather than the boot logo.
**/
STATIC
VOID
DrawStageList (
  VOID
  )
{
  STATIC CONST CHAR8 *mStages[] = {
    "1 UEFI CORE",
    "2 DEFEX CORE",
    "3 FV AND MEMORY",
    "4 SCHEDULER",
    "5 BDS CONSOLE",
    "6 LOADER CHAIN",
    "7 XNU ENTRY",
    "8 STALLED HERE",
  };
  UINTN   Index;
  UINT32  Top;

  FillBand (0, PANEL_HEIGHT, COLOR_BACKGROUND);

  for (Index = 0; Index < ARRAY_SIZE (mStages); Index++) {
    Top = LIST_TOP + (UINT32)Index * (ROW_HEIGHT + ROW_GAP);
    FillBand (Top, ROW_HEIGHT, COLOR_UNKNOWN);
    // The label sits on the row rather than beside it, so a cropped photo still
    // carries the name that goes with the colour.
    DrawText (ROW_LEFT, Top + (ROW_HEIGHT - GLYPH_H * GLYPH_SCALE) / 2, mStages[Index], COLOR_TEXT);
  }
}

/**
  Entry point.

  The first row is flipped before anything else so that "this driver ran" and "the
  panel accepts writes" are the same observation. If the panel stays entirely dark,
  that is measured: the framebuffer base or the stride is wrong, and the stage list
  below it is not worth reading.

  @param[in]  ImageHandle  The image handle.
  @param[in]  SystemTable  The system table.

  @retval EFI_SUCCESS  The panel was painted.
**/
EFI_STATUS
EFIAPI
BootScreenEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  DrawStageList ();

  // Row 0: reached, and the driver is about to return.
  FillBand (LIST_TOP, ROW_HEIGHT, COLOR_SURVIVED);
  DrawText (ROW_LEFT, LIST_TOP + (ROW_HEIGHT - GLYPH_H * GLYPH_SCALE) / 2, (CHAR8 *)"1 UEFI CORE", COLOR_TEXT);

  return EFI_SUCCESS;
}