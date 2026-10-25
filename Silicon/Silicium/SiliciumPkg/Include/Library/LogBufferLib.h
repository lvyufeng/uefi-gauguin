/** @file
  Ring buffer in the Log Buffer region, readable from a recovery image after a failed boot.

  Copyright (c) 2025, the UniXNU contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef LOG_BUFFER_LIB_H_
#define LOG_BUFFER_LIB_H_

#include <Uefi.h>

VOID
EFIAPI
LogBufferInit (
  VOID
  );

VOID
EFIAPI
LogBufferWrite (
  IN CONST CHAR8  *Text,
  IN UINTN        Count
  );

VOID
EFIAPI
LogBufferWriteString (
  IN CONST CHAR8  *Text
  );

VOID
EFIAPI
LogBufferWriteHex (
  IN UINT64  Value
  );

VOID
EFIAPI
LogBufferWriteDec (
  IN UINT64  Value
  );

#endif