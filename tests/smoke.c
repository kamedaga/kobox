#include "kobox/device.h"
#include "kobox/device_linux_mock.h"
#include "kobox/elf.h"
#include "kobox/interface_linux.h"
#include "kobox/module.h"
#include "kobox/platform.h"
#include "kobox/shim.h"
#include "../src/device/device_backend_internal.h"
#include "../src/linux_personality/linux_block.h"
#include "../src/linux_personality/linux_nvme.h"
#include "../src/linux_subsystem/block/block.h"
#include "../src/linux_subsystem/dma/dma.h"
#include "../src/linux_subsystem/kvm/kvm_symbols.h"
#include "../src/linux_subsystem/net/net_device.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void irq_callback(void *ctx)
{
    int *called = ctx;
    *called = 1;
}

static void write_u16le(unsigned char *p, uint16_t value)
{
    p[0] = (unsigned char)(value & 0xffu);
    p[1] = (unsigned char)((value >> 8) & 0xffu);
}

static void write_u32le(unsigned char *p, uint32_t value)
{
    p[0] = (unsigned char)(value & 0xffu);
    p[1] = (unsigned char)((value >> 8) & 0xffu);
    p[2] = (unsigned char)((value >> 16) & 0xffu);
    p[3] = (unsigned char)((value >> 24) & 0xffu);
}

static void write_u64le(unsigned char *p, uint64_t value)
{
    write_u32le(p, (uint32_t)(value & 0xffffffffu));
    write_u32le(p + 4, (uint32_t)(value >> 32));
}

static uint64_t read_u64le(const unsigned char *p)
{
    uint64_t value;
    memcpy(&value, p, sizeof(value));
    return value;
}

typedef struct scatter_dma_test_backend scatter_dma_test_backend_t;

typedef struct scatter_dma_test_device {
    scatter_dma_test_backend_t *backend;
} scatter_dma_test_device_t;

typedef struct scatter_dma_test_mapping {
    void *cpu_addr;
    uint64_t dma_addr;
    uint64_t size;
    kb_dma_dir_t direction;
    unsigned int unmap_count;
} scatter_dma_test_mapping_t;

struct scatter_dma_test_backend {
    kb_device_backend_t base;
    scatter_dma_test_device_t device;
    scatter_dma_test_mapping_t mappings[4];
    unsigned int mapping_count;
    unsigned int page_map_call_count;
    unsigned int single_map_call_count;
    int fail_single_map;
    uint64_t page_dma[8];
    size_t page_count;
    unsigned int unmap_order[4];
    unsigned int unmap_count;
};

static kb_status_t scatter_dma_test_device_at(
    kb_device_backend_t *backend_raw,
    size_t index,
    kb_device_t **out_device)
{
    scatter_dma_test_backend_t *backend = (scatter_dma_test_backend_t *)backend_raw;
    if (backend == NULL || index != 0 || out_device == NULL) {
        return KB_ERR_INVALID;
    }
    *out_device = (kb_device_t *)&backend->device;
    return KB_OK;
}

static kb_status_t scatter_dma_test_map(
    kb_device_t *device_raw,
    void *cpu_addr,
    uint64_t size,
    kb_dma_dir_t direction,
    uint64_t *out_dma_addr)
{
    scatter_dma_test_device_t *device = (scatter_dma_test_device_t *)device_raw;
    scatter_dma_test_backend_t *backend = device == NULL ? NULL : device->backend;
    if (backend == NULL || cpu_addr == NULL || size == 0 || out_dma_addr == NULL ||
        backend->mapping_count >= 4)
    {
        return KB_ERR_INVALID;
    }
    backend->single_map_call_count++;
    if (backend->fail_single_map) {
        return KB_ERR_IO;
    }

    const uint64_t dma_addr = UINT64_C(0x20000000) +
        (uint64_t)backend->mapping_count * UINT64_C(0x10000);
    scatter_dma_test_mapping_t *mapping = &backend->mappings[backend->mapping_count++];
    mapping->cpu_addr = cpu_addr;
    mapping->dma_addr = dma_addr;
    mapping->size = size;
    mapping->direction = direction;
    *out_dma_addr = dma_addr;
    return KB_OK;
}

