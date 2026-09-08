/*
 * xhci.h - eXtensible Host Controller Interface 1.2: registers, TRBs,
 * contexts. Register offsets are relative to the capability base
 * (BAR0), the operational base (BAR0 + CAPLENGTH), the runtime base
 * (BAR0 + RTSOFF) and the doorbell array (BAR0 + DBOFF), as the
 * specification lays them out (§5).
 */

#ifndef DRIVERS_USB_XHCI_H
#define DRIVERS_USB_XHCI_H

#include <kernel/compiler.h>
#include <kernel/types.h>

/* Capability registers (§5.3) */
#define XHCI_CAPLENGTH   0x00   /* 8 bits */
#define XHCI_HCIVERSION  0x02   /* 16 bits */
#define XHCI_HCSPARAMS1  0x04
#define XHCI_HCSPARAMS2  0x08
#define XHCI_HCSPARAMS3  0x0c
#define XHCI_HCCPARAMS1  0x10
#define XHCI_DBOFF       0x14
#define XHCI_RTSOFF      0x18

#define HCS1_MAX_SLOTS(v)  ((v) & 0xffu)
#define HCS1_MAX_INTRS(v)  (((v) >> 8) & 0x7ffu)
#define HCS1_MAX_PORTS(v)  (((v) >> 24) & 0xffu)
#define HCS2_SCRATCHPADS(v) (((((v) >> 21) & 0x1fu) << 5) | (((v) >> 27) & 0x1fu))
#define HCC1_AC64          (1u << 0)
#define HCC1_CSZ           (1u << 2)

/* Operational registers (§5.4) */
#define XHCI_USBCMD      0x00
#define XHCI_USBSTS      0x04
#define XHCI_PAGESIZE    0x08
#define XHCI_DNCTRL      0x14
#define XHCI_CRCR        0x18   /* 64 bits */
#define XHCI_DCBAAP      0x30   /* 64 bits */
#define XHCI_CONFIG      0x38
#define XHCI_PORTSC(p)   (0x400 + 0x10 * ((p) - 1))   /* 1-based port */

#define USBCMD_RS        (1u << 0)
#define USBCMD_HCRST     (1u << 1)
#define USBCMD_INTE      (1u << 2)
#define USBCMD_HSEE      (1u << 3)

#define USBSTS_HCH       (1u << 0)
#define USBSTS_HSE       (1u << 2)
#define USBSTS_EINT      (1u << 3)
#define USBSTS_PCD       (1u << 4)
#define USBSTS_CNR       (1u << 11)
#define USBSTS_HCE       (1u << 12)

#define CRCR_RCS         (1u << 0)
#define CRCR_CA          (1u << 2)
#define CRCR_CRR         (1u << 3)

#define PORTSC_CCS       (1u << 0)
#define PORTSC_PED       (1u << 1)
#define PORTSC_OCA       (1u << 3)
#define PORTSC_PR        (1u << 4)
#define PORTSC_PLS_MASK  (0xfu << 5)
#define PORTSC_PP        (1u << 9)
#define PORTSC_SPEED(v)  (((v) >> 10) & 0xfu)
#define PORTSC_CSC       (1u << 17)
#define PORTSC_PEC       (1u << 18)
#define PORTSC_WRC       (1u << 19)
#define PORTSC_OCC       (1u << 20)
#define PORTSC_PRC       (1u << 21)
#define PORTSC_PLC       (1u << 22)
#define PORTSC_CEC       (1u << 23)
#define PORTSC_CHANGE_BITS (PORTSC_CSC | PORTSC_PEC | PORTSC_WRC | PORTSC_OCC | PORTSC_PRC | PORTSC_PLC | PORTSC_CEC)
/* Written back as read, minus the bits a write of 1 would act on. */
#define PORTSC_RW_MASK   (PORTSC_PP | PORTSC_PLS_MASK | (1u << 27) /* WCE */ | (1u << 26) | (1u << 25))

/* Runtime registers (§5.5): interrupter 0 */
#define XHCI_IR0         0x20
#define XHCI_IMAN        0x00
#define XHCI_IMOD        0x04
#define XHCI_ERSTSZ      0x08
#define XHCI_ERSTBA      0x10   /* 64 bits */
#define XHCI_ERDP        0x18   /* 64 bits */
#define IMAN_IP          (1u << 0)
#define IMAN_IE          (1u << 1)
#define ERDP_EHB         (1ull << 3)

/* TRBs (§6.4): 16 bytes, dword 3 carries the type and the cycle bit. */
struct xhci_trb {
    uint64_t ptr;
    uint32_t status;
    uint32_t control;
} __packed;

#define TRB_CYCLE        (1u << 0)
#define TRB_TC           (1u << 1)   /* link: toggle cycle */
#define TRB_ENT          (1u << 1)   /* normal: evaluate next TRB */
#define TRB_ISP          (1u << 2)   /* interrupt on short packet */
#define TRB_CH           (1u << 4)   /* chain */
#define TRB_IOC          (1u << 5)   /* interrupt on completion */
#define TRB_IDT          (1u << 6)   /* immediate data (setup stage) */
#define TRB_BSR          (1u << 9)   /* address device: block SET_ADDRESS */
#define TRB_TYPE(t)      ((uint32_t)(t) << 10)
#define TRB_TYPE_OF(c)   (((c) >> 10) & 0x3fu)
#define TRB_DIR_IN       (1u << 16)  /* data/status stage direction */
#define TRB_TRT_OUT      (2u << 16)  /* setup stage: OUT data follows */
#define TRB_TRT_IN       (3u << 16)  /* setup stage: IN data follows */
#define TRB_EP_ID(id)    ((uint32_t)(id) << 16)
#define TRB_SLOT(s)      ((uint32_t)(s) << 24)
#define TRB_SLOT_OF(c)   (((c) >> 24) & 0xffu)
#define TRB_EP_ID_OF(c)  (((c) >> 16) & 0x1fu)

