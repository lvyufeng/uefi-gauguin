/**
  Bitra (SM7225) ACPI table update library.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/AcpiTableUpdateLib.h>

/**
  Patch board-specific values into the DSDT before it is handed to the OS.

  P3: this is a deliberate no-op. The sibling SoC packages fill in the modem,
  ADSP and TZ shared-memory regions by reading them from SMEM and writing them
  into named DSDT fields, and they do it against a DSDT that was written for
  their board. There is no DSDT for gauguin yet, so there are no fields to
  write to - and inventing values with no table to put them in would only
  produce a build that looks finished and an OS that boots to a stop code.

  When P3 adds the ACPI tables, this is where the SMEM-derived regions go.
**/
VOID
UpdateAcpiTables ()
{
  return;
}
