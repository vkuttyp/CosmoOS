/*
 * netif_floor.c - What the interface layer, now CeeSafe
 * (kernel-services/network/netif.cee), keeps in C: its exports to modules
 * (module ABI v1, docs/kernel/module/api.md). A network driver built as a
 * module registers its interface, hands it received packets and configures it;
 * a debug build's test module installs the worker hooks.
 */

#include <kernel/netif.h>
#include <kernel/module.h>

#if CONFIG_DEBUG
EXPORT_SYMBOL(netif_test_worker_hooks_set);
#endif
EXPORT_SYMBOL(netif_register);
EXPORT_SYMBOL(netif_find);   /* a driver picking the first free ethN */
EXPORT_SYMBOL(netif_unregister);
EXPORT_SYMBOL(netif_release_static);
EXPORT_SYMBOL(netif_rx);
EXPORT_SYMBOL(netif_rx_on);
EXPORT_SYMBOL(net_flow_hash);
EXPORT_SYMBOL(netif_set_ipv4);
EXPORT_SYMBOL(netif_set_up);
