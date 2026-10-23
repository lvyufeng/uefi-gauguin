##
#  Bitra platform (Snapdragon 750G / SM7225) - generated for gauguin.
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

[Defines]
  #
  # Timer
  #
  USE_PHYSICAL_TIMER                         = 1

  #
  # KEP
  #
  HAS_ACTLR_EL1_UNIMPLEMENTED_ERRATA         = 0
  HAS_AMCNTENSET0_EL0_UNIMPLEMENTED_ERRATA   = 0
  HAS_GIC_V3_WITHOUT_IRM_FLAG_SUPPORT_ERRATA = 0
  HAS_PSCI_MEMPROTECT_FAILING_ERRATA         = 1

!include QcomPkg/QcomPkg.dsc.inc

[BuildOptions]
  #
  # Cortex-A77 + Cortex-A55. The Kryo 570 cores implement ARMv8.2-A.
  #
  *_*_AARCH64_CC_FLAGS = -march=armv8.2-a+crypto+rcpc

[PcdsFixedAtBuild]
  #
  # ACPI
  #
  gEfiMdeModulePkgTokenSpaceGuid.PcdAcpiDefaultOemTableId|0x202041544942
  gEfiMdeModulePkgTokenSpaceGuid.PcdAcpiDefaultOemRevision|0x7225

  #
  # CPU - 2x A77 + 6x A55
  #
  gArmPlatformTokenSpaceGuid.PcdCoreCount|8
  gArmPlatformTokenSpaceGuid.PcdClusterCount|2

  #
  # GIC. Bases agree between uefiplat.cfg and the device tree; uefiplat.cfg
  # calls the distributor GIC600, upstream calls it GIC500 - same address.
  #
  gArmTokenSpaceGuid.PcdGicDistributorBase|0x17A00000
  gArmTokenSpaceGuid.PcdGicRedistributorsBase|0x17A60000

  #
  # Timer
  #
  gArmTokenSpaceGuid.PcdArmArchTimerSecIntrNum|17
  gArmTokenSpaceGuid.PcdArmArchTimerIntrNum|18

  #
  # Shared RFS Area
  #
  gQcomPkgTokenSpaceGuid.PcdDestinationVmCount|2

  #
  # SMBIOS
  #
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosProcessorModel|"Snapdragon (TM) 750G @ 2.21 GHz"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosProcessorPartNumber|"SM7225"

[LibraryClasses]
  #
  # SEC Libraries
  #
  PlatformSecLib|BitraPkg/Library/PlatformSecLib/PlatformSecLib.inf

  #
  # ACPI Libraries
  #
  AcpiTableUpdateLib|BitraPkg/Library/AcpiTableUpdateLib/AcpiTableUpdateLib.inf
