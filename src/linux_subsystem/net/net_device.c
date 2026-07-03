#include "linux_subsystem/net/net_device.h"
#include "kobox/shim.h"
#include "linux_subsystem/kvm/kvm_symbols.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    KB_NETDEV_TRACKED_MAX = 32,
    KB_NETDEV_ADDR_MAX = 32,
    KB_RHEL_3_10_NETDEV_OPS_OFFSET = 0x198,
    KB_LEGACY_NETDEV_OPS_OFFSET = 0x8,
    KB_LINUX_6_8_NETDEV_TX_QUEUE_OFFSET = 0x18,
    KB_RHEL_3_10_NETDEV_TX_QUEUE_OFFSET = 0x880,
    KB_NETDEV_QUEUE_SIZE = 0x200,
    KB_LINUX_6_8_NETDEV_DEV_ADDR_OFFSET = 0x430,
    KB_RHEL_3_10_NAPI_STATE_OFFSET = 0x10,
    KB_RHEL_3_10_NAPI_POLL_OFFSET = 0x18,
    KB_RHEL_3_10_NAPI_WEIGHT_OFFSET = 0x20,
    KB_NAPI_STATE_SCHED = 0,
    KB_RHEL_3_10_SKB_HEADROOM = 256,
    KB_RHEL_3_10_SKB_SIZE = 512,
    KB_RHEL_3_10_SKB_HEAD_OFFSET = 0xe0,
    KB_RHEL_3_10_SKB_DATA_OFFSET = 0xe8,
    KB_RHEL_3_10_SKB_TAIL_OFFSET = 0xf0,
    KB_RHEL_3_10_SKB_END_OFFSET = 0xf4,
    KB_RHEL_3_10_SKB_LEN_OFFSET = 0x68,
    KB_RHEL_3_10_SKB_DATA_LEN_OFFSET = 0x6c,
    KB_RHEL_3_10_SKB_QUEUE_MAPPING_OFFSET = 0xa8,
    KB_RHEL_3_10_SKB_IP_SUMMED_OFFSET = 0xab,
    KB_RHEL_3_10_SKB_END_POINTER_OFFSET = 0xdc,
    KB_RHEL_3_10_SG_PAGE_LINK_OFFSET = 0x0,
    KB_RHEL_3_10_SG_OFFSET_OFFSET = 0x8,
    KB_RHEL_3_10_SG_LENGTH_OFFSET = 0xc,
    KB_RHEL_3_10_SKB_SHARED_INFO_NR_FRAGS_OFFSET = 0x0,
    KB_RHEL_3_10_SKB_SHARED_INFO_FRAGS_OFFSET = 0x30,
    KB_RHEL_3_10_SKB_FRAG_SIZE = 0x10,
    KB_RHEL_3_10_SKB_FRAG_PAGE_OFFSET = 0x0,
    KB_RHEL_3_10_SKB_FRAG_PAGE_OFFSET_OFFSET = 0x8,
    KB_RHEL_3_10_SKB_FRAG_SIZE_OFFSET = 0xc,
    KB_LINUX_6_8_SKB_DEV_OFFSET = 0x10,
    KB_LINUX_6_8_SKB_LEN_OFFSET = 0x70,
    KB_LINUX_6_8_SKB_DATA_LEN_OFFSET = 0x74,
    KB_LINUX_6_8_SKB_QUEUE_MAPPING_OFFSET = 0x7c,
    KB_LINUX_6_8_SKB_MAC_HEADER_OFFSET = 0xba,
    KB_LINUX_6_8_SKB_TAIL_OFFSET = 0xbc,
    KB_LINUX_6_8_SKB_END_OFFSET = 0xc0,
    KB_LINUX_6_8_SKB_HEAD_OFFSET = 0xc8,
    KB_LINUX_6_8_SKB_DATA_OFFSET = 0xd0,
    KB_KVM_PAGE_SIZE = 4096,
    KB_KVM_STRUCT_PAGE_SIZE = 64,
    KB_VIRTIO_LEGACY_QUEUE_SIZE = 256,
};

typedef int (*kb_netdev_ndo_init_t)(void *dev);
typedef int (*kb_netdev_ndo_open_t)(void *dev);
typedef int (*kb_netdev_ndo_start_xmit_t)(void *skb, void *dev);
typedef int (*kb_napi_poll_t)(void *napi, int budget);

typedef struct kb_netdev_record {
    void *dev;
    size_t size;
    void *tx_queues;
    unsigned int txq_count;
    unsigned int rxq_count;
    unsigned char dev_addr[KB_NETDEV_ADDR_MAX];
    int registered;
    int opened;
    int carrier_on;
    int smoke_done;
    int internet_arp_reply_pending;
    int internet_arp_reply_sent;
    int internet_arp_request_seen;
    unsigned char internet_arp_peer_mac[6];
    unsigned char internet_arp_peer_ip[4];
} kb_netdev_record_t;

typedef struct kb_skb_record {
    void *skb;
    void *page;
    void *payload;
    uint64_t dma_handle;
    size_t dma_size;
    unsigned int capacity;
    struct kb_skb_record *next;
} kb_skb_record_t;

typedef struct kb_napi_record {
    void *dev;
    void *napi;
    void *poll;
    int weight;
    unsigned int poll_count;
    unsigned int rx_count;
    struct kb_napi_record *next;
} kb_napi_record_t;

static kb_netdev_record_t netdev_records[KB_NETDEV_TRACKED_MAX];
static kb_skb_record_t *skb_records;
static kb_napi_record_t *napi_records;
static kb_net_rx_frame_callback_t rx_frame_callback;
static void *rx_frame_callback_ctx;

