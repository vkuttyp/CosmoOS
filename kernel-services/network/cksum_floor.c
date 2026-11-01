/*
 * cksum_floor.c - What the Internet checksum, now CeeSafe
 * (kernel-services/network/cksum.cee), keeps in C: its export to modules.
 * A virtio-net driver built as a module finishes a packet's checksum with
 * m_csum_complete when its device cannot.
 */

#include <kernel/module.h>
#include <kernel/net/cksum.h>

EXPORT_SYMBOL(m_csum_complete);
