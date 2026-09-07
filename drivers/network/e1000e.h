/*
 * e1000e.h - Intel 82574L registers and descriptors, the subset this
 * driver uses (82574 GbE Controller Family Datasheet, chapter 10).
 * Offsets are bytes from BAR0.
 */

#ifndef DRIVERS_E1000E_H
#define DRIVERS_E1000E_H

#include <kernel/compiler.h>
#include <kernel/types.h>

#define E1000E_VENDOR 0x8086u
#define E1000E_82574L 0x10d3u

/* General */
#define E1000_CTRL     0x0000
#define E1000_STATUS   0x0008
#define E1000_CTRL_EXT 0x0018
#define E1000_ICR      0x00c0
#define E1000_ITR      0x00c4
#define E1000_ICS      0x00c8
#define E1000_IMS      0x00d0
#define E1000_IMC      0x00d8
#define E1000_IVAR     0x00e4
#define E1000_RCTL     0x0100
#define E1000_TCTL     0x0400
#define E1000_TIPG     0x0410
#define E1000_RXCSUM   0x5000
#define E1000_RFCTL    0x5008
#define E1000_MTA      0x5200   /* 128 x 32 bits */
#define E1000_RAL0     0x5400
#define E1000_RAH0     0x5404

/* Receive queue 0 */
#define E1000_RDBAL0   0x2800
#define E1000_RDBAH0   0x2804
#define E1000_RDLEN0   0x2808
#define E1000_RDH0     0x2810
#define E1000_RDT0     0x2818
#define E1000_RXDCTL0  0x2828

/* Transmit queue 0 */
#define E1000_TDBAL0   0x3800
#define E1000_TDBAH0   0x3804
#define E1000_TDLEN0   0x3808
#define E1000_TDH0     0x3810
#define E1000_TDT0     0x3818
#define E1000_TXDCTL0  0x3828

/* CTRL */
#define E1000_CTRL_FD      (1u << 0)
#define E1000_CTRL_ASDE    (1u << 5)
#define E1000_CTRL_SLU     (1u << 6)
#define E1000_CTRL_RST     (1u << 26)
#define E1000_CTRL_PHY_RST (1u << 31)

/* STATUS */
#define E1000_STATUS_LU    (1u << 1)

/* ICR / IMS / IMC. The 82574 has both the legacy causes and the
 * queue-mapped ones; which fires depends on the interrupt scheme. */
#define E1000_ICR_TXDW   (1u << 0)
#define E1000_ICR_LSC    (1u << 2)
#define E1000_ICR_RXDMT0 (1u << 4)
#define E1000_ICR_RXO    (1u << 6)
#define E1000_ICR_RXT0   (1u << 7)
#define E1000_ICR_RXQ0   (1u << 20)
#define E1000_ICR_TXQ0   (1u << 22)
#define E1000_ICR_OTHER  (1u << 24)
#define E1000_IMS_ALL                                                                      \
    (E1000_ICR_TXDW | E1000_ICR_LSC | E1000_ICR_RXDMT0 | E1000_ICR_RXO | E1000_ICR_RXT0 | \
     E1000_ICR_RXQ0 | E1000_ICR_TXQ0 | E1000_ICR_OTHER)

/* IVAR: four bits per cause, low three the MSI-X entry, bit 3 valid.
 * RxQ0 at 0, RxQ1 at 4, TxQ0 at 8, TxQ1 at 12, Other at 16. */
#define E1000_IVAR_VALID    0x8u
#define E1000_IVAR_RXQ0(v)  (((v) | E1000_IVAR_VALID) << 0)
#define E1000_IVAR_TXQ0(v)  (((v) | E1000_IVAR_VALID) << 8)
#define E1000_IVAR_OTHER(v) (((v) | E1000_IVAR_VALID) << 16)

/* RCTL */
#define E1000_RCTL_EN         (1u << 1)
#define E1000_RCTL_SBP        (1u << 2)
#define E1000_RCTL_UPE        (1u << 3)
#define E1000_RCTL_MPE        (1u << 4)
#define E1000_RCTL_LPE        (1u << 5)
#define E1000_RCTL_BAM        (1u << 15)
#define E1000_RCTL_BSIZE_2048 (0u << 16)
#define E1000_RCTL_BSEX       (1u << 25)
#define E1000_RCTL_SECRC      (1u << 26)

/* TCTL */
#define E1000_TCTL_EN   (1u << 1)
#define E1000_TCTL_PSP  (1u << 3)
#define E1000_TCTL_CT   (0x10u << 4)    /* collision threshold */
#define E1000_TCTL_COLD (0x40u << 12)   /* collision distance, full duplex */
#define E1000_TIPG_DEFAULT 0x00602008u  /* IPGT 8, IPGR1 8, IPGR2 6 */

/* RAH */
#define E1000_RAH_AV (1u << 31)

/* Legacy receive descriptor (datasheet §7.1.5). */
struct e1000_rx_desc {
    uint64_t addr;
    uint16_t length;
    uint16_t csum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
} __packed;

#define E1000_RXD_STAT_DD  (1u << 0)
#define E1000_RXD_STAT_EOP (1u << 1)

/* Legacy transmit descriptor (§7.2.3). */
struct e1000_tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t cso;
    uint8_t cmd;
    uint8_t status;
    uint8_t css;
    uint16_t special;
} __packed;

#define E1000_TXD_CMD_EOP  (1u << 0)
#define E1000_TXD_CMD_IFCS (1u << 1)
#define E1000_TXD_CMD_RS   (1u << 3)
#define E1000_TXD_STAT_DD  (1u << 0)

#endif /* DRIVERS_E1000E_H */
