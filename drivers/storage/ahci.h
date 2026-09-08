/*
 * ahci.h - AHCI 1.3.1 host bus adapter registers, command structures and
 * the ATA commands the driver issues (ATA8-ACS). Register offsets are
 * relative to ABAR (BAR5); port registers to ABAR + 0x100 + 0x80 * port.
 */

#ifndef DRIVERS_STORAGE_AHCI_H
#define DRIVERS_STORAGE_AHCI_H

#include <kernel/compiler.h>
#include <kernel/types.h>

/* Generic host control (§3.1) */
#define AHCI_CAP         0x00
#define AHCI_GHC         0x04
#define AHCI_IS          0x08
#define AHCI_PI          0x0c
#define AHCI_VS          0x10
#define AHCI_CAP2        0x24
#define AHCI_BOHC        0x28

#define CAP_NP(v)        ((v) & 0x1fu)             /* ports - 1 */
#define CAP_NCS(v)       (((v) >> 8) & 0x1fu)      /* command slots - 1 */
#define CAP_SNCQ         (1u << 30)
#define CAP_S64A         (1u << 31)
#define GHC_HR           (1u << 0)
#define GHC_IE           (1u << 1)
#define GHC_AE           (1u << 31)
#define CAP2_BOH         (1u << 0)
#define BOHC_BOS         (1u << 0)
#define BOHC_OOS         (1u << 1)
#define BOHC_BB          (1u << 4)

/* Port registers (§3.3) */
#define AHCI_PORT_BASE   0x100
#define AHCI_PORT_SIZE   0x80
#define PX_CLB           0x00
#define PX_CLBU          0x04
#define PX_FB            0x08
#define PX_FBU           0x0c
#define PX_IS            0x10
#define PX_IE            0x14
#define PX_CMD           0x18
#define PX_TFD           0x20
#define PX_SIG           0x24
#define PX_SSTS          0x28
#define PX_SCTL          0x2c
#define PX_SERR          0x30
#define PX_SACT          0x34
#define PX_CI            0x38

#define PXIS_DHRS        (1u << 0)    /* D2H register FIS */
#define PXIS_PSS         (1u << 1)    /* PIO setup FIS */
#define PXIS_DSS         (1u << 2)    /* DMA setup FIS */
#define PXIS_SDBS        (1u << 3)    /* set device bits FIS */
#define PXIS_UFS         (1u << 4)    /* unknown FIS */
#define PXIS_DPS         (1u << 5)    /* descriptor processed */
#define PXIS_PCS         (1u << 6)    /* port connect change */
#define PXIS_PRCS        (1u << 22)   /* PhyRdy change */
#define PXIS_IPMS        (1u << 23)
#define PXIS_OFS         (1u << 24)   /* overflow */
#define PXIS_INFS        (1u << 26)   /* interface non-fatal error */
#define PXIS_IFS         (1u << 27)   /* interface fatal error */
#define PXIS_HBDS        (1u << 28)   /* host bus data error */
#define PXIS_HBFS        (1u << 29)   /* host bus fatal error */
#define PXIS_TFES        (1u << 30)   /* task file error */
#define PXIS_CPDS        (1u << 31)   /* cold port detect */
#define PXIS_ERRORS      (PXIS_UFS | PXIS_OFS | PXIS_INFS | PXIS_IFS | PXIS_HBDS | PXIS_HBFS | PXIS_TFES)
#define PXIE_WANTED      (PXIS_DHRS | PXIS_PSS | PXIS_DSS | PXIS_SDBS | PXIS_PCS | PXIS_PRCS | PXIS_ERRORS)

#define PXCMD_ST         (1u << 0)
#define PXCMD_SUD        (1u << 1)
#define PXCMD_POD        (1u << 2)
#define PXCMD_CLO        (1u << 3)
#define PXCMD_FRE        (1u << 4)
#define PXCMD_CCS(v)     (((v) >> 8) & 0x1fu)   /* current command slot */
#define PXCMD_FR         (1u << 14)
#define PXCMD_CR         (1u << 15)
#define PXCMD_ICC_ACTIVE (1u << 28)

