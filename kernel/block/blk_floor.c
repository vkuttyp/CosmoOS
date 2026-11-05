/*
 * blk_floor.c - The C under kernel/block/blk.cee (roadmap 2.0 SA7): the
 * module exports, a C macro. The block layer is CeeSafe (blk.cee,
 * generated into blk.c).
 */

#include <kernel/blk.h>
#include <kernel/module.h>

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