static kb_status_t scatter_dma_test_map_pages(
    kb_device_t *device_raw,
    void *cpu_addr,
    uint64_t size,
    kb_dma_dir_t direction,
    uint64_t *out_page_dma,
    size_t out_capacity)
{
    enum { PAGE_SIZE = 4096 };
    scatter_dma_test_device_t *device = (scatter_dma_test_device_t *)device_raw;
    scatter_dma_test_backend_t *backend = device == NULL ? NULL : device->backend;
    if (backend == NULL || cpu_addr == NULL || size == 0 || size > (uint64_t)SIZE_MAX ||
        out_page_dma == NULL || backend->mapping_count >= 4)
    {
        return KB_ERR_INVALID;
    }

    const size_t page_offset = (uintptr_t)cpu_addr & (PAGE_SIZE - 1u);
    if ((size_t)size > SIZE_MAX - page_offset) {
        return KB_ERR_INVALID;
    }
    const size_t span = page_offset + (size_t)size;
    const size_t page_count = span / PAGE_SIZE + (span % PAGE_SIZE != 0);
    if (page_count == 0 || page_count > out_capacity || page_count > 8) {
        return KB_ERR_INVALID;
    }

    backend->page_map_call_count++;
    backend->page_count = page_count;
    for (size_t i = 0; i < page_count; i++) {
        const uint64_t page_base = UINT64_C(0x10000000) + (uint64_t)i * UINT64_C(0x3000);
        out_page_dma[i] = page_base + (i == 0 ? page_offset : 0);
        backend->page_dma[i] = out_page_dma[i];
    }

    scatter_dma_test_mapping_t *mapping = &backend->mappings[backend->mapping_count++];
    mapping->cpu_addr = cpu_addr;
    mapping->dma_addr = out_page_dma[0];
    mapping->size = size;
    mapping->direction = direction;
    return KB_OK;
}

static void scatter_dma_test_unmap(
    kb_device_t *device_raw,
    uint64_t dma_addr,
    uint64_t size,
    kb_dma_dir_t direction)
{
    scatter_dma_test_device_t *device = (scatter_dma_test_device_t *)device_raw;
    scatter_dma_test_backend_t *backend = device == NULL ? NULL : device->backend;
    if (backend == NULL) {
        return;
    }
    for (unsigned int i = 0; i < backend->mapping_count; i++) {
        scatter_dma_test_mapping_t *mapping = &backend->mappings[i];
        if (mapping->dma_addr == dma_addr && mapping->size == size && mapping->direction == direction) {
            mapping->unmap_count++;
            if (backend->unmap_count < 4) {
                backend->unmap_order[backend->unmap_count++] = i;
            }
            return;
        }
    }
}

static const kb_device_backend_ops_t scatter_dma_test_ops = {
    .device_at = scatter_dma_test_device_at,
    .dma_map = scatter_dma_test_map,
    .dma_map_pages = scatter_dma_test_map_pages,
    .dma_unmap = scatter_dma_test_unmap,
};