static uint64_t net_metric_read_tsc(void)
{
    uint32_t lo = 0;
    uint32_t hi = 0;
    __asm__ __volatile__("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
}

static int metric_net_enabled(void)
{
    const char *value = getenv("KOBOX_NET_METRIC");
    return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
}

static void net_metric(const char *stage, uint64_t start_cycles)
{
    if (!metric_net_enabled() || stage == NULL || start_cycles == 0) {
        return;
    }
    uint64_t end_cycles = net_metric_read_tsc();
    if (end_cycles < start_cycles) {
        return;
    }
    printf("[kobox-net] metric stage=%s cycles=%llu\n",
        stage,
        (unsigned long long)(end_cycles - start_cycles));
}

static int trace_net_enabled(void)
{
    const char *value = getenv("KOBOX_TRACE_NET");
    return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
}

static int auto_open_enabled(void)
{
    const char *value = getenv("KOBOX_NET_AUTO_OPEN");
    return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
}

static int tx_smoke_enabled(void)
{
    const char *value = getenv("KOBOX_NET_TX_SMOKE");
    return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
}

static int tx_smoke_wants_udp(void)
{
    const char *value = getenv("KOBOX_NET_TX_SMOKE");
    return value != NULL && (strcmp(value, "udp") == 0 || strcmp(value, "both") == 0 || strcmp(value, "internet") == 0);
}

static int tx_smoke_wants_arp(void)
{
    const char *value = getenv("KOBOX_NET_TX_SMOKE");
    return value == NULL || value[0] == '\0' || strcmp(value, "1") == 0 || strcmp(value, "arp") == 0 ||
        strcmp(value, "both") == 0;
}

static int rx_poll_smoke_enabled(void)
{
    const char *value = getenv("KOBOX_NET_RX_POLL_SMOKE");
    return value == NULL || value[0] == '\0' || strcmp(value, "0") != 0;
}

static int call_module_xmit(void *xmit_ptr, void *skb, void *dev)
{
    if (xmit_ptr == NULL) {
        return 0;
    }
    unsigned long old_gs = 0;
    unsigned long kernel_gs = kb_module_kernel_gs_for_address(xmit_ptr);
    int has_gs = kernel_gs != 0 && kb_shim_enter_kernel_gs(kernel_gs, &old_gs) == 0;
    kb_netdev_ndo_start_xmit_t ndo_start_xmit = NULL;
    memcpy(&ndo_start_xmit, &xmit_ptr, sizeof(ndo_start_xmit));
    int result = ndo_start_xmit(skb, dev);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    return result;
}

static kb_netdev_record_t *find_netdev_record(void *dev)
{
    if (dev == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < KB_NETDEV_TRACKED_MAX; i++) {
        if (netdev_records[i].dev == dev) {
            return &netdev_records[i];
        }
    }
    return NULL;
}

static void track_netdev(void *dev, size_t size, unsigned int txqs, unsigned int rxqs)
{
    if (dev == NULL) {
        return;
    }
    for (size_t i = 0; i < KB_NETDEV_TRACKED_MAX; i++) {
        if (netdev_records[i].dev == NULL) {
            netdev_records[i].dev = dev;
            netdev_records[i].size = size;
            netdev_records[i].txq_count = txqs;
            netdev_records[i].rxq_count = rxqs;
            return;
        }
    }
}

static void untrack_netdev(void *dev)
{
    kb_netdev_record_t *record = find_netdev_record(dev);
    if (record != NULL) {
        free(record->tx_queues);
        memset(record, 0, sizeof(*record));
    }
}

static void *read_ptr_at(const void *base, size_t size, size_t offset)
{
    void *ptr = NULL;
    if (base != NULL && size >= offset + sizeof(ptr)) {
        memcpy(&ptr, (const unsigned char *)base + offset, sizeof(ptr));
    }
    return ptr;
}

static void write_ptr_at(void *base, size_t offset, const void *ptr)
{
    memcpy((unsigned char *)base + offset, &ptr, sizeof(ptr));
}

static void write_u32_at(void *base, size_t offset, uint32_t value)
{
    memcpy((unsigned char *)base + offset, &value, sizeof(value));
}

static void write_u16_at(void *base, size_t offset, uint16_t value)
{
    memcpy((unsigned char *)base + offset, &value, sizeof(value));
}

static void write_u8_at(void *base, size_t offset, uint8_t value)
{
    memcpy((unsigned char *)base + offset, &value, sizeof(value));
}

static uint16_t read_u16_at(const void *base, size_t offset)
{
    uint16_t value = 0;
    memcpy(&value, (const unsigned char *)base + offset, sizeof(value));
    return value;
}

static uint32_t read_u32_at(const void *base, size_t offset)
{
    uint32_t value = 0;
    memcpy(&value, (const unsigned char *)base + offset, sizeof(value));
    return value;
}

static uint64_t read_u64_at(const void *base, size_t offset)
{
    uint64_t value = 0;
    memcpy(&value, (const unsigned char *)base + offset, sizeof(value));
    return value;
}

static uint16_t read_be16(const unsigned char *bytes)
{
    return ((uint16_t)bytes[0] << 8) | bytes[1];
}

static uint32_t read_be32(const unsigned char *bytes)
{
    return ((uint32_t)bytes[0] << 24) |
        ((uint32_t)bytes[1] << 16) |
        ((uint32_t)bytes[2] << 8) |
        bytes[3];
}

static void *record_netdev_ops(const kb_netdev_record_t *record)
{
    if (record == NULL) {
        return NULL;
    }

    void *ops = read_ptr_at(record->dev, record->size, KB_RHEL_3_10_NETDEV_OPS_OFFSET);
    if (ops != NULL) {
        return ops;
    }
    return read_ptr_at(record->dev, record->size, KB_LEGACY_NETDEV_OPS_OFFSET);
}

static void *record_ndo_start_xmit(const kb_netdev_record_t *record)
{
    void *ops = record_netdev_ops(record);
    return read_ptr_at(ops, ops != NULL ? 4096 : 0, sizeof(void *) * 4);
}

static kb_netdev_record_t *first_opened_netdev_record(void)
{
    for (size_t i = 0; i < KB_NETDEV_TRACKED_MAX; i++) {
        kb_netdev_record_t *record = &netdev_records[i];
        if (record->dev != NULL && record->registered && record->opened) {
            return record;
        }
    }
    return NULL;
}

static void *payload_for_kvm_page(void *page)
{
    uintptr_t vmemmap = kb_linux_kvm_vmemmap_base();
    uintptr_t page_offset_base = kb_linux_kvm_page_offset_base();
    uintptr_t page_addr = (uintptr_t)page;
    if (page == NULL || vmemmap == 0 || page_offset_base == 0 || page_addr < vmemmap) {
        return NULL;
    }

    uintptr_t page_index = (page_addr - vmemmap) / KB_KVM_STRUCT_PAGE_SIZE;
    return (void *)(page_offset_base + page_index * KB_KVM_PAGE_SIZE);
}

static uint64_t dma_addr_for_kvm_page(void *page)
{
    uintptr_t vmemmap = kb_linux_kvm_vmemmap_base();
    uintptr_t page_addr = (uintptr_t)page;
    if (page == NULL || vmemmap == 0 || page_addr < vmemmap) {
        return 0;
    }

    uint64_t page_index = (uint64_t)((page_addr - vmemmap) / KB_KVM_STRUCT_PAGE_SIZE);
    return page_index * KB_KVM_PAGE_SIZE;
}

static kb_skb_record_t *find_skb_record(void *skb)
{
    for (kb_skb_record_t *record = skb_records; record != NULL; record = record->next) {
        if (record->skb == skb) {
            return record;
        }
    }
    return NULL;
}

static kb_napi_record_t *find_napi_record(void *napi)
{
    for (kb_napi_record_t *record = napi_records; record != NULL; record = record->next) {
        if (record->napi == napi) {
            return record;
        }
    }
    return NULL;
}

static const unsigned char *skb_packet_bytes(void *skb, uint32_t *out_len)
{
    kb_skb_record_t *record = find_skb_record(skb);
    if (out_len != NULL) {
        *out_len = 0;
    }
    if (record == NULL) {
        return NULL;
    }

    void *data = read_ptr_at(skb, KB_RHEL_3_10_SKB_SIZE, KB_RHEL_3_10_SKB_DATA_OFFSET);
    uint32_t len = read_u32_at(skb, KB_RHEL_3_10_SKB_LEN_OFFSET);
    if (data == NULL || (uintptr_t)data < (uintptr_t)record->payload) {
        return NULL;
    }
    uintptr_t offset = (uintptr_t)data - (uintptr_t)record->payload;
    if (offset >= record->capacity) {
        return NULL;
    }
    uint32_t available = record->capacity - (uint32_t)offset;
    if (len > available) {
        len = available;
    }
    if (out_len != NULL) {
        *out_len = len;
    }
    return (const unsigned char *)data;
}

static const unsigned char *skb_first_frag_bytes(void *skb, uint32_t *out_len)
{
    if (out_len != NULL) {
        *out_len = 0;
    }
    if (skb == NULL) {
        return NULL;
    }

    void *head = read_ptr_at(skb, KB_RHEL_3_10_SKB_SIZE, KB_RHEL_3_10_SKB_HEAD_OFFSET);
    uint32_t end = read_u32_at(skb, KB_RHEL_3_10_SKB_END_POINTER_OFFSET);
    if (head == NULL || end > KB_KVM_PAGE_SIZE - KB_RHEL_3_10_SKB_SHARED_INFO_FRAGS_OFFSET) {
        return NULL;
    }

    const unsigned char *shinfo = (const unsigned char *)head + end;
    unsigned int nr_frags = shinfo[KB_RHEL_3_10_SKB_SHARED_INFO_NR_FRAGS_OFFSET];
    if (nr_frags == 0) {
        if (trace_net_enabled()) {
            fprintf(stderr, "kobox net: skb_frag none skb=%p head=%p end=%u\n", skb, head, end);
        }
        return NULL;
    }

    const unsigned char *frag = shinfo + KB_RHEL_3_10_SKB_SHARED_INFO_FRAGS_OFFSET;
    void *page = read_ptr_at(frag, KB_RHEL_3_10_SKB_FRAG_SIZE, KB_RHEL_3_10_SKB_FRAG_PAGE_OFFSET);
    uint32_t page_offset = read_u32_at(frag, KB_RHEL_3_10_SKB_FRAG_PAGE_OFFSET_OFFSET);
    uint32_t size = read_u32_at(frag, KB_RHEL_3_10_SKB_FRAG_SIZE_OFFSET);
    void *payload = payload_for_kvm_page((void *)((uintptr_t)page & ~(uintptr_t)0x3f));
    if (trace_net_enabled()) {
        fprintf(stderr,
            "kobox net: skb_frag skb=%p head=%p end=%u nr=%u page=%p page_offset=%u size=%u payload=%p\n",
            skb,
            head,
            end,
            nr_frags,
            page,
            page_offset,
            size,
            payload);
    }
    if (payload == NULL || page_offset >= KB_KVM_PAGE_SIZE) {
        return NULL;
    }

    uint32_t available = KB_KVM_PAGE_SIZE - page_offset;
    if (size > available) {
        size = available;
    }
    if (out_len != NULL) {
        *out_len = size;
    }
    return (const unsigned char *)payload + page_offset;
}

static const unsigned char *find_l2_frame_in_first_frag_page(void *skb, uint32_t *out_len, uint32_t *out_offset)
{
    if (out_len != NULL) {
        *out_len = 0;
    }
    if (out_offset != NULL) {
        *out_offset = 0;
    }
    if (skb == NULL) {
        return NULL;
    }

    void *head = read_ptr_at(skb, KB_RHEL_3_10_SKB_SIZE, KB_RHEL_3_10_SKB_HEAD_OFFSET);
    uint32_t end = read_u32_at(skb, KB_RHEL_3_10_SKB_END_POINTER_OFFSET);
    if (head == NULL || end > KB_KVM_PAGE_SIZE - KB_RHEL_3_10_SKB_SHARED_INFO_FRAGS_OFFSET) {
        return NULL;
    }

    const unsigned char *shinfo = (const unsigned char *)head + end;
    if (shinfo[KB_RHEL_3_10_SKB_SHARED_INFO_NR_FRAGS_OFFSET] == 0) {
        return NULL;
    }

    const unsigned char *frag = shinfo + KB_RHEL_3_10_SKB_SHARED_INFO_FRAGS_OFFSET;
    void *page = read_ptr_at(frag, KB_RHEL_3_10_SKB_FRAG_SIZE, KB_RHEL_3_10_SKB_FRAG_PAGE_OFFSET);
    const unsigned char *payload = payload_for_kvm_page((void *)((uintptr_t)page & ~(uintptr_t)0x3f));
    if (payload == NULL) {
        return NULL;
    }

    for (uint32_t offset = 0; offset + 14u <= KB_KVM_PAGE_SIZE; offset++) {
        uint16_t ethertype = read_be16(payload + offset + 12u);
        if (ethertype != 0x0800 && ethertype != 0x0806) {
            continue;
        }
        uint32_t frame_len = KB_KVM_PAGE_SIZE - offset;
        if (ethertype == 0x0806 && frame_len > 42u) {
            frame_len = 42u;
        } else if (ethertype == 0x0800 && frame_len >= 34u) {
            uint16_t ip_total = read_be16(payload + offset + 16u);
            if (ip_total >= 20u && frame_len > 14u + ip_total) {
                frame_len = 14u + ip_total;
            }
        }
        if (out_len != NULL) {
            *out_len = frame_len;
        }
        if (out_offset != NULL) {
            *out_offset = offset;
        }
        return payload + offset;
    }
    return NULL;
}

static const unsigned char *find_l2_frame_in_skb(void *skb, uint32_t *out_len, uint32_t *out_offset)
{
    kb_skb_record_t *record = find_skb_record(skb);
    if (out_len != NULL) {
        *out_len = 0;
    }
    if (out_offset != NULL) {
        *out_offset = 0;
    }
    if (record == NULL || record->payload == NULL || record->capacity < 14) {
        return NULL;
    }

    const unsigned char *payload = record->payload;
    for (uint32_t offset = 0; offset + 14u <= record->capacity; offset++) {
        uint16_t ethertype = read_be16(payload + offset + 12u);
        if (ethertype != 0x0800 && ethertype != 0x0806) {
            continue;
        }
        if (out_len != NULL) {
            *out_len = record->capacity - offset;
        }
        if (out_offset != NULL) {
            *out_offset = offset;
        }
        return payload + offset;
    }
    uint32_t frag_len = 0;
    const unsigned char *frag = skb_first_frag_bytes(skb, &frag_len);
    if (frag != NULL && frag_len >= 14) {
        for (uint32_t offset = 0; offset + 14u <= frag_len; offset++) {
            uint16_t ethertype = read_be16(frag + offset + 12u);
            if (ethertype != 0x0800 && ethertype != 0x0806) {
                continue;
            }
            if (out_len != NULL) {
                *out_len = frag_len - offset;
            }
            if (out_offset != NULL) {
                *out_offset = offset;
            }
            return frag + offset;
        }
    }
    return find_l2_frame_in_first_frag_page(skb, out_len, out_offset);
}

static void observe_arp_request(kb_netdev_record_t *record, const unsigned char *packet, uint32_t len)
{
    static const unsigned char local_ip[4] = {10, 0, 2, 15};
    if (record == NULL || packet == NULL || len < 42 || read_be16(packet + 12) != 0x0806) {
        return;
    }
    if (read_be16(packet + 14) != 0x0001 ||
        read_be16(packet + 16) != 0x0800 ||
        packet[18] != 6 ||
        packet[19] != 4 ||
        read_be16(packet + 20) != 0x0001 ||
        memcmp(packet + 38, local_ip, sizeof(local_ip)) != 0)
    {
        return;
    }

    memcpy(record->internet_arp_peer_mac, packet + 22, sizeof(record->internet_arp_peer_mac));
    memcpy(record->internet_arp_peer_ip, packet + 28, sizeof(record->internet_arp_peer_ip));
    record->internet_arp_request_seen = 1;
}

static void log_received_packet(kb_napi_record_t *napi_record, void *skb)
{
    uint32_t len = 0;
    const unsigned char *packet = skb_packet_bytes(skb, &len);
    uint32_t payload_offset = 0;
    if (packet == NULL || len < 14 || read_be16(packet + 12) == 0) {
        uint32_t scanned_len = 0;
        const unsigned char *scanned = find_l2_frame_in_skb(skb, &scanned_len, &payload_offset);
        if (scanned != NULL) {
            packet = scanned;
            len = scanned_len;
        }
    }
    if (packet == NULL || len < 14) {
        if (trace_net_enabled()) {
            fprintf(stderr,
                "kobox net: rx_packet napi=%p skb=%p len=%u type=unavailable\n",
                napi_record == NULL ? NULL : napi_record->napi,
                skb,
                len);
        }
        return;
    }

    uint32_t l2_offset = 0;
    uint16_t ethertype = read_be16(packet + 12);
    if ((ethertype == 0 || ethertype > 0x05ffu) && len >= 26) {
        uint16_t shifted = read_be16(packet + 24);
        if (shifted == 0x0800 || shifted == 0x0806) {
            l2_offset = 12;
            ethertype = shifted;
        }
    }
    packet += l2_offset;
    len -= l2_offset;
    observe_arp_request(find_netdev_record(napi_record == NULL ? NULL : napi_record->dev), packet, len);
    if (rx_frame_callback != NULL) {
        rx_frame_callback(rx_frame_callback_ctx, napi_record == NULL ? NULL : napi_record->dev, packet, len);
    }
    if (ethertype == 0x0806 && len >= 42) {
        uint16_t op = read_be16(packet + 20);
        uint32_t spa = read_be32(packet + 28);
        uint32_t tpa = read_be32(packet + 38);
        if (trace_net_enabled()) {
            fprintf(stderr,
                "kobox net: rx_packet napi=%p skb=%p len=%u payload_offset=%u l2_offset=%u type=arp op=%u spa=%u.%u.%u.%u tpa=%u.%u.%u.%u\n",
                napi_record == NULL ? NULL : napi_record->napi,
                skb,
                len,
                payload_offset,
                l2_offset,
                op,
                (spa >> 24) & 0xffu,
                (spa >> 16) & 0xffu,
                (spa >> 8) & 0xffu,
                spa & 0xffu,
                (tpa >> 24) & 0xffu,
                (tpa >> 16) & 0xffu,
                (tpa >> 8) & 0xffu,
                tpa & 0xffu);
        }
        return;
    }

    if (ethertype == 0x0800 && len >= 34) {
        const unsigned char *ip = packet + 14;
        uint8_t ihl = (uint8_t)((ip[0] & 0x0fu) * 4u);
        uint8_t proto = ip[9];
        uint32_t src = read_be32(ip + 12);
        uint32_t dst = read_be32(ip + 16);
        const char *proto_name = proto == 1 ? "icmp" : proto == 6 ? "tcp" : proto == 17 ? "udp" : "other";
        if (trace_net_enabled()) {
            fprintf(stderr,
                "kobox net: rx_packet napi=%p skb=%p len=%u payload_offset=%u l2_offset=%u type=ipv4 proto=%s src=%u.%u.%u.%u dst=%u.%u.%u.%u",
                napi_record == NULL ? NULL : napi_record->napi,
                skb,
                len,
                payload_offset,
                l2_offset,
                proto_name,
                (src >> 24) & 0xffu,
                (src >> 16) & 0xffu,
                (src >> 8) & 0xffu,
                src & 0xffu,
                (dst >> 24) & 0xffu,
                (dst >> 16) & 0xffu,
                (dst >> 8) & 0xffu,
                dst & 0xffu);
            if (proto == 17 && ihl >= 20 && len >= 14u + ihl + 8u) {
                const unsigned char *udp = ip + ihl;
                fprintf(stderr, " sport=%u dport=%u", read_be16(udp), read_be16(udp + 2));
            }
            fprintf(stderr, "\n");
        }
        return;
    }

    if (trace_net_enabled()) {
        fprintf(stderr,
            "kobox net: rx_packet napi=%p skb=%p len=%u payload_offset=%u l2_offset=%u type=ether ethertype=0x%04x bytes=",
            napi_record == NULL ? NULL : napi_record->napi,
            skb,
            len,
            payload_offset,
            l2_offset,
            ethertype);
        uint32_t dump_len = len < 48u ? len : 48u;
        for (uint32_t i = 0; i < dump_len; i++) {
            fprintf(stderr, "%02x", packet[i]);
            if (i + 1u < dump_len) {
                fprintf(stderr, ":");
            }
        }
        fprintf(stderr, "\n");
    }
}

static void dump_bytes_for_dma(uint64_t dma_addr, uint32_t len)
{
    size_t available = 0;
    const unsigned char *cpu = kb_dma_cpu_addr(dma_addr, &available);
    if (cpu == NULL || available == 0) {
        fprintf(stderr, " cpu=(unmapped)");
        return;
    }

    uint32_t dump_len = len < 32u ? len : 32u;
    if ((size_t)dump_len > available) {
        dump_len = (uint32_t)available;
    }
    fprintf(stderr, " bytes=");
    for (uint32_t i = 0; i < dump_len; i++) {
        fprintf(stderr, "%02x", cpu[i]);
        if (i + 1u < dump_len) {
            fprintf(stderr, ":");
        }
    }
}

static void dump_virtio_queue(unsigned int queue_index, uint64_t desc_dma, uint64_t avail_dma, uint64_t used_dma)
{
    size_t desc_available = 0;
    size_t avail_available = 0;
    size_t used_available = 0;
    const unsigned char *desc = kb_dma_cpu_addr(desc_dma, &desc_available);
    const unsigned char *avail = kb_dma_cpu_addr(avail_dma, &avail_available);
    const unsigned char *used = kb_dma_cpu_addr(used_dma, &used_available);
    if (desc == NULL || avail == NULL || desc_available < 16 || avail_available < 6) {
        fprintf(stderr,
            "kobox net: txdump q%u unavailable desc=0x%llx cpu=%p avail=0x%llx cpu=%p used=0x%llx cpu=%p\n",
            queue_index,
            (unsigned long long)desc_dma,
            (const void *)desc,
            (unsigned long long)avail_dma,
            (const void *)avail,
            (unsigned long long)used_dma,
            (const void *)used);
        return;
    }

    uint16_t avail_flags = read_u16_at(avail, 0);
    uint16_t avail_idx = read_u16_at(avail, 2);
    uint16_t last_head = read_u16_at(avail, 4 + ((avail_idx == 0 ? 0 : (avail_idx - 1u)) % KB_VIRTIO_LEGACY_QUEUE_SIZE) * 2u);
    uint16_t used_flags = 0;
    uint16_t used_idx = 0;
    if (used != NULL && used_available >= 4) {
        used_flags = read_u16_at(used, 0);
        used_idx = read_u16_at(used, 2);
    }
    fprintf(stderr,
        "kobox net: txdump q%u avail_flags=0x%04x avail_idx=%u last_head=%u used_flags=0x%04x used_idx=%u\n",
        queue_index,
        avail_flags,
        avail_idx,
        last_head,
        used_flags,
        used_idx);

    uint16_t head = last_head;
    for (unsigned int depth = 0; depth < 8; depth++) {
        if ((size_t)head * 16u + 16u > desc_available) {
            fprintf(stderr, "kobox net: txdump q%u desc[%u] out-of-range\n", queue_index, head);
            break;
        }
        const unsigned char *entry = desc + (size_t)head * 16u;
        uint64_t addr = read_u64_at(entry, 0);
        uint32_t len = read_u32_at(entry, 8);
        uint16_t flags = read_u16_at(entry, 12);
        uint16_t next = read_u16_at(entry, 14);
        fprintf(stderr,
            "kobox net: txdump q%u desc[%u] addr=0x%llx len=%u flags=0x%04x next=%u",
            queue_index,
            head,
            (unsigned long long)addr,
            len,
            flags,
            next);
        dump_bytes_for_dma(addr, len);
        fprintf(stderr, "\n");
        if ((flags & 0x1u) == 0) {
            break;
        }
        head = next;
    }
}

static void dump_virtio_legacy_tx_rings(void)
{
    if (!trace_net_enabled()) {
        return;
    }
    kb_virtio_modern_debug_dump_queues();
    dump_virtio_queue(0, 0x0000, 0x1000, 0x1240);
    dump_virtio_queue(1, 0x2000, 0x3000, 0x3240);
    dump_virtio_queue(2, 0x4000, 0x4400, 0x44c0);
}

static uint32_t read_bar_u32(volatile unsigned char *bar, size_t bar_size, size_t offset)
{
    uint32_t value = 0;
    if (bar == NULL || offset + sizeof(value) > bar_size) {
        return 0;
    }
    memcpy(&value, (const void *)(bar + offset), sizeof(value));
    return value;
}

static void sync_bar_u32_shadow(volatile unsigned char *bar, size_t bar_size, size_t offset)
{
    if (bar == NULL || offset + sizeof(uint32_t) > bar_size) {
        return;
    }
    uint32_t value = read_bar_u32(bar, bar_size, offset);
    int status = kb_pci_bar_write32(0, offset, value);
    if (trace_net_enabled()) {
        fprintf(
            stderr,
            "kobox net: e1000e mmio_sync offset=0x%zx value=0x%08x status=%d\n",
            offset,
            value,
            status);
    }
}

static void sync_bar_u32_value(volatile unsigned char *bar, size_t bar_size, size_t offset, uint32_t value)
{
    if (bar == NULL || offset + sizeof(uint32_t) > bar_size) {
        return;
    }
    int status = kb_pci_bar_write32(0, offset, value);
    if (trace_net_enabled()) {
        fprintf(
            stderr,
            "kobox net: e1000e mmio_sync offset=0x%zx value=0x%08x status=%d\n",
            offset,
            value,
            status);
    }
}

static void sync_e1000e_tx_shadow(void)
{
    const char *driver = getenv("KOBOX_NET_DRIVER");
    if (driver == NULL || strcmp(driver, "e1000e") != 0) {
        return;
    }

    volatile unsigned char *bar = NULL;
    size_t bar_size = 0;
    if (!kb_pci_bar0_snapshot(&bar, &bar_size)) {
        return;
    }

    enum {
        KB_E1000_CTRL = 0x0000,
        KB_E1000_TCTL = 0x0400,
        KB_E1000_TIPG = 0x0410,
        KB_E1000_TDBAL0 = 0x3800,
        KB_E1000_TDBAH0 = 0x3804,
        KB_E1000_TDLEN0 = 0x3808,
        KB_E1000_TDH0 = 0x3810,
        KB_E1000_TDT0 = 0x3818,
        KB_E1000_TXDCTL0 = 0x3828,
        KB_E1000_TARC0 = 0x3840,
        KB_E1000_TDBAL1 = 0xe000,
        KB_E1000_TDBAH1 = 0xe004,
        KB_E1000_TDLEN1 = 0xe008,
        KB_E1000_TDH1 = 0xe010,
        KB_E1000_TDT1 = 0xe018,
        KB_E1000_TXDCTL1 = 0xe028,
        KB_E1000_TARC1 = 0xe040,
        KB_E1000_TCTL_EN = 0x00000002,
        KB_E1000_TXDCTL_QUEUE_ENABLE = 0x02000000,
    };

    sync_bar_u32_shadow(bar, bar_size, KB_E1000_CTRL);
    sync_bar_u32_value(
        bar,
        bar_size,
        KB_E1000_TCTL,
        read_bar_u32(bar, bar_size, KB_E1000_TCTL) | KB_E1000_TCTL_EN);
    sync_bar_u32_shadow(bar, bar_size, KB_E1000_TIPG);
    sync_bar_u32_shadow(bar, bar_size, KB_E1000_TDBAL0);
    sync_bar_u32_shadow(bar, bar_size, KB_E1000_TDBAH0);
    sync_bar_u32_shadow(bar, bar_size, KB_E1000_TDLEN0);
    sync_bar_u32_shadow(bar, bar_size, KB_E1000_TDH0);
    sync_bar_u32_value(
        bar,
        bar_size,
        KB_E1000_TXDCTL0,
        read_bar_u32(bar, bar_size, KB_E1000_TXDCTL0) | KB_E1000_TXDCTL_QUEUE_ENABLE);
    sync_bar_u32_shadow(bar, bar_size, KB_E1000_TARC0);
    sync_bar_u32_shadow(bar, bar_size, KB_E1000_TDT0);
    sync_bar_u32_shadow(bar, bar_size, KB_E1000_TDBAL1);
    sync_bar_u32_shadow(bar, bar_size, KB_E1000_TDBAH1);
    sync_bar_u32_shadow(bar, bar_size, KB_E1000_TDLEN1);
    sync_bar_u32_shadow(bar, bar_size, KB_E1000_TDH1);
    sync_bar_u32_value(
        bar,
        bar_size,
        KB_E1000_TXDCTL1,
        read_bar_u32(bar, bar_size, KB_E1000_TXDCTL1) | KB_E1000_TXDCTL_QUEUE_ENABLE);
    sync_bar_u32_shadow(bar, bar_size, KB_E1000_TARC1);
    sync_bar_u32_shadow(bar, bar_size, KB_E1000_TDT1);
}

static void dump_e1000e_core_regs(volatile unsigned char *bar, size_t bar_size)
{
    if (bar == NULL || bar_size < 0x420) {
        return;
    }
    fprintf(
        stderr,
        "kobox net: e1000e regs ctrl=0x%08x status=0x%08x ctrl_ext=0x%08x icr=0x%08x ims=0x%08x rctl=0x%08x tctl=0x%08x tipg=0x%08x\n",
        read_bar_u32(bar, bar_size, 0x0000),
        read_bar_u32(bar, bar_size, 0x0008),
        read_bar_u32(bar, bar_size, 0x0018),
        read_bar_u32(bar, bar_size, 0x00c0),
        read_bar_u32(bar, bar_size, 0x00d0),
        read_bar_u32(bar, bar_size, 0x0100),
        read_bar_u32(bar, bar_size, 0x0400),
        read_bar_u32(bar, bar_size, 0x0410));
}

static void dump_e1000e_tx_ring_at(volatile unsigned char *bar, size_t bar_size, size_t base)
{
    enum {
        KB_E1000_TDBAL = 0x00,
        KB_E1000_TDBAH = 0x04,
        KB_E1000_TDLEN = 0x08,
        KB_E1000_TDH = 0x10,
        KB_E1000_TDT = 0x18,
        KB_E1000_TXDCTL = 0x28,
        KB_E1000_TX_DESC_SIZE = 16,
    };

    if (base + KB_E1000_TXDCTL + sizeof(uint32_t) > bar_size) {
        return;
    }

    uint64_t ring_dma =
        ((uint64_t)read_bar_u32(bar, bar_size, base + KB_E1000_TDBAH) << 32) |
        read_bar_u32(bar, bar_size, base + KB_E1000_TDBAL);
    uint32_t ring_len = read_bar_u32(bar, bar_size, base + KB_E1000_TDLEN);
    uint32_t head = read_bar_u32(bar, bar_size, base + KB_E1000_TDH);
    uint32_t tail = read_bar_u32(bar, bar_size, base + KB_E1000_TDT);
    uint32_t txdctl = read_bar_u32(bar, bar_size, base + KB_E1000_TXDCTL);

    if (ring_dma == 0 && ring_len == 0 && head == 0 && tail == 0 && txdctl == 0) {
        return;
    }

    size_t available = 0;
    const unsigned char *ring = kb_dma_cpu_addr(ring_dma, &available);
    fprintf(stderr,
        "kobox net: e1000e txregs base=0x%zx ring=0x%llx len=%u head=%u tail=%u txdctl=0x%08x cpu=%p avail=%zu\n",
        base,
        (unsigned long long)ring_dma,
        ring_len,
        head,
        tail,
        txdctl,
        (const void *)ring,
        available);

    if (ring == NULL || available < KB_E1000_TX_DESC_SIZE || ring_len < KB_E1000_TX_DESC_SIZE) {
        return;
    }

    uint32_t desc_count = ring_len / KB_E1000_TX_DESC_SIZE;
    if (desc_count > available / KB_E1000_TX_DESC_SIZE) {
        desc_count = (uint32_t)(available / KB_E1000_TX_DESC_SIZE);
    }
    if (desc_count > 8u) {
        desc_count = 8u;
    }
    for (uint32_t i = 0; i < desc_count; i++) {
        const unsigned char *desc = ring + (size_t)i * KB_E1000_TX_DESC_SIZE;
        uint64_t buf = read_u64_at(desc, 0);
        uint32_t lower = read_u32_at(desc, 8);
        uint32_t upper = read_u32_at(desc, 12);
        uint32_t len = lower & 0x000fffffu;
        fprintf(stderr,
            "kobox net: e1000e txdesc[%u] buf=0x%llx lower=0x%08x upper=0x%08x len=%u",
            i,
            (unsigned long long)buf,
            lower,
            upper,
            len);
        if (buf != 0 && len != 0) {
            dump_bytes_for_dma(buf, len);
        }
        fprintf(stderr, "\n");
    }
}

static void dump_e1000e_tx_rings(void)
{
    if (!trace_net_enabled()) {
        return;
    }

    volatile unsigned char *bar = NULL;
    size_t bar_size = 0;
    if (!kb_pci_bar0_snapshot(&bar, &bar_size)) {
        fprintf(stderr, "kobox net: e1000e txregs unavailable bar0=%p size=%zu\n", (void *)bar, bar_size);
        return;
    }

    dump_e1000e_core_regs(bar, bar_size);
    dump_e1000e_tx_ring_at(bar, bar_size, 0x3800);
    dump_e1000e_tx_ring_at(bar, bar_size, 0xe000);
}

static int xmit_raw_frame(void *dev, void *xmit_ptr, const unsigned char *frame, size_t frame_len);
static size_t build_arp_reply_frame(
    unsigned char *frame,
    size_t frame_capacity,
    const unsigned char target_mac[6],
    const unsigned char target_ip[4]);

static void track_skb(void *skb, void *page, void *payload, uint64_t dma_handle, size_t dma_size, unsigned int capacity)
{
    if (skb == NULL || page == NULL || payload == NULL) {
        return;
    }
    kb_skb_record_t *record = kb_kmalloc(sizeof(*record), 0);
    if (record == NULL) {
        return;
    }
    record->skb = skb;
    record->page = page;
    record->payload = payload;
    record->dma_handle = dma_handle;
    record->dma_size = dma_size;
    record->capacity = capacity;
    record->next = skb_records;
    skb_records = record;
}

void *kb_net_device_alloc(
    int sizeof_priv,
    const char *name,
    unsigned char name_assign_type,
    void (*setup)(void *),
    unsigned int txqs,
    unsigned int rxqs)
{
    (void)name_assign_type;
    size_t size = sizeof_priv < 0 ? 65536 : (size_t)sizeof_priv + 65536;
    void *dev = calloc(1, size);
    unsigned int queue_count = txqs == 0 ? 1 : txqs;
    void *tx_queues = calloc(queue_count, KB_NETDEV_QUEUE_SIZE);

    track_netdev(dev, size, txqs, rxqs);
    kb_netdev_record_t *record = find_netdev_record(dev);
    if (record != NULL) {
        record->tx_queues = tx_queues;
        static const unsigned char default_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
        memcpy(record->dev_addr, default_mac, sizeof(default_mac));
        if (size >= KB_LINUX_6_8_NETDEV_DEV_ADDR_OFFSET + sizeof(void *)) {
            write_ptr_at(dev, KB_LINUX_6_8_NETDEV_DEV_ADDR_OFFSET, record->dev_addr);
        }
    }
    if (dev != NULL && tx_queues != NULL && size >= KB_RHEL_3_10_NETDEV_TX_QUEUE_OFFSET + sizeof(tx_queues)) {
        memcpy((unsigned char *)dev + KB_RHEL_3_10_NETDEV_TX_QUEUE_OFFSET, &tx_queues, sizeof(tx_queues));
    }
    if (dev != NULL && tx_queues != NULL && size >= KB_LINUX_6_8_NETDEV_TX_QUEUE_OFFSET + sizeof(tx_queues)) {
        memcpy((unsigned char *)dev + KB_LINUX_6_8_NETDEV_TX_QUEUE_OFFSET, &tx_queues, sizeof(tx_queues));
    }

    if (trace_net_enabled()) {
        fprintf(
            stderr,
            "kobox net: alloc_netdev_mqs priv=%d name=%s txqs=%u rxqs=%u dev=%p size=%zu tx_queues=%p setup=%p\n",
            sizeof_priv,
            name != NULL ? name : "",
            txqs,
            rxqs,
            dev,
            size,
            tx_queues,
            (void *)setup);
    }
    if (dev != NULL && setup != NULL) {
        setup(dev);
    }
    return dev;
}

void kb_net_device_free(void *dev)
{
    if (trace_net_enabled()) {
        fprintf(stderr, "kobox net: free_netdev dev=%p\n", dev);
    }
    untrack_netdev(dev);
    free(dev);
}

void *kb_net_device_ops(void *dev)
{
    return record_netdev_ops(find_netdev_record(dev));
}

int kb_net_device_register(void *dev)
{
    const uint64_t total_start_cycles = net_metric_read_tsc();
    kb_netdev_record_t *record = find_netdev_record(dev);
    if (record == NULL) {
        if (trace_net_enabled()) {
            fprintf(stderr, "kobox net: register_netdevice ignored dev=%p record=%p\n", dev, (void *)record);
        }
        return 0;
    }

    void *ops = record_netdev_ops(record);
    void *ndo_init_ptr = read_ptr_at(ops, ops != NULL ? 4096 : 0, 0);
    void *ndo_open_ptr = read_ptr_at(ops, ops != NULL ? 4096 : 0, sizeof(void *) * 2);
    void *ndo_xmit_ptr = read_ptr_at(ops, ops != NULL ? 4096 : 0, sizeof(void *) * 4);
    if (trace_net_enabled()) {
        fprintf(
            stderr,
            "kobox net: register_netdevice dev=%p ops=%p ndo_init=%p ndo_open=%p ndo_start_xmit=%p\n",
            dev,
            ops,
            ndo_init_ptr,
            ndo_open_ptr,
            ndo_xmit_ptr);
    }
    if (ndo_init_ptr != NULL) {
        kb_netdev_ndo_init_t ndo_init = NULL;
        memcpy(&ndo_init, &ndo_init_ptr, sizeof(ndo_init));
        const uint64_t init_start_cycles = net_metric_read_tsc();
        int result = ndo_init(dev);
        net_metric("ndo_init", init_start_cycles);
        if (trace_net_enabled()) {
            fprintf(stderr, "kobox net: ndo_init dev=%p result=%d\n", dev, result);
        }
        if (result < 0) {
            return result;
        }
    }
    record->registered = 1;
    if (auto_open_enabled()) {
        const uint64_t open_start_cycles = net_metric_read_tsc();
        int result = kb_net_device_open(dev);
        net_metric("auto_open", open_start_cycles);
        net_metric("register_netdevice_total", total_start_cycles);
        return result;
    }
    net_metric("register_netdevice_total", total_start_cycles);
    return 0;
}

int kb_net_device_open(void *dev)
{
    const uint64_t total_start_cycles = net_metric_read_tsc();
    kb_netdev_record_t *record = find_netdev_record(dev);
    if (record == NULL) {
        if (trace_net_enabled()) {
            fprintf(stderr, "kobox net: dev_open ignored dev=%p\n", dev);
        }
        return 0;
    }

    void *ops = record_netdev_ops(record);
    void *ndo_open_ptr = read_ptr_at(ops, ops != NULL ? 4096 : 0, sizeof(void *) * 2);
    if (trace_net_enabled()) {
        fprintf(stderr, "kobox net: dev_open dev=%p ops=%p ndo_open=%p\n", dev, ops, ndo_open_ptr);
    }
    if (ndo_open_ptr == NULL) {
        record->opened = 1;
        return 0;
    }

    kb_netdev_ndo_open_t ndo_open = NULL;
    memcpy(&ndo_open, &ndo_open_ptr, sizeof(ndo_open));
    const uint64_t open_start_cycles = net_metric_read_tsc();
    int result = ndo_open(dev);
    net_metric("ndo_open", open_start_cycles);
    if (trace_net_enabled()) {
        fprintf(stderr, "kobox net: ndo_open dev=%p result=%d\n", dev, result);
    }
    if (result == 0) {
        record->opened = 1;
    }
    net_metric("dev_open_total", total_start_cycles);
    return result;
}

void *kb_netdev_alloc_skb(void *dev, unsigned int length, unsigned int gfp)
{
    unsigned int capacity = length + KB_RHEL_3_10_SKB_HEADROOM;
    if (capacity > KB_KVM_PAGE_SIZE) {
        if (trace_net_enabled()) {
            fprintf(stderr, "kobox net: alloc_skb too large length=%u capacity=%u\n", length, capacity);
        }
        return NULL;
    }

    void *skb = kb_alloc_pages_exact(KB_KVM_PAGE_SIZE, gfp);
    void *page = kb_kvm_alloc_pages_stub(gfp, 0);
    void *payload = payload_for_kvm_page(page);
    uint64_t dma_handle = dma_addr_for_kvm_page(page);
    if (skb == NULL || page == NULL || payload == NULL) {
        if (trace_net_enabled()) {
            fprintf(stderr,
                "kobox net: alloc_skb failed length=%u skb=%p payload=%p page=%p dma=0x%llx\n",
                length,
                skb,
                payload,
                page,
                (unsigned long long)dma_handle);
        }
        return NULL;
    }

    memset(skb, 0, KB_RHEL_3_10_SKB_SIZE);
    memset(payload, 0, KB_KVM_PAGE_SIZE);
    void *data = (unsigned char *)payload + KB_RHEL_3_10_SKB_HEADROOM;
    write_ptr_at(skb, KB_RHEL_3_10_SKB_HEAD_OFFSET, payload);
    write_ptr_at(skb, KB_RHEL_3_10_SKB_DATA_OFFSET, data);
    write_u32_at(skb, KB_RHEL_3_10_SKB_END_POINTER_OFFSET, capacity);
    write_u32_at(skb, KB_RHEL_3_10_SKB_TAIL_OFFSET, KB_RHEL_3_10_SKB_HEADROOM);
    write_u32_at(skb, KB_RHEL_3_10_SKB_END_OFFSET, capacity);
    write_u32_at(skb, KB_RHEL_3_10_SKB_LEN_OFFSET, 0);
    write_u32_at(skb, KB_RHEL_3_10_SKB_DATA_LEN_OFFSET, 0);
    write_u16_at(skb, KB_RHEL_3_10_SKB_QUEUE_MAPPING_OFFSET, 0);
    write_u8_at(skb, KB_RHEL_3_10_SKB_IP_SUMMED_OFFSET, 0);
    write_ptr_at(skb, KB_LINUX_6_8_SKB_DEV_OFFSET, dev);
    write_u32_at(skb, KB_LINUX_6_8_SKB_LEN_OFFSET, 0);
    write_u32_at(skb, KB_LINUX_6_8_SKB_DATA_LEN_OFFSET, 0);
    write_u16_at(skb, KB_LINUX_6_8_SKB_QUEUE_MAPPING_OFFSET, 0);
    write_u16_at(skb, KB_LINUX_6_8_SKB_MAC_HEADER_OFFSET, 0);
    write_u32_at(skb, KB_LINUX_6_8_SKB_TAIL_OFFSET, KB_RHEL_3_10_SKB_HEADROOM);
    write_u32_at(skb, KB_LINUX_6_8_SKB_END_OFFSET, capacity);
    write_ptr_at(skb, KB_LINUX_6_8_SKB_HEAD_OFFSET, payload);
    write_ptr_at(skb, KB_LINUX_6_8_SKB_DATA_OFFSET, data);
    track_skb(skb, page, payload, dma_handle, KB_KVM_PAGE_SIZE, capacity);

    if (trace_net_enabled()) {
        fprintf(stderr,
            "kobox net: alloc_skb skb=%p payload=%p page=%p dma=0x%llx capacity=%u\n",
            skb,
            payload,
            page,
            (unsigned long long)dma_handle,
            capacity);
    }
    return skb;
}

void *kb_alloc_skb(unsigned int length, unsigned int gfp, int flags, int node)
{
    (void)flags;
    (void)node;
    return kb_netdev_alloc_skb(NULL, length, gfp);
}

void *kb_napi_alloc_skb(void *napi, unsigned int length, unsigned int gfp)
{
    kb_napi_record_t *record = find_napi_record(napi);
    return kb_netdev_alloc_skb(record == NULL ? NULL : record->dev, length, gfp);
}

void *kb_skb_put(void *skb, unsigned int len)
{
    kb_skb_record_t *record = find_skb_record(skb);
    if (record == NULL) {
        return skb;
    }

    void *data = read_ptr_at(skb, KB_RHEL_3_10_SKB_SIZE, KB_RHEL_3_10_SKB_DATA_OFFSET);
    uint32_t tail = 0;
    uint32_t old_len = 0;
    memcpy(&tail, (unsigned char *)skb + KB_RHEL_3_10_SKB_TAIL_OFFSET, sizeof(tail));
    memcpy(&old_len, (unsigned char *)skb + KB_RHEL_3_10_SKB_LEN_OFFSET, sizeof(old_len));
    if (data == NULL || tail + len > record->capacity) {
        return NULL;
    }
    void *old_tail = (unsigned char *)record->payload + tail;
    tail += len;
    old_len += len;
    write_u32_at(skb, KB_RHEL_3_10_SKB_TAIL_OFFSET, tail);
    write_u32_at(skb, KB_RHEL_3_10_SKB_LEN_OFFSET, old_len);
    write_u32_at(skb, KB_LINUX_6_8_SKB_TAIL_OFFSET, tail);
    write_u32_at(skb, KB_LINUX_6_8_SKB_LEN_OFFSET, old_len);
    return old_tail;
}

void *kb_pskb_pull_tail(void *skb, int delta)
{
    (void)delta;
    if (find_skb_record(skb) == NULL) {
        return NULL;
    }
    return read_ptr_at(skb, KB_RHEL_3_10_SKB_SIZE, KB_RHEL_3_10_SKB_DATA_OFFSET);
}

int kb_skb_copy_bits(void *skb, int offset, void *to, int len)
{
    kb_skb_record_t *record = find_skb_record(skb);
    if (record == NULL || to == NULL || offset < 0 || len < 0) {
        return -22;
    }
    void *data = read_ptr_at(skb, KB_RHEL_3_10_SKB_SIZE, KB_RHEL_3_10_SKB_DATA_OFFSET);
    uint32_t skb_len = read_u32_at(skb, KB_RHEL_3_10_SKB_LEN_OFFSET);
    if (data == NULL || (uintptr_t)data < (uintptr_t)record->payload || (uint32_t)offset > skb_len) {
        return -22;
    }
    uintptr_t data_offset = (uintptr_t)data - (uintptr_t)record->payload;
    uint32_t copy_len = (uint32_t)len;
    if (copy_len > skb_len - (uint32_t)offset || data_offset + (uint32_t)offset + copy_len > record->capacity) {
        return -22;
    }
    memcpy(to, (const unsigned char *)record->payload + data_offset + (uint32_t)offset, copy_len);
    return 0;
}

int kb_skb_to_sgvec(void *skb, void *sg, int offset, int len)
{
    kb_skb_record_t *record = find_skb_record(skb);
    if (record == NULL || sg == NULL || len < 0 || offset < 0) {
        return 0;
    }

    void *data = read_ptr_at(skb, KB_RHEL_3_10_SKB_SIZE, KB_RHEL_3_10_SKB_DATA_OFFSET);
    if (data == NULL) {
        return 0;
    }
    uintptr_t data_offset = (uintptr_t)data - (uintptr_t)record->payload + (uintptr_t)offset;
    if (data_offset >= record->capacity) {
        return 0;
    }
    unsigned int available = record->capacity - (unsigned int)data_offset;
    unsigned int length = (unsigned int)len < available ? (unsigned int)len : available;
    uintptr_t page_index = data_offset / KB_KVM_PAGE_SIZE;
    uintptr_t page_offset = data_offset % KB_KVM_PAGE_SIZE;
    void *page = (unsigned char *)record->page + page_index * KB_KVM_STRUCT_PAGE_SIZE;
    uintptr_t page_link = ((uintptr_t)page) | 0x2u;

    memset(sg, 0, 32);
    write_ptr_at(sg, KB_RHEL_3_10_SG_PAGE_LINK_OFFSET, (void *)page_link);
    write_u32_at(sg, KB_RHEL_3_10_SG_OFFSET_OFFSET, (uint32_t)page_offset);
    write_u32_at(sg, KB_RHEL_3_10_SG_LENGTH_OFFSET, length);
    if (trace_net_enabled()) {
        fprintf(stderr, "kobox net: skb_to_sgvec skb=%p sg=%p offset=%d len=%d page=%p page_offset=%lu dma=0x%llx\n",
            skb,
            sg,
            offset,
            len,
            page,
            (unsigned long)page_offset,
            (unsigned long long)(record->dma_handle + page_offset));
    }
    return length == 0 ? 0 : 1;
}

int kb_net_device_xmit_smoke(void *dev)
{
    kb_netdev_record_t *record = find_netdev_record(dev);
    void *xmit_ptr = record_ndo_start_xmit(record);
    if (record == NULL || xmit_ptr == NULL) {
        if (trace_net_enabled()) {
            fprintf(stderr, "kobox net: xmit_smoke skipped dev=%p xmit=%p\n", dev, xmit_ptr);
        }
        return 0;
    }

    static const unsigned char frame[60] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0x52, 0x54, 0x00, 0x12, 0x34, 0x56,
        0x08, 0x06,
        0x00, 0x01, 0x08, 0x00, 0x06, 0x04, 0x00, 0x01,
        0x52, 0x54, 0x00, 0x12, 0x34, 0x56,
        10, 0, 2, 15,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        10, 0, 2, 2,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };

    void *skb = kb_netdev_alloc_skb(dev, sizeof(frame), 0);
    unsigned char *payload = kb_skb_put(skb, sizeof(frame));
    if (payload == NULL) {
        return -12;
    }
    memcpy(payload, frame, sizeof(frame));
    int result = call_module_xmit(xmit_ptr, skb, dev);
    sync_e1000e_tx_shadow();
    dump_virtio_legacy_tx_rings();
    dump_e1000e_tx_rings();
    if (result == 0) {
        kb_msleep(1000);
        if (rx_poll_smoke_enabled()) {
            kb_net_device_poll_rx_smoke();
        }
        dump_virtio_legacy_tx_rings();
        dump_e1000e_tx_rings();
    }
    fprintf(stderr, "kobox net: xmit_smoke dev=%p skb=%p len=%zu result=%d\n", dev, skb, sizeof(frame), result);
    return result;
}

