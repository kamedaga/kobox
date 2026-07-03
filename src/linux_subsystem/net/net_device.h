#pragma once

#include <stddef.h>

typedef void (*kb_net_rx_frame_callback_t)(void *ctx, void *dev, const void *frame, size_t frame_len);

void *kb_net_device_alloc(
    int sizeof_priv,
    const char *name,
    unsigned char name_assign_type,
    void (*setup)(void *),
    unsigned int txqs,
    unsigned int rxqs);
void kb_net_device_free(void *dev);
int kb_net_device_register(void *dev);
int kb_net_device_open(void *dev);
int kb_net_device_xmit_smoke(void *dev);
int kb_net_device_xmit_internet_smoke(void *dev);
void kb_net_device_run_pending_smokes(void);
void kb_net_device_set_rx_frame_callback(kb_net_rx_frame_callback_t callback, void *ctx);
int kb_net_device_tx_frame(const void *frame, size_t frame_len);
void kb_net_device_poll(void);
void kb_net_device_set_carrier(void *dev, int carrier_on);
void kb_net_device_addr_mod(void *dev, unsigned int offset, const void *addr, size_t len);
void *kb_net_device_ops(void *dev);
void kb_netif_napi_add(void *dev, void *napi, void *poll, int weight);
void kb_napi_disable(void *napi);
int kb_napi_schedule_prep(void *napi);
int kb_napi_gro_receive(void *napi, void *skb);
void kb_net_device_poll_rx_smoke(void);
void *kb_netdev_alloc_skb(void *dev, unsigned int length, unsigned int gfp);
void *kb_alloc_skb(unsigned int length, unsigned int gfp, int flags, int node);
void *kb_napi_alloc_skb(void *napi, unsigned int length, unsigned int gfp);
void *kb_skb_put(void *skb, unsigned int len);
int kb_skb_to_sgvec(void *skb, void *sg, int offset, int len);
