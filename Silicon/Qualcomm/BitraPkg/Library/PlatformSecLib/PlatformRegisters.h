#ifndef _PLATFORM_REGISTERS_H_
#define _PLATFORM_REGISTERS_H_

//
// SMMU Bank Details
//
#define SMMU_CTX_BANK_SIZE                     0x1000

//
// SMMU Bank Offsets
//
#define SMMU_CTX_BANK_0_OFFSET                 0x80000

//
// SMMU Bank Register Offsets
//
#define SMMU_CTX_BANK_SCTLR_OFFSET             0x00
#define SMMU_CTX_BANK_TTBR0_0_OFFSET           0x20
#define SMMU_CTX_BANK_TTBR0_1_OFFSET           0x24
#define SMMU_CTX_BANK_TTBR1_0_OFFSET           0x28
#define SMMU_CTX_BANK_TTBR1_1_OFFSET           0x2C
#define SMMU_CTX_BANK_MAIR0_OFFSET             0x38
#define SMMU_CTX_BANK_MAIR1_OFFSET             0x3C
#define SMMU_CTX_BANK_TTBCR_OFFSET             0x30

//
// SMMU SCTLR
//
#define SMMU_CCA_SCTLR                         0x9F00E0

//
// SMMU Context Banks
//
#define UFS_CTX_BANK                           2

//
// WatchDog Timer Registers
//
// Verified against mainline: sm6350.dtsi declares the watchdog as
// "qcom,apss-wdt-sm6350", "qcom,kpss-wdt", and drivers/watchdog/qcom-wdt.c
// matches on the *second* compatible -> match_data_kpss -> WDT_EN = 0x8.
// (The other layout in that driver, match_data_apcs_tmr, puts it at 0x40 and
// is for the older "qcom,kpss-timer".)
//
#define APSS_WDT_ENABLE_OFFSET                 0x8

#endif /* _PLATFORM_REGISTERS_H_ */
