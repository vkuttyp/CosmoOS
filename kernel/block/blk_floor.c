/*
 * blk_floor.c - The C under kernel/block/blk.cee (roadmap 2.0 SA7): the
 * module exports, a C macro, and the values the block layer's CeeSafe
 * self-tests (blktest.cee, parttest.cee) write as their own constants.
 * The block layer is CeeSafe (blk.cee, generated into blk.c).
 */

#include <kernel/blk.h>
#include <kernel/dma.h>
#include <kernel/errno.h>
#include <kernel/gpt.h>
#include <kernel/kmalloc.h>
#include <kernel/module.h>
#include <kernel/page.h>

_Static_assert(EINVAL == 22 && ETIMEDOUT == 110 && EBUSY == 16 && BIO_READ == 0 && BIO_WRITE == 1 &&
                   BIO_PREFLUSH == 1 && BIO_FUA == 2 && BLK_TIMEOUT_NS == 30000000000ull && PAGE_SIZE == 4096 &&
                   DMA_ZERO == 1 && KMEM_ZERO == 1,
               "blktest.cee's and parttest.cee's errno.h, blk.h, page.h, dma.h and kmalloc.h values");
_Static_assert(GPT_BUILD_ARRAY_SECTORS(512) == 32 && GPT_BUILD_HEAD_SECTORS(512) == 34 &&
                   GPT_BUILD_TAIL_SECTORS(512) == 33,
               "parttest.cee's gpt.h values for 512-byte sectors");

/* Module ABI v1 exports (docs/kernel/module/api.md). */
EXPORT_SYMBOL(blk_register);
EXPORT_SYMBOL(blk_register_named);
#if CONFIG_DEBUG
EXPORT_SYMBOL(blk_test_tick);
EXPORT_SYMBOL(blk_test_driver_hooks_set);
#endif
EXPORT_SYMBOL(bio_segment);
EXPORT_SYMBOL(blk_unregister);
EXPORT_SYMBOL(blk_reset);
EXPORT_SYMBOL(blk_submit);
EXPORT_SYMBOL(bio_complete);
EXPORT_SYMBOL(blk_read);
EXPORT_SYMBOL(blk_write);
EXPORT_SYMBOL(blk_write_flags);
EXPORT_SYMBOL(blk_flush);
EXPORT_SYMBOL(blk_find);
EXPORT_SYMBOL(blk_serial_ascii);   /* blkserial.cee: a driver decodes its device's serial with it */
