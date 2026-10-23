/**
  Copyright (c) 2011-2012, ARM Limited. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/IoLib.h>
#include <Library/PlatformSecLib.h>
#include <Library/MemoryMapHelperLib.h>
#include <Library/ArmSmmuDetachLib.h>

#include "PlatformRegisters.h"

VOID
DisableWatchDogTimer ()
{
  EFI_STATUS                   Status;
  EFI_MEMORY_REGION_DESCRIPTOR WDogRegion;

  // Locate "APSS_WDT_TMR1" Memory Region
  Status = LocateMemoryRegionByName ("APSS_WDT_TMR1", &WDogRegion);
  if (EFI_ERROR (Status)) {
    return;
  }

  // Disable WatchDog Timer
  MmioWrite32 (WDogRegion.Address + APSS_WDT_ENABLE_OFFSET, 0x0);
}

VOID
PlatformInitialize ()
{
  // Disable WatchDog Timer
  DisableWatchDogTimer ();

  // Set MDP SIDs
  //
  // Only 0x800 is confirmed for this SoC: it is the SID the mainline SM6350
  // device tree gives the display subsystem. The rest of the list is inherited
  // from MooreaPkg (SM7150), which is the same generation of display block
  // (and whose 0x800 - 0xC41 range contains the one value we can confirm).
  // These are the rotation and VBIF stream IDs, which the device tree does not
  // describe because Linux's DRM driver attaches them itself.
  //
  // P5: confirm the full set against Qualcomm's own DisplayDxe before display
  // is brought up. Detaching a SID that is not in use is harmless, so the risk
  // here is a missing one, not an extra.
  CONST UINT16 MdpStreams[] = { 0x800, 0x801, 0x840, 0x841, 0xC00, 0xC01, 0xC40, 0xC41 };

  // Detach IOMMU Domains
  ArmSmmuDetach (MdpStreams, ARRAY_SIZE (MdpStreams));
}