static int test_nvme_scattered_prps(void)
{
    enum {
        PAGE_SIZE = 4096,
        BUFFER_OFFSET = 123,
        BUFFER_ALLOC_SIZE = PAGE_SIZE * 5,
        BUFFER_SIZE = (PAGE_SIZE - BUFFER_OFFSET) + PAGE_SIZE * 2 + 321,
        NVME_DEV_CTRL_OFFSET = 0x1f0,
        NVME_QUEUE_QID_OFFSET = 0x74,
        TAG_SET_DRIVER_DATA_OFFSET = 0x58,
    };

    scatter_dma_test_backend_t backend;
    memset(&backend, 0, sizeof(backend));
    backend.base.ops = &scatter_dma_test_ops;
    backend.device.backend = &backend;
    kb_shim_set_device_backend(&backend.base);

    unsigned char nvme_device[0x300];
    unsigned char nvme_queue[0x100];
    unsigned char tag_set[0x100];
    memset(nvme_device, 0, sizeof(nvme_device));
    memset(nvme_queue, 0, sizeof(nvme_queue));
    memset(tag_set, 0, sizeof(tag_set));
    void *device_ptr = nvme_device;
    void *ctrl = nvme_device + NVME_DEV_CTRL_OFFSET;
    memcpy(nvme_queue, &device_ptr, sizeof(device_ptr));
    write_u16le(nvme_queue + NVME_QUEUE_QID_OFFSET, 1);
    memcpy(tag_set + TAG_SET_DRIVER_DATA_OFFSET, &ctrl, sizeof(ctrl));

    kb_nvme_shim_track_queue(nvme_queue);
    kb_nvme_shim_register_block_driver();
    void *request = kb_linux_block_alloc_driver_request(tag_set, ctrl, nvme_queue, 0x22, 1);
    void *raw_buffer = aligned_alloc(PAGE_SIZE, BUFFER_ALLOC_SIZE);
    int result = 0;
    if (request == NULL || raw_buffer == NULL) {
        result = 1;
        goto cleanup;
    }

    unsigned char *command = kb_linux_block_request_command(request);
    unsigned char *buffer = (unsigned char *)raw_buffer + BUFFER_OFFSET;
    memset(command, 0, 64);
    command[0] = 0x02;

    if (kb_blk_rq_map_kern(NULL, request, buffer, PAGE_SIZE * 512u, 0) != -95 ||
        backend.page_map_call_count != 0 || backend.single_map_call_count != 0 ||
        backend.mapping_count != 0 || backend.unmap_count != 0 ||
        read_u64le(command + 24) != 0 || read_u64le(command + 32) != 0)
    {
        result = 2;
        goto cleanup;
    }

    backend.fail_single_map = 1;
    if (kb_blk_rq_map_kern(NULL, request, buffer, BUFFER_SIZE, 0) == 0 ||
        backend.page_map_call_count != 1 || backend.single_map_call_count != 1 ||
        backend.mapping_count != 1 || backend.unmap_count != 1 ||
        backend.unmap_order[0] != 0 || backend.mappings[0].unmap_count != 1 ||
        read_u64le(command + 24) != 0 || read_u64le(command + 32) != 0)
    {
        result = 3;
        goto cleanup;
    }

    memset(backend.mappings, 0, sizeof(backend.mappings));
    memset(backend.page_dma, 0, sizeof(backend.page_dma));
    memset(backend.unmap_order, 0, sizeof(backend.unmap_order));
    backend.mapping_count = 0;
    backend.page_map_call_count = 0;
    backend.single_map_call_count = 0;
    backend.page_count = 0;
    backend.unmap_count = 0;
    backend.fail_single_map = 0;
    if (kb_blk_rq_map_kern(NULL, request, buffer, BUFFER_SIZE, 0) != 0) {
        result = 4;
        goto cleanup;
    }
    if (backend.page_map_call_count != 1 || backend.single_map_call_count != 1 ||
        backend.page_count != 4 || backend.mapping_count != 2 ||
        read_u64le(command + 24) != backend.page_dma[0] ||
        read_u64le(command + 32) != backend.mappings[1].dma_addr)
    {
        result = 5;
        goto cleanup;
    }

    size_t prp_list_available = 0;
    const uint64_t *prp_list = kb_subsystem_dma_cpu_addr(read_u64le(command + 32), &prp_list_available);
    if (prp_list == NULL || prp_list_available < 3u * sizeof(*prp_list) ||
        prp_list[0] != backend.page_dma[1] ||
        prp_list[1] != backend.page_dma[2] ||
        prp_list[2] != backend.page_dma[3] ||
        prp_list[1] == prp_list[0] + PAGE_SIZE ||
        backend.mappings[0].size != BUFFER_SIZE ||
        backend.mappings[1].size != 3u * sizeof(uint64_t) ||
        backend.mappings[0].direction != KB_DMA_FROM_DEVICE ||
        backend.mappings[1].direction != KB_DMA_TO_DEVICE)
    {
        result = 6;
        goto cleanup;
    }

cleanup:
    if (request != NULL) {
        kb_blk_mq_free_request(request);
    }
    free(raw_buffer);
    if (result == 0) {
        static const unsigned int expected_unmap_order[] = {1, 0};
        if (backend.unmap_count != 2 ||
            memcmp(backend.unmap_order, expected_unmap_order, sizeof(expected_unmap_order)) != 0)
        {
            result = 7;
        }
        for (unsigned int i = 0; result == 0 && i < backend.mapping_count; i++) {
            size_t available = 1;
            if (backend.mappings[i].unmap_count != 1 ||
                kb_subsystem_dma_cpu_addr(backend.mappings[i].dma_addr, &available) != NULL ||
                available != 0)
            {
                result = 8;
            }
        }
    }
    kb_block_subsystem_tagset_free(tag_set);
    kb_shim_set_device_backend(NULL);
    return result;
}

static void write_elf_symbol(
    unsigned char *p,
    uint32_t name,
    uint8_t info,
    uint16_t section_index,
    uint64_t value,
    uint64_t size)
{
    write_u32le(p, name);
    p[4] = info;
    p[5] = 0;
    write_u16le(p + 6, section_index);
    write_u64le(p + 8, value);
    write_u64le(p + 16, size);
}

static void write_elf_rela(unsigned char *p, uint64_t offset, uint32_t symbol_index, uint32_t type, int64_t addend)
{
    write_u64le(p, offset);
    write_u64le(p + 8, ((uint64_t)symbol_index << 32) | (uint64_t)type);
    write_u64le(p + 16, (uint64_t)addend);
}