#define TRB_NORMAL           1
#define TRB_SETUP            2
#define TRB_DATA             3
#define TRB_STATUS           4
#define TRB_LINK             6
#define TRB_NOOP             8
#define TRB_CMD_ENABLE_SLOT  9
#define TRB_CMD_DISABLE_SLOT 10
#define TRB_CMD_ADDRESS_DEV  11
#define TRB_CMD_CONFIG_EP    12
#define TRB_CMD_EVAL_CTX     13
#define TRB_CMD_RESET_EP     14
#define TRB_CMD_STOP_EP      15
#define TRB_CMD_SET_DEQ      16
#define TRB_CMD_NOOP         23
#define TRB_EV_TRANSFER      32
#define TRB_EV_CMD_COMPLETE  33
#define TRB_EV_PORT_STATUS   34
#define TRB_EV_HOST_CTRL     37

#define TRB_CC_OF(status)    (((status) >> 24) & 0xffu)
#define TRB_RESIDUAL_OF(st)  ((st) & 0xffffffu)
#define TRB_PORT_OF(ptr)     ((unsigned)(((ptr) >> 24) & 0xffu))

/* Completion codes (§6.4.5) */
#define CC_SUCCESS           1
#define CC_DATA_BUFFER_ERR   2
#define CC_BABBLE            3
#define CC_USB_TRANSACTION   4
#define CC_TRB_ERROR         5
#define CC_STALL             6
#define CC_RESOURCE          7
#define CC_BANDWIDTH         8
#define CC_NO_SLOTS          9
#define CC_SHORT_PACKET      13
#define CC_RING_UNDERRUN     14
#define CC_RING_OVERRUN      15
#define CC_PARAMETER         17
#define CC_CONTEXT_STATE     19
#define CC_STOPPED           26
#define CC_STOPPED_LEN_INV   27
#define CC_STOPPED_SHORT     28

/* Contexts (§6.2): 32 or 64 bytes each (HCCPARAMS1.CSZ); the fields
 * used here live in the first 32. */
struct xhci_slot_ctx {
    uint32_t dw[8];
} __packed;
#define SLOT_ROUTE(r)        ((uint32_t)(r) & 0xfffffu)   /* dword 0, bits 0-19 (five tiers of four bits) */
#define SLOT_SPEED(s)        ((uint32_t)(s) << 20)
#define SLOT_MTT             (1u << 25)
#define SLOT_HUB             (1u << 26)
#define SLOT_ENTRIES(n)      ((uint32_t)(n) << 27)
#define SLOT_ROOT_PORT(p)    ((uint32_t)(p) << 16)   /* dword 1 */
#define SLOT_PORTS(n)        ((uint32_t)(n) << 24)   /* dword 1: a hub's ports */
#define SLOT_TT_HUB(s)       ((uint32_t)(s) & 0xffu)      /* dword 2: the transaction translator's slot */
#define SLOT_TT_PORT(p)      ((uint32_t)(p) << 8)         /* dword 2: and its port */
#define SLOT_ADDR_OF(dw3)    ((dw3) & 0xffu)
#define SLOT_STATE_OF(dw3)   (((dw3) >> 27) & 0x1fu)

struct xhci_ep_ctx {
    uint32_t dw[8];
} __packed;
#define EP_STATE_OF(dw0)     ((dw0) & 0x7u)
#define EP_STATE_DISABLED    0
#define EP_STATE_RUNNING     1
#define EP_STATE_HALTED      2
#define EP_STATE_STOPPED     3
#define EP_STATE_ERROR       4
#define EP_INTERVAL(i)       ((uint32_t)(i) << 16)         /* dword 0 */
#define EP_CERR(n)           ((uint32_t)(n) << 1)          /* dword 1 */
#define EP_TYPE(t)           ((uint32_t)(t) << 3)
#define EP_MPS(m)            ((uint32_t)(m) << 16)
#define EP_TYPE_ISOC_OUT     1
#define EP_TYPE_BULK_OUT     2
#define EP_TYPE_INT_OUT      3
#define EP_TYPE_CONTROL      4
#define EP_TYPE_ISOC_IN      5
#define EP_TYPE_BULK_IN      6
#define EP_TYPE_INT_IN       7
#define EP_DCS               (1ull << 0)                    /* dequeue pointer cycle state */
#define EP_AVG_TRB_LEN(n)    ((uint32_t)(n))                /* dword 4 */

struct xhci_input_ctrl_ctx {
    uint32_t drop, add;
    uint32_t rsvd[6];
} __packed;

/* The endpoint id (DCI) of an endpoint address: EP0 is 1. */
static inline unsigned xhci_dci(uint8_t ep_addr)
{
    unsigned n = ep_addr & 0x0fu;
    return n == 0 ? 1 : n * 2 + ((ep_addr & 0x80u) ? 1 : 0);
}

#endif /* DRIVERS_USB_XHCI_H */
