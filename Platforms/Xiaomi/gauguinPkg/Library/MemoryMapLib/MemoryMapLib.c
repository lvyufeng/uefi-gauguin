/**
  Copyright (c) 2011-2012, ARM Limited. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <Library/MemoryMapLib.h>

/*
 * Memory map of the Xiaomi Redmi Note 9 Pro 5G (gauguin, SM7125 lagoon).
 *
 * Every reservation below is a /reserved-memory node read from this phone's own
 * device tree, and the firmware window is the one the vendor map used, so the
 * two agree with what ABL actually leaves behind.
 *
 * The layout changed with the move to a Mu-only build. The vendor DXE modules
 * used to place themselves across 0x9B800000..0x9FC00000 and left a DXE pool
 * that ran dry mid-dispatch; this map instead gives the pool one contiguous run
 * and keeps the boot image's ramdisk reserved, because ShengBootMemoryDxe
 * publishes that memory as a block device the FAT driver reads while DarwinBoot
 * works inside it.
 */
STATIC
EFI_MEMORY_REGION_DESCRIPTOR
gMemoryDescriptor[] = {
  // Name, Address, Length, HobOption, ResourceType, ResourceAttribute, MemoryType, ArmAttribute

  // DDR Regions
  {"HLOS", 0x80000000, 0x00600000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv, WRITE_BACK_XN},
  {"XBL AOP", 0x80600000, 0x00026000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"AOP CMD DB", 0x80860000, 0x00020000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"SEC APPS", 0x808FF000, 0x00001000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"SMEM", 0x80900000, 0x00200000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"CDSP SECURE", 0x80B00000, 0x01E00000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},

  {"CAMERA FW", 0x86000000, 0x00500000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"NPU FW", 0x86500000, 0x00500000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"VIDEO FW", 0x86A00000, 0x00500000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"CDSP FW", 0x86F00000, 0x01E00000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"ADSP FW", 0x88D00000, 0x02800000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"WLAN FW", 0x8B500000, 0x00200000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  // IPA firmware, IPA GSI and the GPU's gpu_region@8b715400, as one reservation.
  //
  // The device tree gives three nodes here whose sizes stop mid-page - IPA GSI is
  // 0x5400 bytes at 0x8B710000, gpu_region is 0x2000 at 0x8B715400 - and the MMU
  // cannot map a range that does not end on a 4 KiB boundary:
  //
  //   UpdateRegionMapping RegionStart: 0x8B710000 or RegionLength: 0x5400 are not page aligned!
  //   Failed to Configure MMU! Status = Invalid Parameter
  //
  // which is where this firmware stopped the first time it ran. Rounding each node
  // out separately would leave the gaps between them unmapped while still owned by
  // the secure side, so they become one reservation covering all three: 0x8B700000
  // up to the modem at 0x8B800000. Nothing in this build reads the IPA or the GPU.
  {"IPA GPU RESERVED", 0x8B700000, 0x00100000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"MODEM FW", 0x8B800000, 0x0F800000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},

  // The two gaps in the low bank, as ordinary system memory.
  //
  //  0x82900000..0x86000000  (55 MiB) - above the CDSP secure regions, below the camera
  //  0x9B000000..0x9FC00000  (76 MiB) - above the modem, up to the firmware volume
  //
  // Both are free: no /reserved-memory node covers them and no firmware region does.
  // They are listed separately, and the first one comes first, on purpose: PrePi allocates
  // the DxeCore image out of the lowest conventional range, so the core lands near the start
  // of the first gap and keeps its own pages out of the second, which is where the DXE pool
  // ends up. When both gaps were one range reaching from 0x83000000 the core was loaded at
  // 0x9B363000 - inside the very pool it went on to manage - and DxeCore stopped at once.
  //
  // An earlier revision also put 96 MiB at 0x83000000 and ran straight through CAMERA, NPU,
  // VIDEO, CDSP and ADSP, all of which the secure world still owns.
  //
  // 32 MiB, not 55: DecompressFirstFv() allocates the decompressed volume with AllocatePages out
  // of the first conventional range, and with the whole gap free it landed at 0x84C4E000 - inside
  // this very pool. The pool then grew over the volume. Reading a driver out of the volume walks
  // its file headers, and once those are overwritten FvGetNextFile never reaches the name it is
  // looking for: dispatch hangs inside GetFileBufferByFilePath with no error and no output. The
  // volume now gets its own range below, and the pool stops short of it.
  {"DXE Heap", 0x82900000, 0x02000000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv, WRITE_BACK_XN},
  {"FV Image", 0x84900000, 0x01700000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},

  // Firmware window, top of the low bank.
  //
  // 11 MiB, not the 3 MiB the stock uefiplat.cfg declares. The volume carries the vendor's own
  // DXE set and compresses to just over 3 MiB, at which point FVMAIN_COMPACT no longer fit and the
  // FV was truncated - dispatch then stopped on whichever driver fell off the end. It grows
  // downwards into the free 76 MiB above the modem; upwards is SEC Heap at 0x9FF00000.
  {"UEFI FD", 0x9F400000, 0x00B00000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
  {"SEC Heap", 0x9FF00000, 0x0008C000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
  {"CPU Vectors", 0x9FF8C000, 0x00001000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
  {"MMU PageTables", 0x9FF8D000, 0x00003000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
  {"UEFI Stack", 0x9FF90000, 0x00040000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
  {"Log Buffer", 0x9FFF7000, 0x00008000, AddMem, SYS_MEM, SYS_MEM_CAP, RtData, WRITE_BACK_XN},
  {"Info Blk", 0x9FFFF000, 0x00001000, AddMem, SYS_MEM, SYS_MEM_CAP, RtData, WRITE_BACK_XN},

  // Display. ABL leaves the J17 panel scanning out this buffer, and SimpleFbDxe
  // draws into it: the console is the boot logo's framebuffer, not a new one.
  {"Display Reserved", 0xA0000000, 0x02300000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH_XN},
  {"DFPS Data", 0xA2300000, 0x00100000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH_XN},
  {"DISP RDUMP", 0xAC000000, 0x01000000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},

  // The rest of the low bank is free for DarwinBoot to hand to XNU.
  {"DRAM 0xAD000000", 0xAD000000, 0x13000000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv, WRITE_BACK_XN},

  // removed_region@c0000000: no-map in the device tree, so it never enters the map.

  // The upper bank: 4 GiB where the payload's kernel and ramdisk live.
  //
  // BootShim loads the boot image into this bank and ShengBootMemoryDxe reads
  // the ramdisk back out of it. Marking the whole bank conventional lets the
  // DXE allocator hand away memory the ramdisk still occupies, so the image and
  // its ramdisk are reserved by the same rule the tablet port uses.
  {"Boot Image", 0x100000000, 0x01000000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},
  {"Ramdisk", 0x101000000, 0x08000000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},
  {"DRAM 0x109000000", 0x109000000, 0x257000000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv, WRITE_BACK_XN},

  // Register Regions
  {"AOP_SS_MSG_RAM", 0x0C300000, 0x00100000, NoHob, MMAP_IO, INITIALIZED, Conv, NS_DEVICE},
  {"IMEM Base", 0x14680000, 0x0002E000, NoHob, MMAP_IO, INITIALIZED, Conv, NS_DEVICE},
  {"IMEM Cookie Base", 0x146AA000, 0x00001000, AddDev, MMAP_IO, INITIALIZED, Conv, NS_DEVICE},
  {"GCC CLK CTL", 0x00100000, 0x00200000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"IPC_ROUTER_TOP", 0x00400000, 0x00100000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"MMCX_CPR3", 0x0062C000, 0x00004000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"SECURITY CONTROL", 0x00780000, 0x00010000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"SDC1_REG", 0x007C0000, 0x00020000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"QUPV3_0_GSI", 0x00800000, 0x00100000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"QUPV3_1_GSI", 0x00900000, 0x00100000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"PRNG_CFG_PRNG", 0x00790000, 0x00010000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"UFS UFS REGS", 0x01D80000, 0x00020000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"CRYPTO0 CRYPTO", 0x01DC0000, 0x00040000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"TCSR_TCSR_REGS", 0x01FC0000, 0x00040000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"GPU_GMU_CX_BLK", 0x03D7D000, 0x0000C000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"GPU_CC", 0x03D90000, 0x00009000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"GPU_CPR", 0x03D9A000, 0x00004000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"MCCC_MCCC_MSTR", 0x090B0000, 0x00008000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"TLMM", 0x0F000000, 0x01000000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"QUPV3_SSC_GSI", 0x05A00000, 0x000D0000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"PERIPH_SS", 0x08800000, 0x00200000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"NPU_CC", 0x09980000, 0x00010000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"USB30_PRIM", 0x0A600000, 0x00200000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"USB_RUMI", 0x0A720000, 0x00010000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"USB30_SEC", 0x0A800000, 0x0011B000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"PDC_DISPLAY", 0x0B2A0000, 0x00010000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"PDC_DISP_SEQ", 0x0B4A0000, 0x00010000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"RPMH_BCM_BCM_TOP", 0x0BA00000, 0x00200000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"RPMH_CPRF_CPRF", 0x0C200000, 0x00010000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"SLP_CNTR", 0x0C221000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"TSENS0", 0x0C222000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"TSENS1", 0x0C223000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"TSENS0_TM", 0x0C263000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"PSHOLD", 0x0C264000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"TSENS1_TM", 0x0C265000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"PMIC ARB SPMI", 0x0C400000, 0x02800000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"VIDEO_CC", 0x0AAF0000, 0x00010000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"TITAN_SS_TITAN", 0x0AC00000, 0x00121000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"TITAN_CAM_CC", 0x0AD00000, 0x00020000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"MDSS", 0x0AE00000, 0x00200000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"DISP_CC", 0x0AF00000, 0x00020000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"APSS_GIC600_GICD", 0x17A00000, 0x00170000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"APSS_GIC500_GICR", 0x17A60000, 0x00100000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"QTIMER", 0x17C00000, 0x00110000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"APSS_WDT_TMR1", 0x17C10000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"APSS_RSC_RSCCR", 0x18200000, 0x00030000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"SILVER_CLK_CTL", 0x18280000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"SILVER_ACD", 0x18290000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"GOLD_CLK_CTL", 0x18282000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"GOLDPLUS_CLK_CTL", 0x18286000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"GOLD_ACD", 0x18292000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"GOLDPLUS_ACD", 0x18296000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"L3_CLK_CTL", 0x18284000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"L3_ACD", 0x18294000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"APSS_ACTPM_WRAP", 0x18300000, 0x000B0000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"SMMU", 0x15000000, 0x00200000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
};

VOID
GetMemoryMap (
  OUT EFI_MEMORY_REGION_DESCRIPTOR **MemoryDescriptor,
  OUT UINT8                         *MemoryDescriptorCount)
{
  // Pass Data
  *MemoryDescriptor      = gMemoryDescriptor;
  *MemoryDescriptorCount = ARRAY_SIZE (gMemoryDescriptor);
}
