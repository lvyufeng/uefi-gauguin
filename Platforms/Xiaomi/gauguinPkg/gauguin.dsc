##
#  Xiaomi Redmi Note 9 Pro 5G / Mi 10T Lite (gauguin, SM7225 Snapdragon 750G)
#
#  A Mu-only platform, built the way uefi-tb128fu is built.
#
#  Why there are no vendor drivers: gauguin's xbl carries Qualcomm's signed DXE
#  modules, and an earlier revision of this file used them - all 54 of them,
#  extracted into Binaries/gauguin. That firmware never reached a boot manager.
#  It stopped inside UFSDxe, then inside TzDxe, then in a DXE pool drained by
#  drivers that failed and retried, and finally in DxeMain's assertion that every
#  architectural protocol is published. Each of those modules is a closed binary
#  written for another platform build; making them agree is not a matter of
#  ordering them correctly.
#
#  The tablet port reached the opposite conclusion from the same observation:
#  when the ABL has no separable vendor DXE, use the Mu drivers only and let the
#  storage be memory. SimpleFbDxe publishes the cont_splash_region ABL leaves
#  scanning out, and ShengBootMemoryDxe publishes the boot image's ramdisk as a
#  read-only block device for DarwinBoot. Nothing here touches the UFS host.
#
#  The trade is deliberate: no UFS means the payload rides in the ramdisk, so it
#  is bounded by the 128 MiB boot partition rather than by userdata.
#
#  Copyright (c) 2011 - 2022, ARM Limited. All rights reserved.
#  Copyright (c) 2014, Linaro Limited. All rights reserved.
#  Copyright (c) 2015 - 2020, Intel Corporation. All rights reserved.
#  Copyright (c) 2018, Bingxing Wang. All rights reserved.
#  Copyright (c) Microsoft Corporation.
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

################################################################################
#
# Defines Section - statements that will be processed to create a Makefile.
#
################################################################################
[Defines]
  PLATFORM_NAME                  = gauguin
  # USB. Without this the XHCI host stack is never compiled in, and a boot looks like a crash:
  # the phone leaves the bootloader, the panel keeps working, and the host simply stops seeing
  # the device.
  USE_XHCI_HOST_DRIVER           = 1
  PLATFORM_GUID                  = 7E1B4C62-2F4A-4D18-9E31-2C5A0D8B6F42
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/gauguinPkg
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = RELEASE|DEBUG
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = gauguinPkg/gauguin.fdf

  #
  # SimpleFbDxe on the splash buffer ABL leaves scanning out, as on tb128fu.
  # 1 would be Qualcomm's DisplayDxe, which is a vendor binary and needs the
  # DSI display mailbox that this build does not carry.
  #
  USE_CUSTOM_DISPLAY_DRIVER      = 0

!include BitraPkg/BitraPkg.dsc.inc

[PcdsFixedAtBuild]
  #
  # DDR Memory
  #
  # MEASURED: the low bank starts at 0x80000000 on gauguin.
  #
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x80000000

  #
  # UEFI Stack
  #
  # The top of the low bank, above the firmware volume. Kept where the vendor
  # memory map put it so the stack cannot land inside the DXE heap.
  #
  gArmPlatformTokenSpaceGuid.PcdCPUCoresStackBase|0x9FF90000
  gArmPlatformTokenSpaceGuid.PcdCPUCorePrimaryStackSize|0x40000

  #
  # SMBIOS
  #
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemManufacturer|"Xiaomi"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemModel|"Redmi Note 9 Pro 5G"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailModel|"gauguin"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailSku|"M2007J17C"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemBoardModel|"gauguin"

  #
  # Simple Frame Buffer
  #
  # 1080x2248, and the number comes from what works, not from what the panel is.
  #
  # The panel is j17_36_02_0a: dsi_display0 on the command line names it, and its node in the
  # device tree says 1080x2400 at 120 Hz. 2400 in this PCD was measured to be wrong anyway:
  # with it, the Mu logo and a few lines of the boot appear and then the panel goes dark,
  # because the console lays text out past the last row ABL is actually scanning out. 2248 is
  # the value this port booted the UniXNU menu with.
  #
  # Do not "fix" this back to 2400 from the device tree alone - the panel's native height and
  # the visible extent ABL leaves behind are different numbers here, and only the second one
  # decides whether text can be read.
  #
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferWidth|1080
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferHeight|2248
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferColorDepth|32
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferFontScale|1
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferDelay|20000

  #
  # Platform PEI
  #
  # gauguin's ABL is a Linux loader, not the stock DXE firmware: it leaves no
  # scheduler or FV HOBs for PlatformPeim to consume.
  #
  gQcomPkgTokenSpaceGuid.PcdPlatformType|"LA"
  gQcomPkgTokenSpaceGuid.PcdBuildXblHobs|FALSE

  #
  # UEFI variables, kept in RAM.
  #
  # VariableRuntimeDxe publishes EFI_VARIABLE_ARCH_PROTOCOL, and on this platform
  # almost every other architectural protocol declares a DEPEX on it. With the PCD
  # below left at Mu's default of 0 the driver had no store to open and stopped at
  # its own entry point - the last line the panel showed was its "Loading driver"
  # line, and nothing after it ever ran.
  #
  # The store lives in the "Log Buffer" reservation: 32 KiB of RtData at
  # 0x9FFF7000 that the memory map already declares and nothing else claims. The
  # variables do not survive a reboot, which is enough to reach BDS and the loader.
  #
  # Emulated NV: the driver keeps the variable store in RAM it allocates itself,
  # instead of opening a flash region that this phone does not expose to the
  # firmware. Pointing PcdFlashNvStorageVariableBase64 at a RAM address is NOT the
  # same thing - the driver still walks the flash layout there and finds no valid
  # store header, so it stops on its own entry point.
  gEfiMdeModulePkgTokenSpaceGuid.PcdEmuVariableNvModeEnable|TRUE
  gEfiMdeModulePkgTokenSpaceGuid.PcdVariableStoreSize|0x8000

