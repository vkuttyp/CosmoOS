/*
^ * diskid.c - What a disk's device reports about itself, decoded (diskid.h):
 * the decoders ahci.c, nvme.c and usb_storage.c had, moved here unchanged.
 * Linked into each of their modules (build/module.mk).
 */

#include <kernel/diskid.h>
#include <kernel/string.h>

/* ATA IDENTIFY DEVICE's words (ACS-3 §7.12.7). */
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

/* NVMe Identify (NVMe 1.4 §5.15). */
#define NVME_ID_CTRL_SN    4     /* 20 bytes */
#define NVME_ID_CTRL_MN    24    /* 40 bytes */
#define NVME_ID_CTRL_MDTS  77    /* power of two, 4 KiB pages; 0 = no limit */
#define NVME_ID_CTRL_NN    516   /* u32 */
#define NVME_ID_NS_NSZE  0
#define NVME_ID_NS_FLBAS 26
#define NVME_ID_NS_LBAF  128

static void ata_string(char *out, const uint16_t *words, unsigned nwords)
{
    for (unsigned i = 0; i < nwords; i++) {
        out[2 * i] = (char)(words[i] >> 8);
        out[2 * i + 1] = (char)(words[i] & 0xff);
    }
    out[2 * nwords] = '\0';
    for (char *e = out + 2 * nwords - 1; e >= out && *e == ' '; e--)
        *e = '\0';
    char *s = out;
    while (*s == ' ')
        s++;
    if (s != out)
        memmove(out, s, strlen(s) + 1);
}

void ata_identity_decode(const uint16_t *id, char *model, char *serial, struct ata_identity *out)
{
    ata_string(model, id + ID_MODEL, 20);
    ata_string(serial, id + ID_SERIAL, 10);
    out->lba48 = (id[ID_CMD_SET_2] & (1u << 10)) != 0;
    uint64_t sectors;
    if (out->lba48) {
        sectors = (uint64_t)id[ID_LBA48_COUNT] | ((uint64_t)id[ID_LBA48_COUNT + 1] << 16) |
                  ((uint64_t)id[ID_LBA48_COUNT + 2] << 32) | ((uint64_t)id[ID_LBA48_COUNT + 3] << 48);
    } else {
        sectors = (uint64_t)id[ID_LBA28_COUNT] | ((uint64_t)id[ID_LBA28_COUNT + 1] << 16);
    }
    uint32_t ss = 512;
    uint16_t info = id[ID_SECTOR_INFO];
    if ((info & 0xc000u) == 0x4000u && (info & (1u << 12))) {
        uint32_t words = (uint32_t)id[ID_SECTOR_SIZE] | ((uint32_t)id[ID_SECTOR_SIZE + 1] << 16);
        ss = words * 2;
    }
    out->wcache = (id[ID_CMD_SET_EN_2] & (1u << 5)) != 0;
    out->ncq = (id[ID_SATA_CAP] & (1u << 8)) != 0;
    out->qdepth = (id[ID_QUEUE_DEPTH] & 0x1fu) + 1;
    out->sectors = sectors;
    out->sector_size = ss;
    out->usable = !(sectors == 0 || ss < 512 || ss > 4096 || (ss & (ss - 1)) != 0);
}

void nvme_ctrl_identity_decode(const uint8_t *id, uint32_t limit, char *serial, char *model,
                               struct nvme_ctrl_identity *out)
{
    memcpy(serial, id + NVME_ID_CTRL_SN, 20);
    memcpy(model, id + NVME_ID_CTRL_MN, 40);
    serial[20] = '\0';
    model[40] = '\0';
    for (int i = 19; i >= 0 && serial[i] == ' '; i--)
        serial[i] = '\0';
    for (int i = 39; i >= 0 && model[i] == ' '; i--)
        model[i] = '\0';
    unsigned mdts = id[NVME_ID_CTRL_MDTS];
    out->max_pages = mdts == 0 || mdts >= 7 ? limit : (1u << mdts);
    if (out->max_pages > limit)
        out->max_pages = limit;
    memcpy(&out->nn, id + NVME_ID_CTRL_NN, sizeof(out->nn));
}

void nvme_ns_identity_decode(const uint8_t *id, struct nvme_ns_identity *out)
{
    memcpy(&out->nsze, id + NVME_ID_NS_NSZE, sizeof(out->nsze));
    unsigned flbas = id[NVME_ID_NS_FLBAS] & 0xf;
    uint16_t ms;
    memcpy(&ms, id + NVME_ID_NS_LBAF + 4 * flbas, sizeof(ms));
    out->ms = ms;
    out->lbads = id[NVME_ID_NS_LBAF + 4 * flbas + 2];
    out->usable = !(out->nsze == 0 || ms != 0 || out->lbads < 9 || out->lbads > 12);
}

static inline uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

uint32_t scsi_inquiry_decode(const uint8_t *inq, char *vendor, char *product, char *rev)
{
    memcpy(vendor, inq + 8, 8);
    memcpy(product, inq + 16, 16);
    memcpy(rev, inq + 32, 4);
    vendor[8] = product[16] = rev[4] = '\0';
    for (char *p = vendor + 7; p >= vendor && *p == ' '; p--) *p = '\0';
    for (char *p = product + 15; p >= product && *p == ' '; p--) *p = '\0';
    return inq[0] & 0x1fu;
}

bool scsi_capacity10_decode(const uint8_t *buf, uint32_t *last, uint32_t *block_size)
{
    uint32_t l = be32(buf), bs = be32(buf + 4);
    *last = l;
    *block_size = bs;
    return !(bs < 512 || bs > 4096 || (bs & (bs - 1)) != 0 || l == 0xffffffffu);
}