static int test_executable_module(kb_device_backend_t *backend)
{
    unsigned char elf[768];
    memset(elf, 0, sizeof(elf));

    elf[0] = 0x7f;
    elf[1] = 'E';
    elf[2] = 'L';
    elf[3] = 'F';
    elf[4] = 2;
    elf[5] = 1;
    elf[6] = 1;
    write_u16le(elf + 16, KB_ELF_ET_REL);
    write_u16le(elf + 18, KB_ELF_EM_X86_64);
    write_u32le(elf + 20, 1);
    write_u64le(elf + 40, 64);
    write_u16le(elf + 52, 64);
    write_u16le(elf + 58, 64);
    write_u16le(elf + 60, 5);
    write_u16le(elf + 62, 4);

    unsigned char *text = elf + 64 + 64;
    write_u32le(text, 1);
    write_u32le(text + 4, KB_ELF_SHT_PROGBITS);
    write_u64le(text + 8, KB_ELF_SHF_ALLOC | KB_ELF_SHF_EXECINSTR);
    write_u64le(text + 24, 0x200);
    write_u64le(text + 32, 6);
    write_u64le(text + 48, 16);

    unsigned char *symtab = elf + 64 + (2 * 64);
    write_u32le(symtab, 7);
    write_u32le(symtab + 4, KB_ELF_SHT_SYMTAB);
    write_u64le(symtab + 24, 0x220);
    write_u64le(symtab + 32, 48);
    write_u32le(symtab + 40, 3);
    write_u32le(symtab + 44, 1);
    write_u64le(symtab + 48, 8);
    write_u64le(symtab + 56, 24);

    unsigned char *strtab = elf + 64 + (3 * 64);
    write_u32le(strtab, 15);
    write_u32le(strtab + 4, KB_ELF_SHT_STRTAB);
    write_u64le(strtab + 24, 0x250);
    write_u64le(strtab + 32, 13);
    write_u64le(strtab + 48, 1);

    unsigned char *shstrtab = elf + 64 + (4 * 64);
    write_u32le(shstrtab, 23);
    write_u32le(shstrtab + 4, KB_ELF_SHT_STRTAB);
    write_u64le(shstrtab + 24, 0x260);
    write_u64le(shstrtab + 32, 33);
    write_u64le(shstrtab + 48, 1);

    memcpy(elf + 0x200, "\xb8\x7b\x00\x00\x00\xc3", 6);
    write_elf_symbol(elf + 0x220, 0, 0, KB_ELF_SHN_UNDEF, 0, 0);
    write_elf_symbol(elf + 0x220 + 24, 1, (KB_ELF_STB_GLOBAL << 4) | KB_ELF_STT_FUNC, 1, 0, 6);
    memcpy(elf + 0x250, "\0init_module\0", 13);
    memcpy(elf + 0x260, "\0.text\0.symtab\0.strtab\0.shstrtab\0", 33);

    kb_module_image_t image = {
        .data = elf,
        .size = sizeof(elf),
        .name = "executable-fixture.ko",
    };
    kb_module_t *module = 0;
    if (kb_module_open_image(&image, backend, &module) != KB_OK || module == 0) {
        kb_module_close(module);
        return 1;
    }

    int result = 0;
    if (kb_module_call_init(module, &result) != KB_OK || result != 123) {
        kb_module_close(module);
        return 2;
    }
    if (kb_module_call_cleanup(module) != KB_ERR_NOT_FOUND) {
        kb_module_close(module);
        return 3;
    }

    kb_module_close(module);
    return 0;
}

static int test_platform_facets(void)
{
    kb_device_backend_t *backend = NULL;
    if (kb_linux_mock_device_create(&backend) != KB_OK || backend == NULL) {
        return 1;
    }

    kb_platform_t *platform = NULL;
    kb_platform_desc_t desc = {
        "mock-platform",
        backend,
        NULL,
        0,
    };
    if (kb_platform_create(&desc, &platform) != KB_OK || platform == NULL) {
        kb_device_backend_destroy(backend);
        return 1;
    }

    int failed = 0;
    failed += strcmp(kb_platform_name(platform), "mock-platform") != 0;
    failed += kb_platform_device_backend(platform) != backend;

    const kb_platform_memory_ops_t *memory = kb_platform_memory(platform);
    void *mem = memory == NULL || memory->alloc == NULL ? NULL : memory->alloc(platform, 32, 16);
    failed += mem == NULL;
    if (memory != NULL && memory->free != NULL) {
        memory->free(platform, mem, 32);
    }

    const kb_platform_time_ops_t *time_ops = kb_platform_time(platform);
    failed += time_ops == NULL || time_ops->monotonic_ns == NULL || time_ops->monotonic_ns(platform) == 0;

    const kb_platform_log_ops_t *log_ops = kb_platform_log(platform);
    if (log_ops != NULL && log_ops->log != NULL) {
        log_ops->log(platform, 0, "platform facet smoke");
    } else {
        failed++;
    }

    size_t interface_count = 99;
    failed += kb_platform_interface_count(platform, &interface_count) != KB_OK || interface_count != 0;

    kb_platform_destroy(platform);
    return failed != 0;
}

