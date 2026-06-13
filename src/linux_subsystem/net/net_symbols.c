#include "linux_subsystem/net/net_symbols.h"
#include "kobox/shim.h"

#include <stdint.h>

static const kb_linux_symbol_t symbols[] = {
    {"__rtnl_link_register", (void *)(uintptr_t)&kb_rtnl_link_register},
    {"__rtnl_link_unregister", (void *)(uintptr_t)&kb_rtnl_link_unregister},
    {"rtnl_link_unregister", (void *)(uintptr_t)&kb_rtnl_link_unregister},
    {"rtnl_lock", (void *)(uintptr_t)&kb_rtnl_lock},
    {"rtnl_unlock", (void *)(uintptr_t)&kb_rtnl_unlock},
    {"register_netdevice", (void *)(uintptr_t)&kb_register_netdevice},
    {"alloc_netdev_mqs", (void *)(uintptr_t)&kb_alloc_netdev_mqs},
    {"free_netdev", (void *)(uintptr_t)&kb_free_netdev},
    {"ether_setup", (void *)(uintptr_t)&kb_ether_setup},
    {"eth_mac_addr", (void *)(uintptr_t)&kb_eth_mac_addr},
    {"eth_validate_addr", (void *)(uintptr_t)&kb_eth_validate_addr},
    {"dev_addr_mod", (void *)(uintptr_t)&kb_dev_addr_mod},
    {"netif_carrier_on", (void *)(uintptr_t)&kb_netif_carrier_on},
    {"netif_carrier_off", (void *)(uintptr_t)&kb_netif_carrier_off},
    {"consume_skb", (void *)(uintptr_t)&kb_consume_skb},
    {"skb_tstamp_tx", (void *)(uintptr_t)&kb_skb_tstamp_tx},
    {"skb_clone_tx_timestamp", (void *)(uintptr_t)&kb_skb_clone_tx_timestamp},
    {"dev_lstats_read", (void *)(uintptr_t)&kb_dev_lstats_read},
    {"ethtool_op_get_ts_info", (void *)(uintptr_t)&kb_ethtool_op_get_ts_info},
};

const kb_linux_symbol_t *kb_linux_net_symbols(size_t *out_count)
{
    if (out_count != NULL) {
        *out_count = sizeof(symbols) / sizeof(symbols[0]);
    }
    return symbols;
}
