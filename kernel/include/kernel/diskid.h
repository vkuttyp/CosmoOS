/*
 * diskid.h - What a disk's device reports about itself, decoded: ATA
 * IDENTIFY DEVICE (ahci), NVMe Identify Controller and Namespace (nvme),
 * SCSI INQUIRY and READ CAPACITY (10) (usb_storage). The bytes are the
 * device's: every decoder reads within the answer's fixed size.
 */

#ifndef KERNEL_DISKID_H
#define KERNEL_DISKID_H

#include <kernel/types.h>

/* ATA IDENTIFY DEVICE's 256 words. `usable`: a capacity and a logical
 * sector of 512 to 4096 bytes, a power of two. */
struct ata_identity {
    uint64_t sectors;
    uint32_t sector_size;
    uint32_t qdepth;     /* NCQ's queue depth, 1-32 */
    bool lba48;
    bool wcache;
    bool ncq;
    bool usable;
};
/* model: 41 bytes, serial: 21, each the ATA string, spaces trimmed. */
void ata_identity_decode(const uint16_t *id, char *model, char *serial, struct ata_identity *out);

/* NVMe Identify Controller's 4096 bytes. `max_pages`: per request, from
 * MDTS, at most `limit`. */
struct nvme_ctrl_identity {
    uint32_t max_pages;
    uint32_t nn;         /* the number of namespaces */
};
/* serial: 21 bytes, model: 41, trailing spaces trimmed. */
void nvme_ctrl_identity_decode(const uint8_t *id, uint32_t limit, char *serial, char *model,
                               struct nvme_ctrl_identity *out);

/* NVMe Identify Namespace's 4096 bytes: the formatted LBA format's
 * fields. `usable`: blocks, no metadata, 512 to 4096 bytes a block. */
struct nvme_ns_identity {
    uint64_t nsze;
    uint32_t ms;
    uint32_t lbads;
    bool usable;
};
void nvme_ns_identity_decode(const uint8_t *id, struct nvme_ns_identity *out);

/* SCSI INQUIRY's 36 bytes: vendor (9 bytes), product (17), revision (5),
 * trailing spaces trimmed from the first two; the result is the
 * peripheral device type. */
uint32_t scsi_inquiry_decode(const uint8_t *inq, char *vendor, char *product, char *rev);
/* READ CAPACITY (10)'s 8 bytes: the last block and the block size; the
 * result is whether they make a disk (512 to 4096 bytes a block, a power
 * of two, and a last block that is not the "too big for 10" marker). */
bool scsi_capacity10_decode(const uint8_t *buf, uint32_t *last, uint32_t *block_size);

#endif /* KERNEL_DISKID_H */