static int test_linux_host_interfaces(void)
{
    kb_device_backend_t *backend = NULL;
    if (kb_linux_mock_device_create(&backend) != KB_OK || backend == NULL) {
        return 1;
    }

    kb_interface_t *ipc = NULL;
    kb_linux_interface_desc_t ipc_desc = {
        .name = "linux-ipc-fs",
        .subsystem = "fs",
        .endpoint = "kobox.fs",
    };
    if (kb_linux_ipc_interface_create(&ipc_desc, &ipc) != KB_OK || ipc == NULL) {
        kb_device_backend_destroy(backend);
        return 1;
    }

    kb_interface_t *interfaces[] = {
        ipc,
    };
    kb_platform_t *platform = NULL;
    kb_platform_desc_t platform_desc = {
        "linux-interface-platform",
        backend,
        interfaces,
        1,
    };
    if (kb_platform_create(&platform_desc, &platform) != KB_OK || platform == NULL) {
        kb_interface_destroy(ipc);
        kb_device_backend_destroy(backend);
        return 1;
    }

    int failed = 0;
    size_t interface_count = 0;
    failed += kb_platform_interface_count(platform, &interface_count) != KB_OK || interface_count != 1;

    kb_interface_t *first = NULL;
    failed += kb_platform_interface_at(platform, 0, &first) != KB_OK || first != ipc;
    failed += kb_interface_kind(first) != KB_INTERFACE_IPC;
    failed += strcmp(kb_interface_name(first), "linux-ipc-fs") != 0;
    failed += strcmp(kb_interface_subsystem(first), "fs") != 0;
    failed += kb_interface_bind(first, platform) != KB_OK;
    failed += kb_interface_poll(first, 0) != KB_OK;
    failed += kb_interface_dispatch(first, "mount", 5) != KB_OK;
    kb_interface_unbind(first);

    kb_platform_destroy(platform);
    return failed != 0;
}

static kb_device_backend_t *net_lifetime_test_backend;
static unsigned int net_lifetime_test_xmits;

static int net_lifetime_test_xmit(void *skb, void *dev)
{
    (void)dev;
    int result = kb_shim_current_device_backend() == net_lifetime_test_backend ? 0 : 1;
    net_lifetime_test_xmits++;
    kb_consume_skb(skb);
    return result;
}

static int test_net_backend_and_page_lifetime(kb_device_backend_t *backend)
{
    enum {
        RHEL_NETDEV_OPS_OFFSET = 0x198,
    };
    kb_shim_set_device_backend(backend);
    if (kb_kvm_prepare_dma_arena(backend) != 0) {
        return 1;
    }

    void *page = kb_kvm_alloc_pages_stub(0, 1);
    if (page == NULL || kb_kvm_release_pages(page, 0) != 0) {
        return 2;
    }
    void *next_page = kb_kvm_alloc_pages_stub(0, 1);
    if (next_page == NULL || next_page == page) {
        return 3;
    }
    if (!kb_kvm_release_pages(next_page, 1) || !kb_kvm_release_pages(page, 1) ||
        kb_kvm_release_pages(page, 1) != 0)
    {
        return 4;
    }
    void *reused_page = kb_kvm_alloc_pages_stub(0, 1);
    if (reused_page != page || !kb_kvm_release_pages(reused_page, 1)) {
        return 5;
    }

    unsigned long address = kb_kvm_get_free_pages_stub(0, 1);
    if (address == 0) {
        return 6;
    }
    kb_kvm_free_pages_addr_stub(address, 1);
    unsigned long reused_address = kb_kvm_get_free_pages_stub(0, 1);
    if (reused_address != address) {
        return 7;
    }
    kb_kvm_free_pages_addr_stub(reused_address, 1);

    void *dev = kb_net_device_alloc(0, "nettest%d", 0, NULL, 1, 1);
    if (dev == NULL) {
        return 8;
    }
    void *ops[5] = {0};
    int (*xmit)(void *, void *) = net_lifetime_test_xmit;
    memcpy(&ops[4], &xmit, sizeof(xmit));
    void *ops_ptr = ops;
    memcpy((unsigned char *)dev + RHEL_NETDEV_OPS_OFFSET, &ops_ptr, sizeof(ops_ptr));
    if (kb_net_device_register(dev) != 0 || kb_net_device_open(dev) != 0) {
        kb_net_device_free(dev);
        return 9;
    }

    net_lifetime_test_backend = backend;
    net_lifetime_test_xmits = 0;
    kb_shim_set_device_backend(NULL);
    unsigned char frame[60] = {0};
    int failed = 0;
    for (unsigned int i = 0; i < 5000; i++) {
        if (kb_net_device_tx_frame(frame, sizeof(frame)) != 0) {
            failed = 1;
            break;
        }
    }
    failed |= net_lifetime_test_xmits != 5000 || kb_shim_current_device_backend() != NULL;
    kb_net_device_free(dev);
    net_lifetime_test_backend = NULL;
    return failed ? 10 : 0;
}

