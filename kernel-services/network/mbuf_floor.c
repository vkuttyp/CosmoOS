/*
 * mbuf_floor.c - What the packet buffers, now CeeSafe
 * (kernel-services/network/mbuf.cee), keep in C: their exports to modules
 * (module ABI v1, docs/kernel/module/api.md). A network driver built as a
 * module gets, fills and frees mbufs.
 */

#include <kernel/mbuf.h>
#include <kernel/module.h>

EXPORT_SYMBOL(m_get);
EXPORT_SYMBOL(m_getcl);
EXPORT_SYMBOL(m_free);
EXPORT_SYMBOL(m_freem);
EXPORT_SYMBOL(m_prepend);
EXPORT_SYMBOL(m_pullup);
EXPORT_SYMBOL(m_adj);
EXPORT_SYMBOL(m_copydata);
EXPORT_SYMBOL(m_append);
EXPORT_SYMBOL(m_length);
EXPORT_SYMBOL(m_copypacket);