#define PXTFD_STS(v)     ((v) & 0xffu)
#define PXTFD_ERR(v)     (((v) >> 8) & 0xffu)
#define ATA_STS_ERR      (1u << 0)
#define ATA_STS_DRQ      (1u << 3)
#define ATA_STS_BSY      (1u << 7)

#define PXSSTS_DET(v)    ((v) & 0xfu)
#define PXSSTS_IPM(v)    (((v) >> 8) & 0xfu)
#define DET_PRESENT      3u
#define IPM_ACTIVE       1u
#define PXSCTL_DET_MASK  0xfu
#define PXSCTL_DET_INIT  1u   /* COMRESET */

#define SIG_SATA         0x00000101u
#define SIG_ATAPI        0xeb140101u
#define SIG_PMP          0x96690101u
#define SIG_SEMB         0xc33c0101u

/* Command list header (§4.2.2), 32 bytes, 32 per port, 1 KiB aligned */
struct ahci_cmd_header {
    uint32_t flags;      /* CFL bits 0-4, A 5, W 6, P 7, R 8, B 9, C 10, PMP 12-15, PRDTL 16-31 */
    uint32_t prdbc;      /* bytes transferred */
    uint64_t ctba;       /* command table, 128-byte aligned */
    uint32_t rsvd[4];
} __packed;
#define CMDH_CFL(dw)     ((uint32_t)(dw) & 0x1fu)
#define CMDH_W           (1u << 6)
#define CMDH_P           (1u << 7)
#define CMDH_C           (1u << 10)
#define CMDH_PRDTL(n)    ((uint32_t)(n) << 16)

/* PRDT entry (§4.2.3.3) */
struct ahci_prd {
    uint64_t dba;
    uint32_t rsvd;
    uint32_t dbc;        /* byte count - 1 (bits 0-21, even), I bit 31 */
} __packed;
#define PRD_MAX_BYTES    (4u << 20)

/* Command table (§4.2.3): the FIS, ATAPI command, then the PRDT at 0x80 */
#define AHCI_PRDT_MAX    56u   /* 128 + 56 * 16 = 1024 bytes per table */
struct ahci_cmd_table {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t rsvd[48];
    struct ahci_prd prdt[AHCI_PRDT_MAX];
} __packed;

/* Register host-to-device FIS (SATA 3.x §10.3.4) */
#define FIS_TYPE_REG_H2D 0x27
#define FIS_H2D_C        0x80   /* byte 1: command (not control) update */
#define ATA_DEV_LBA      0x40

/* ATA commands (ATA8-ACS) */
#define ATA_CMD_READ_DMA_EXT   0x25
#define ATA_CMD_WRITE_DMA_EXT  0x35
#define ATA_CMD_FLUSH_CACHE_EXT 0xea
#define ATA_CMD_IDENTIFY       0xec

/* IDENTIFY DEVICE words (ATA8-ACS §7.16) */
#define ID_SERIAL        10   /* 10 words */
#define ID_MODEL         27   /* 20 words */
#define ID_LBA28_COUNT   60   /* 2 words */
#define ID_QUEUE_DEPTH   75   /* bits 0-4: depth - 1 */
#define ID_SATA_CAP      76   /* bit 8: NCQ */
#define ID_CMD_SET_2     83   /* bit 10: LBA48 */
#define ID_CMD_SET_EN_2  85   /* bit 5: write cache enabled */
#define ID_LBA48_COUNT   100  /* 4 words */
#define ID_SECTOR_INFO   106  /* bit 14 valid / bit 15 clear, bit 12: logical sector > 256 words */
#define ID_SECTOR_SIZE   117  /* 2 words: logical sector size in words */

#endif /* DRIVERS_STORAGE_AHCI_H */