static uint16_t internet_checksum(const void *data, size_t len)
{
    const unsigned char *bytes = data;
    uint32_t sum = 0;
    while (len >= 2) {
        sum += ((uint32_t)bytes[0] << 8) | bytes[1];
        bytes += 2;
        len -= 2;
    }
    if (len != 0) {
        sum += (uint32_t)bytes[0] << 8;
    }
    while ((sum >> 16) != 0) {
        sum = (sum & 0xffffu) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

static void write_be16(unsigned char *dst, uint16_t value)
{
    dst[0] = (unsigned char)(value >> 8);
    dst[1] = (unsigned char)value;
}

static int xmit_raw_frame(void *dev, void *xmit_ptr, const unsigned char *frame, size_t frame_len)
{
    if (dev == NULL || xmit_ptr == NULL || frame == NULL || frame_len == 0 || frame_len > UINT32_MAX) {
        return -22;
    }

    void *skb = kb_netdev_alloc_skb(dev, (unsigned int)frame_len, 0);
    unsigned char *payload = kb_skb_put(skb, (unsigned int)frame_len);
    if (payload == NULL) {
        return -12;
    }
    memcpy(payload, frame, frame_len);

    int result = call_module_xmit(xmit_ptr, skb, dev);
    sync_e1000e_tx_shadow();
    dump_e1000e_tx_rings();
    return result;
}

void kb_net_device_set_rx_frame_callback(kb_net_rx_frame_callback_t callback, void *ctx)
{
    rx_frame_callback = callback;
    rx_frame_callback_ctx = ctx;
}

int kb_net_device_tx_frame(const void *frame, size_t frame_len)
{
    kb_netdev_record_t *record = first_opened_netdev_record();
    void *xmit_ptr = record_ndo_start_xmit(record);
    if (record == NULL || xmit_ptr == NULL) {
        if (trace_net_enabled()) {
            fprintf(stderr, "kobox net: tx_frame skipped dev=%p xmit=%p\n", record == NULL ? NULL : record->dev, xmit_ptr);
        }
        return -19;
    }
    return xmit_raw_frame(record->dev, xmit_ptr, frame, frame_len);
}

static size_t build_arp_reply_frame(
    unsigned char *frame,
    size_t frame_capacity,
    const unsigned char target_mac[6],
    const unsigned char target_ip[4])
{
    static const unsigned char source_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
    static const unsigned char source_ip[4] = {10, 0, 2, 15};
    const size_t frame_len = 60;

    if (frame == NULL || target_mac == NULL || target_ip == NULL || frame_capacity < frame_len) {
        return 0;
    }

    memset(frame, 0, frame_capacity);
    memcpy(frame, target_mac, 6);
    memcpy(frame + 6, source_mac, sizeof(source_mac));
    write_be16(frame + 12, 0x0806);
    write_be16(frame + 14, 0x0001);
    write_be16(frame + 16, 0x0800);
    frame[18] = 6;
    frame[19] = 4;
    write_be16(frame + 20, 0x0002);
    memcpy(frame + 22, source_mac, sizeof(source_mac));
    memcpy(frame + 28, source_ip, sizeof(source_ip));
    memcpy(frame + 32, target_mac, 6);
    memcpy(frame + 38, target_ip, 4);
    return frame_len;
}

static size_t build_udp_internet_smoke_frame(unsigned char *frame, size_t frame_capacity)
{
    static const unsigned char gateway_mac[6] = {0x52, 0x55, 0x0a, 0x00, 0x02, 0x02};
    static const unsigned char source_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
    static const unsigned char source_ip[4] = {10, 0, 2, 15};
    static const unsigned char dest_ip[4] = {10, 0, 2, 3};
    static const unsigned char dns_query[] = {
        0x4b, 0x42, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e',
        0x03, 'c', 'o', 'm',
        0x00, 0x00, 0x01, 0x00, 0x01,
    };
    const uint16_t ip_header_len = 20;
    const uint16_t udp_len = (uint16_t)(8 + sizeof(dns_query));
    const uint16_t ip_total_len = ip_header_len + udp_len;
    const size_t frame_len = 14u + ip_total_len;

    if (frame == NULL || frame_capacity < frame_len) {
        return 0;
    }

    memset(frame, 0, frame_capacity);
    memcpy(frame, gateway_mac, sizeof(gateway_mac));
    memcpy(frame + 6, source_mac, sizeof(source_mac));
    write_be16(frame + 12, 0x0800);

    unsigned char *ip = frame + 14;
    ip[0] = 0x45;
    ip[1] = 0x00;
    write_be16(ip + 2, ip_total_len);
    write_be16(ip + 4, 0x4242);
    write_be16(ip + 6, 0x4000);
    ip[8] = 64;
    ip[9] = 17;
    memcpy(ip + 12, source_ip, sizeof(source_ip));
    memcpy(ip + 16, dest_ip, sizeof(dest_ip));
    write_be16(ip + 10, internet_checksum(ip, ip_header_len));

    unsigned char *udp = ip + ip_header_len;
    write_be16(udp + 0, 49152);
    write_be16(udp + 2, 53);
    write_be16(udp + 4, udp_len);
    write_be16(udp + 6, 0);
    memcpy(udp + 8, dns_query, sizeof(dns_query));
    return frame_len;
}

int kb_net_device_xmit_internet_smoke(void *dev)
{
    kb_netdev_record_t *record = find_netdev_record(dev);
    void *xmit_ptr = record_ndo_start_xmit(record);
    if (record == NULL || xmit_ptr == NULL) {
        if (trace_net_enabled()) {
            fprintf(stderr, "kobox net: internet_smoke skipped dev=%p xmit=%p\n", dev, xmit_ptr);
        }
        return 0;
    }

    unsigned char frame[96];
    size_t frame_len = build_udp_internet_smoke_frame(frame, sizeof(frame));
    if (frame_len == 0) {
        return -22;
    }

    int result = xmit_raw_frame(dev, xmit_ptr, frame, frame_len);
    int arp_result = 0;
    if (result == 0) {
        record->internet_arp_reply_pending = 1;
        record->internet_arp_reply_sent = 0;
        record->internet_arp_request_seen = 0;
        memset(record->internet_arp_peer_mac, 0, sizeof(record->internet_arp_peer_mac));
        memset(record->internet_arp_peer_ip, 0, sizeof(record->internet_arp_peer_ip));
        kb_msleep(500);
        if (rx_poll_smoke_enabled()) {
            kb_net_device_poll_rx_smoke();
        }
        arp_result = record->internet_arp_reply_pending ? -11 : 0;
        if (arp_result == 0) {
            kb_msleep(500);
            kb_net_device_poll_rx_smoke();
        }
    }
    fprintf(stderr, "kobox net: internet_smoke dev=%p dst=10.0.2.3 udp_dport=53 dns_id=0x4b42 len=%zu result=%d arp_result=%d\n",
        dev,
        frame_len,
        result,
        arp_result);
    return result == 0 ? arp_result : result;
}

void kb_net_device_run_pending_smokes(void)
{
    if (!tx_smoke_enabled()) {
        return;
    }
    for (size_t i = 0; i < KB_NETDEV_TRACKED_MAX; i++) {
        kb_netdev_record_t *record = &netdev_records[i];
        if (record->dev == NULL || !record->registered || !record->opened || record->smoke_done) {
            continue;
        }
        record->smoke_done = 1;
        int result = 0;
        if (tx_smoke_wants_arp()) {
            result = kb_net_device_xmit_smoke(record->dev);
        }
        if (result == 0 && tx_smoke_wants_udp()) {
            result = kb_net_device_xmit_internet_smoke(record->dev);
        }
        if (trace_net_enabled()) {
            fprintf(stderr, "kobox net: pending_xmit_smoke dev=%p result=%d\n", record->dev, result);
        }
    }
}

void kb_net_device_set_carrier(void *dev, int carrier_on)
{
    kb_netdev_record_t *record = find_netdev_record(dev);
    if (record != NULL) {
        record->carrier_on = carrier_on ? 1 : 0;
    }
    if (trace_net_enabled()) {
        fprintf(stderr, "kobox net: carrier_%s dev=%p\n", carrier_on ? "on" : "off", dev);
    }
}

void kb_net_device_addr_mod(void *dev, unsigned int offset, const void *addr, size_t len)
{
    kb_netdev_record_t *record = find_netdev_record(dev);
    if (record != NULL && addr != NULL && offset < KB_NETDEV_ADDR_MAX && len <= KB_NETDEV_ADDR_MAX - offset) {
        memcpy(record->dev_addr + offset, addr, len);
    }
}

static int kb_net_device_maybe_reply_gateway_arp(kb_netdev_record_t *record, unsigned int rx_before)
{
    if (record == NULL ||
        !record->internet_arp_reply_pending ||
        record->internet_arp_reply_sent ||
        !record->internet_arp_request_seen)
    {
        return 0;
    }

    unsigned int rx_after = 0;
    for (kb_napi_record_t *napi = napi_records; napi != NULL; napi = napi->next) {
        if (napi->dev == record->dev) {
            rx_after += napi->rx_count;
        }
    }
    if (rx_after <= rx_before) {
        return 0;
    }

    void *xmit_ptr = record_ndo_start_xmit(record);
    unsigned char arp_frame[60];
    size_t arp_len = build_arp_reply_frame(
        arp_frame,
        sizeof(arp_frame),
        record->internet_arp_peer_mac,
        record->internet_arp_peer_ip);
    int result = xmit_raw_frame(record->dev, xmit_ptr, arp_frame, arp_len);
    if (result == 0) {
        record->internet_arp_reply_pending = 0;
        record->internet_arp_reply_sent = 1;
    }
    fprintf(stderr,
        "kobox net: rx_arp_reply dev=%p rx_before=%u rx_after=%u peer=%u.%u.%u.%u len=%zu result=%d\n",
        record->dev,
        rx_before,
        rx_after,
        record->internet_arp_peer_ip[0],
        record->internet_arp_peer_ip[1],
        record->internet_arp_peer_ip[2],
        record->internet_arp_peer_ip[3],
        arp_len,
        result);
    return result;
}

static void napi_set_scheduled(void *napi)
{
    if (napi == NULL) {
        return;
    }
    unsigned long state = 0;
    memcpy(&state, (unsigned char *)napi + KB_RHEL_3_10_NAPI_STATE_OFFSET, sizeof(state));
    state |= 1ul << KB_NAPI_STATE_SCHED;
    memcpy((unsigned char *)napi + KB_RHEL_3_10_NAPI_STATE_OFFSET, &state, sizeof(state));
}

void kb_netif_napi_add(void *dev, void *napi, void *poll, int weight)
{
    if (napi == NULL) {
        return;
    }
    kb_napi_record_t *record = find_napi_record(napi);
    if (record == NULL) {
        record = kb_kzalloc(sizeof(*record), 0);
        if (record != NULL) {
            record->next = napi_records;
            napi_records = record;
        }
    }
    if (record != NULL) {
        record->dev = dev;
        record->napi = napi;
        record->poll = poll;
        record->weight = weight;
    }
    napi_set_scheduled(napi);
    memcpy((unsigned char *)napi + KB_RHEL_3_10_NAPI_POLL_OFFSET, &poll, sizeof(poll));
    memcpy((unsigned char *)napi + KB_RHEL_3_10_NAPI_WEIGHT_OFFSET, &weight, sizeof(weight));
    if (trace_net_enabled()) {
        fprintf(stderr, "kobox net: napi_add dev=%p napi=%p poll=%p weight=%d\n", dev, napi, poll, weight);
    }
}

void kb_napi_disable(void *napi)
{
    napi_set_scheduled(napi);
    if (trace_net_enabled()) {
        fprintf(stderr, "kobox net: napi_disable napi=%p\n", napi);
    }
}

int kb_napi_schedule_prep(void *napi)
{
    if (napi == NULL) {
        return 0;
    }
    napi_set_scheduled(napi);
    return 1;
}

int kb_napi_gro_receive(void *napi, void *skb)
{
    kb_napi_record_t *record = find_napi_record(napi);
    if (record != NULL) {
        record->rx_count++;
    }
    if (trace_net_enabled()) {
        fprintf(stderr,
            "kobox net: napi_gro_receive napi=%p skb=%p rx_count=%u\n",
            napi,
            skb,
            record == NULL ? 0 : record->rx_count);
    }
    log_received_packet(record, skb);
    return 0;
}

static void poll_rx_devices(int verbose)
{
    for (kb_napi_record_t *record = napi_records; record != NULL; record = record->next) {
        if (record->napi == NULL || record->poll == NULL) {
            continue;
        }
        kb_netdev_record_t *netdev = find_netdev_record(record->dev);
        unsigned int rx_before = record->rx_count;
        kb_napi_poll_t poll = NULL;
        memcpy(&poll, &record->poll, sizeof(poll));
        int budget = record->weight > 0 ? record->weight : 64;
        unsigned long old_gs = 0;
        unsigned long kernel_gs = kb_module_kernel_gs_for_address(record->poll);
        int has_gs = kernel_gs != 0 && kb_shim_enter_kernel_gs(kernel_gs, &old_gs) == 0;
        int result = poll(record->napi, budget);
        if (has_gs) {
            kb_shim_leave_kernel_gs(old_gs);
        }
        record->poll_count++;
        int reply_result = kb_net_device_maybe_reply_gateway_arp(netdev, rx_before);
        if (verbose || trace_net_enabled()) {
            fprintf(stderr,
                "kobox net: rx_poll_smoke dev=%p napi=%p poll=%p budget=%d result=%d polls=%u rx=%u reply=%d\n",
                record->dev,
                record->napi,
                record->poll,
                budget,
                result,
                record->poll_count,
                record->rx_count,
                reply_result);
        }
    }
}

void kb_net_device_poll_rx_smoke(void)
{
    poll_rx_devices(1);
}

void kb_net_device_poll(void)
{
    kb_run_deferred_work();
    (void)kb_handle_any_irq(0);
    poll_rx_devices(0);
}
