/*
 * nvme.h - NVM Express 1.4 register, command and completion layouts used
 * by the driver (docs/drivers/nvme/design.md).
 */
#ifndef DRIVERS_NVME_H
#define DRIVERS_NVME_H

#include <kernel/types.h>

/* Controller registers (BAR0). */
#define NVME_REG_CAP   0x00
#define NVME_REG_VS    0x08
#define NVME_REG_INTMS 0x0c
#define NVME_REG_INTMC 0x10
#define NVME_REG_CC    0x14
#define NVME_REG_CSTS  0x1c
#define NVME_REG_AQA   0x24
#define NVME_REG_ASQ   0x28
#define NVME_REG_ACQ   0x30
#define NVME_REG_DBS   0x1000

#define NVME_CAP_MQES(cap)   ((unsigned)((cap) & 0xffff))
#define NVME_CAP_TO(cap)     ((unsigned)(((cap) >> 24) & 0xff))       /* 500 ms units */
#define NVME_CAP_DSTRD(cap)  ((unsigned)(((cap) >> 32) & 0xf))
#define NVME_CAP_CSS_NVM(cap) ((((cap) >> 37) & 1) != 0)
#define NVME_CAP_MPSMIN(cap) ((unsigned)(((cap) >> 48) & 0xf))

#define NVME_CC_EN        (1u << 0)
#define NVME_CC_CSS_NVM   (0u << 4)
#define NVME_CC_MPS_4K    (0u << 7)
#define NVME_CC_AMS_RR    (0u << 11)
#define NVME_CC_SHN_NONE  (0u << 14)
#define NVME_CC_IOSQES    (6u << 16)   /* 64-byte submission entries */
#define NVME_CC_IOCQES    (4u << 20)   /* 16-byte completion entries */

#define NVME_CSTS_RDY (1u << 0)
#define NVME_CSTS_CFS (1u << 1)

/* Admin opcodes. */
#define NVME_ADMIN_DELETE_SQ  0x00
#define NVME_ADMIN_CREATE_SQ  0x01
#define NVME_ADMIN_DELETE_CQ  0x04
#define NVME_ADMIN_CREATE_CQ  0x05
#define NVME_ADMIN_IDENTIFY   0x06
#define NVME_ADMIN_ABORT      0x08
#define NVME_ADMIN_SET_FEATURES 0x09

/* NVM command set opcodes. */
#define NVME_CMD_FLUSH 0x00
#define NVME_CMD_WRITE 0x01
#define NVME_CMD_READ  0x02

#define NVME_CNS_NAMESPACE   0
#define NVME_CNS_CONTROLLER  1
#define NVME_CNS_ACTIVE_LIST 2
#define NVME_FEAT_NUM_QUEUES 0x07

/* Status: DW3 bits 17-31 = status field; SCT bits 25-27, SC bits 17-24. */
#define NVME_CQE_STATUS(dw3)    (((dw3) >> 17) & 0x7fff)
#define NVME_CQE_PHASE(dw3)     (((dw3) >> 16) & 1)
#define NVME_CQE_CID(dw3)       ((uint16_t)((dw3) & 0xffff))
#define NVME_SC_ABORT_REQUESTED 0x07

struct nvme_sqe {
    uint32_t cdw0;      /* opcode 0-7, fuse 8-9, psdt 14-15, cid 16-31 */
    uint32_t nsid;
    uint32_t rsvd2, rsvd3;
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
} __packed;

struct nvme_cqe {
    uint32_t dw0;       /* command specific */
    uint32_t dw1;
    uint32_t dw2;       /* sq head 0-15, sq id 16-31 */
    uint32_t dw3;       /* cid 0-15, phase 16, status 17-31 */
} __packed;

/* Identify Controller (4096 bytes): the fields the driver reads. */
#define NVME_ID_CTRL_SN    4     /* 20 bytes */
#define NVME_ID_CTRL_MN    24    /* 40 bytes */
#define NVME_ID_CTRL_FR    64    /* 8 bytes */
#define NVME_ID_CTRL_MDTS  77    /* power of two, 4 KiB pages; 0 = no limit */
#define NVME_ID_CTRL_SQES  512
#define NVME_ID_CTRL_CQES  513
#define NVME_ID_CTRL_NN    516   /* u32 */

/* Identify Namespace: NSZE u64 at 0, FLBAS at 26, LBAF[i] at 128 + 4 i (MS u16, LBADS u8, RP u8). */
#define NVME_ID_NS_NSZE  0
#define NVME_ID_NS_FLBAS 26
#define NVME_ID_NS_LBAF  128

#endif /* DRIVERS_NVME_H */