int main(void)
{
    if (getenv("KOBOX_TEST_NVME_SCATTER_ONLY") != NULL) {
        return test_nvme_scattered_prps();
    }

    kb_device_backend_t *backend = 0;
    if (kb_linux_mock_device_create(&backend) != KB_OK || backend == 0) {
        return 1;
    }

    const kb_device_backend_ops_t *ops = kb_device_backend_get_ops(backend);
    if (ops == 0) {
        kb_device_backend_destroy(backend);
        return 2;
    }
    const unsigned encoded_msix =
        (KB_DEVICE_IRQ_BACKEND_KIND_MSIX << KB_DEVICE_IRQ_BACKEND_KIND_SHIFT) | 15u;
    if (kb_device_irq_backend_kind(encoded_msix) != KB_DEVICE_IRQ_BACKEND_KIND_MSIX ||
        kb_device_irq_backend_vector(encoded_msix) != 15u ||
        KB_DEVICE_ROUTED_MSIX_ENTRY_COUNT != 16 ||
        ops->msix_delivery_vector != 0)
    {
        kb_device_backend_destroy(backend);
        return 32;
    }

    size_t count = 0;
    if (ops->device_count(backend, &count) != KB_OK || count != 1) {
        kb_device_backend_destroy(backend);
        return 3;
    }

    kb_device_t *device = 0;
    if (ops->device_at(backend, 0, &device) != KB_OK || device == 0) {
        kb_device_backend_destroy(backend);
        return 4;
    }

    kb_pci_bar_info_t bar_info;
    memset(&bar_info, 0, sizeof(bar_info));
    if (ops->pci_bar_info == 0 || ops->pci_bar_info(device, 0, &bar_info) != KB_OK) {
        kb_device_backend_destroy(backend);
        return 30;
    }
    if (!bar_info.present || bar_info.size != 4096) {
        kb_device_backend_destroy(backend);
        return 31;
    }

    kb_dma_buffer_t dma;
    memset(&dma, 0, sizeof(dma));
    if (ops->dma_alloc(device, 4096, 4096, KB_DMA_BIDIRECTIONAL, &dma) != KB_OK) {
        kb_device_backend_destroy(backend);
        return 5;
    }
    if (dma.cpu_addr == 0 || dma.dma_addr == 0 || dma.size != 4096) {
        ops->dma_free(device, &dma);
        kb_device_backend_destroy(backend);
        return 6;
    }
    ops->dma_free(device, &dma);

    int called = 0;
    kb_device_irq_t *irq = 0;
    if (ops->irq_register(device, 0, irq_callback, &called, &irq) != KB_OK) {
        kb_device_backend_destroy(backend);
        return 7;
    }
    if (ops->irq_wait(device, irq, 0) != KB_OK || called != 1) {
        ops->irq_unregister(device, irq);
        kb_device_backend_destroy(backend);
        return 8;
    }
    ops->irq_unregister(device, irq);

    void *mem = kb_kzalloc(16, 0);
    if (mem == 0) {
        kb_device_backend_destroy(backend);
        return 9;
    }
    kb_kfree(mem);

    const unsigned char fake_module[] = {0x7f, 'E', 'L', 'F'};
    kb_module_image_t image = {
        .data = fake_module,
        .size = sizeof(fake_module),
        .name = "fake.ko",
    };
    kb_module_t *module = 0;
    if (kb_module_open_image(&image, backend, &module) != KB_ERR_INVALID) {
        kb_module_close(module);
        kb_device_backend_destroy(backend);
        return 10;
    }
    if (module != 0) {
        kb_device_backend_destroy(backend);
        return 11;
    }

    unsigned char elf[832];
    memset(elf, 0, sizeof(elf));
    elf[0] = 0x7f;
    elf[1] = 'E';
    elf[2] = 'L';
    elf[3] = 'F';
    elf[4] = 2;
    elf[5] = 1;
    elf[6] = 1;
    write_u16le(elf + 16, KB_ELF_ET_REL);
    write_u16le(elf + 18, KB_ELF_EM_X86_64);
    write_u32le(elf + 20, 1);
    write_u64le(elf + 40, 64);
    write_u16le(elf + 52, 64);
    write_u16le(elf + 58, 64);
    write_u16le(elf + 60, 7);
    write_u16le(elf + 62, 6);

    unsigned char *text = elf + 64 + 64;
    write_u32le(text, 1);
    write_u32le(text + 4, KB_ELF_SHT_PROGBITS);
    write_u64le(text + 24, 0x200);
    write_u64le(text + 32, 0x20);
    write_u64le(text + 48, 16);

    unsigned char *symtab = elf + 64 + (2 * 64);
    write_u32le(symtab, 7);
    write_u32le(symtab + 4, KB_ELF_SHT_SYMTAB);
    write_u64le(symtab + 24, 0x220);
    write_u64le(symtab + 32, 72);
    write_u32le(symtab + 40, 3);
    write_u32le(symtab + 44, 1);
    write_u64le(symtab + 48, 8);
    write_u64le(symtab + 56, 24);

    unsigned char *strtab_section = elf + 64 + (3 * 64);
    write_u32le(strtab_section, 15);
    write_u32le(strtab_section + 4, KB_ELF_SHT_STRTAB);
    write_u64le(strtab_section + 24, 0x268);
    write_u64le(strtab_section + 32, 20);

    unsigned char *rela_text = elf + 64 + (4 * 64);
    write_u32le(rela_text, 23);
    write_u32le(rela_text + 4, KB_ELF_SHT_RELA);
    write_u64le(rela_text + 24, 0x280);
    write_u64le(rela_text + 32, 24);
    write_u32le(rela_text + 40, 2);
    write_u32le(rela_text + 44, 1);
    write_u64le(rela_text + 48, 8);
    write_u64le(rela_text + 56, 24);

    unsigned char *modinfo = elf + 64 + (5 * 64);
    write_u32le(modinfo, 34);
    write_u32le(modinfo + 4, KB_ELF_SHT_PROGBITS);
    write_u64le(modinfo + 24, 0x2c8);
    write_u64le(modinfo + 32, 36);
    write_u64le(modinfo + 48, 1);

    unsigned char *shstrtab = elf + 64 + (6 * 64);
    write_u32le(shstrtab, 43);
    write_u32le(shstrtab + 4, KB_ELF_SHT_STRTAB);
    write_u64le(shstrtab + 24, 0x300);
    write_u64le(shstrtab + 32, 53);

    write_elf_symbol(elf + 0x220, 0, 0, KB_ELF_SHN_UNDEF, 0, 0);
    write_elf_symbol(elf + 0x220 + 24, 1, (KB_ELF_STB_GLOBAL << 4) | KB_ELF_STT_FUNC, 1, 0, 16);
    write_elf_symbol(elf + 0x220 + 48, 13, (KB_ELF_STB_GLOBAL << 4) | KB_ELF_STT_NOTYPE, KB_ELF_SHN_UNDEF, 0, 0);
    write_elf_rela(elf + 0x280, 4, 2, KB_ELF_R_X86_64_PLT32, -4);

    memcpy(elf + 0x268, "\0init_module\0printk\0", 20);
    memcpy(elf + 0x2c8, "vermagic=6.6.0-test\0depends=foo,bar\0", 36);
    memcpy(elf + 0x300, "\0.text\0.symtab\0.strtab\0.rela.text\0.modinfo\0.shstrtab\0", 53);

    kb_elf_file_t parsed_elf;
    if (kb_elf_open(elf, sizeof(elf), &parsed_elf) != KB_OK) {
        kb_device_backend_destroy(backend);
        return 12;
    }
    if (kb_elf_section_count(&parsed_elf) != 7) {
        kb_device_backend_destroy(backend);
        return 13;
    }
    kb_elf_section_t section;
    if (kb_elf_section(&parsed_elf, 1, &section) != KB_OK) {
        kb_device_backend_destroy(backend);
        return 14;
    }
    if (strcmp(section.name, ".text") != 0 || section.type != KB_ELF_SHT_PROGBITS || section.size != 0x20) {
        kb_device_backend_destroy(backend);
        return 15;
    }
    size_t symbol_count = 0;
    if (kb_elf_symbol_count(&parsed_elf, 2, &symbol_count) != KB_OK || symbol_count != 3) {
        kb_device_backend_destroy(backend);
        return 16;
    }
    kb_elf_symbol_t symbol;
    if (kb_elf_symbol(&parsed_elf, 2, 1, &symbol) != KB_OK) {
        kb_device_backend_destroy(backend);
        return 17;
    }
    if (strcmp(symbol.name, "init_module") != 0 ||
        symbol.binding != KB_ELF_STB_GLOBAL ||
        symbol.type != KB_ELF_STT_FUNC ||
        symbol.section_index != 1)
    {
        kb_device_backend_destroy(backend);
        return 18;
    }
    if (kb_elf_symbol(&parsed_elf, 2, 2, &symbol) != KB_OK) {
        kb_device_backend_destroy(backend);
        return 19;
    }
    if (strcmp(symbol.name, "printk") != 0 || symbol.section_index != KB_ELF_SHN_UNDEF) {
        kb_device_backend_destroy(backend);
        return 20;
    }
    size_t relocation_count = 0;
    if (kb_elf_relocation_count(&parsed_elf, 4, &relocation_count) != KB_OK || relocation_count != 1) {
        kb_device_backend_destroy(backend);
        return 21;
    }
    kb_elf_relocation_t relocation;
    if (kb_elf_relocation(&parsed_elf, 4, 0, &relocation) != KB_OK) {
        kb_device_backend_destroy(backend);
        return 22;
    }
    if (relocation.offset != 4 ||
        relocation.type != KB_ELF_R_X86_64_PLT32 ||
        relocation.symbol_index != 2 ||
        relocation.addend != -4 ||
        relocation.target_section_index != 1 ||
        relocation.symbol_table_section_index != 2)
    {
        kb_device_backend_destroy(backend);
        return 23;
    }
    size_t modinfo_section = 0;
    if (kb_elf_modinfo_section(&parsed_elf, &modinfo_section) != KB_OK || modinfo_section != 5) {
        kb_device_backend_destroy(backend);
        return 24;
    }
    size_t modinfo_count = 0;
    if (kb_elf_modinfo_entry_count(&parsed_elf, modinfo_section, &modinfo_count) != KB_OK || modinfo_count != 2) {
        kb_device_backend_destroy(backend);
        return 25;
    }
    kb_elf_modinfo_entry_t modinfo_entry;
    if (kb_elf_modinfo_entry(&parsed_elf, modinfo_section, 0, &modinfo_entry) != KB_OK) {
        kb_device_backend_destroy(backend);
        return 26;
    }
    if (modinfo_entry.key_size != 8 ||
        strncmp(modinfo_entry.key, "vermagic", modinfo_entry.key_size) != 0 ||
        modinfo_entry.value_size != 10 ||
        strncmp(modinfo_entry.value, "6.6.0-test", modinfo_entry.value_size) != 0)
    {
        kb_device_backend_destroy(backend);
        return 27;
    }
    if (kb_elf_modinfo_entry(&parsed_elf, modinfo_section, 1, &modinfo_entry) != KB_OK) {
        kb_device_backend_destroy(backend);
        return 28;
    }
    if (modinfo_entry.key_size != 7 ||
        strncmp(modinfo_entry.key, "depends", modinfo_entry.key_size) != 0 ||
        modinfo_entry.value_size != 7 ||
        strncmp(modinfo_entry.value, "foo,bar", modinfo_entry.value_size) != 0)
    {
        kb_device_backend_destroy(backend);
        return 29;
    }

    int executable_status = test_executable_module(backend);
    if (executable_status != 0) {
        kb_device_backend_destroy(backend);
        return 30 + executable_status;
    }

    if (test_platform_facets() != 0) {
        kb_device_backend_destroy(backend);
        return 40;
    }

    if (test_linux_host_interfaces() != 0) {
        kb_device_backend_destroy(backend);
        return 41;
    }

    if (test_nvme_scattered_prps() != 0) {
        kb_device_backend_destroy(backend);
        return 42;
    }

    if (test_net_backend_and_page_lifetime(backend) != 0) {
        kb_device_backend_destroy(backend);
        return 43;
    }

    kb_device_backend_destroy(backend);
    return 0;
}
