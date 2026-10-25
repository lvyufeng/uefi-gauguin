/** @file
  Draw the dispatcher's progress on the panel without a console, a protocol or an
  allocation.

  Copyright (c) 2026, the UniXNU contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef BOOT_SCREEN_H_
#define BOOT_SCREEN_H_

#include <Uefi.h>

typedef enum {
  BootScreenKindStarted,
  BootScreenKindDone
} BOOT_SCREEN_KIND;

/**
  Clear the panel and paint the header. Safe to call from any context once the
  framebuffer is the one ABL left scanning out.
**/
VOID
EFIAPI
BootScreenClear (
  VOID
  );

/**
  Record one dispatch event on the panel.

  @param[in]  Count   Which driver this is, 1-based, in dispatch order.
  @param[in]  Data1   The driver GUID's first field, for identification.
  @param[in]  Status  EFI_SUCCESS when the driver returned.
  @param[in]  Kind    Started or done.
**/
VOID
EFIAPI
BootScreenEvent (
  IN UINTN             Count,
  IN UINT32            Data1,
  IN UINTN             Status,
  IN BOOT_SCREEN_KIND  Kind
  );

#endif