[LibraryClasses]
  #
  # Memory Libraries
  #
  MemoryMapLib|gauguinPkg/Library/MemoryMapLib/MemoryMapLib.inf
  # The console is the only output on this phone, and it scrolls: a boot that dies leaves the
  # last forty lines and nothing else. This mirrors the interesting events into the reserved
  # Log Buffer instead, so a recovery image can dump the whole sequence afterwards.
  LogBufferLib|SiliciumPkg/Library/LogBufferLib/LogBufferLib.inf
  BootScreenLib|gauguinPkg/Library/BootScreenLib/BootScreenLib.inf

  #
  # QCOM Libraries
  #
  ConfigurationMapLib|gauguinPkg/Library/ConfigurationMapLib/ConfigurationMapLib.inf

[Components]
  #
  # UniXNU: the boot manager, its registrar, the ramdisk block device and the
  # XNU loader. ShengBootMemoryDxe is shengPkg's: every range it uses comes from
  # the device tree, so it carries no board-specific address.
  #
  UniXNUPkg/Application/UniXNU/UniXNU.inf
  UniXNUPkg/Driver/UniXNUDxe/UniXNUDxe.inf
  gauguinPkg/Drivers/ShengBootMemoryDxe/ShengBootMemoryDxe.inf
  gauguinPkg/Drivers/UsbProbeDxe/UsbProbeDxe.inf
  DarwinBootPkg/Driver/DarwinBoot.inf

  #
  # The log writer. It was written before the move to a Mu-only platform and never
  # made it into this file, which is why the logdump partition stayed all zero on
  # every boot: nothing was copying the rings out of RAM.
  #
  # It is in the component list but NOT in the a-priori batch, on purpose. Its DEPEX
  # is the block I/O protocol, which on this board no driver publishes - a Mu-only
  # platform has no UFS driver - so the dispatcher parks it as Dependent and it never
  # blocks the boot. If a storage driver is ever added back, it starts by itself and
  # begins writing the log; until then, it is inert. Do not promote it to APRIORI.inc.
  #
  gauguinPkg/Drivers/PlogDxe/PlogDxe.inf

  #
  # The early boot screen. It draws into the buffer ABL leaves scanning out, so it is
  # readable before any of the drivers above have run; Depex TRUE and a two-library
  # dependency list keep it that way. It is also in APRIORI.inc, right after DxeMain.
  #
  gauguinPkg/Drivers/BootScreenDxe/BootScreenDxe.inf

[LibraryClasses]
  UniXNULib|UniXNUPkg/Library/UniXNULib/UniXNULib.inf
  #
  # ARM libraries the shared Mu_Basecore keeps outside the packages this build
  # maps by default. CpuDxe needs the exception handler and the MMU, GIC and
  # disassembler libraries, and nothing in the Qualcomm or Silicium packages
  # provides a default for them.
  #
  DefaultExceptionHandlerLib|ArmPkg/Library/DefaultExceptionHandlerLib/DefaultExceptionHandlerLib.inf
  CpuExceptionHandlerLib|ArmPkg/Library/ArmExceptionLib/ArmExceptionLib.inf
  ArmMmuLib|ArmPkg/Library/ArmMmuLib/ArmMmuBaseLib.inf
  ArmDisassemblerLib|ArmPkg/Library/ArmDisassemblerLib/ArmDisassemblerLib.inf
  ArmGicLib|ArmPkg/Drivers/ArmGicDxe/ArmGicLib.inf
  ArmGicArchLib|ArmPkg/Library/ArmGicArchLib/ArmGicArchLib.inf
  FdtLib|MdePkg/Library/BaseFdtLib/BaseFdtLib.inf