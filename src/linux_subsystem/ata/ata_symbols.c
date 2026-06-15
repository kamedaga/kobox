#include "linux_subsystem/ata/ata_symbols.h"
#include "kobox/shim.h"
#include "linux_subsystem/ata/ata.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    KB_ATA_HOST_DEV_OFFSET = 0x08,
    KB_ATA_HOST_N_PORTS_OFFSET = 0x18,
    KB_ATA_HOST_PORTS_OFFSET = 0x70,
    KB_ATA_HOST_HPRIV_OFFSET = 0x20,
    KB_ATA_PORT_OPS_OFFSET = 0x08,
    KB_ATA_PORT_FLAGS_OFFSET = 0x18,
    KB_ATA_PORT_PORT_NO_OFFSET = 0x2c,
    KB_ATA_PORT_PRIVATE_DATA_OFFSET = 0x3a78,
    KB_ATA_PORT_PRINT_ID_OFFSET = 0x3eb4,
    KB_ATA_PORT_INFO_FLAGS_OFFSET = 0x00,
    KB_ATA_PORT_INFO_OPS_OFFSET = 0x20,
    KB_ATA_PORT_BYTES = 0x4100,
};

static unsigned char kb_ata_dummy_port_ops[512];
static unsigned char kb_sata_pmp_port_ops[512];
static unsigned char kb_sata_deb_timing_hotplug[64];
static unsigned char kb_sata_deb_timing_normal[64];

static unsigned char kb_dev_attr_em_message[128];
static unsigned char kb_dev_attr_em_message_type[128];
static unsigned char kb_dev_attr_link_power_management_policy[128];
static unsigned char kb_dev_attr_link_power_management_supported[128];
static unsigned char kb_dev_attr_ncq_prio_enable[128];
static unsigned char kb_dev_attr_ncq_prio_supported[128];
static unsigned char kb_dev_attr_sw_activity[128];
static unsigned char kb_dev_attr_unload_heads[128];

static void write_u32_local(unsigned char *base, size_t offset, uint32_t value)
{
    memcpy(base + offset, &value, sizeof(value));
}

static void write_ptr_local(unsigned char *base, size_t offset, const void *value)
{
    uintptr_t raw = (uintptr_t)value;
    memcpy(base + offset, &raw, sizeof(raw));
}

static uint32_t read_u32_local(const unsigned char *base, size_t offset)
{
    uint32_t value = 0;
    memcpy(&value, base + offset, sizeof(value));
    return value;
}

static void *read_ptr_local(const unsigned char *base, size_t offset)
{
    uintptr_t raw = 0;
    memcpy(&raw, base + offset, sizeof(raw));
    return (void *)raw;
}

static int trace_ata_enabled(void)
{
    const char *value = getenv("KOBOX_TRACE_ATA");
    return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
}

static void *kb_ata_host_alloc_pinfo(void *dev, const void *ppi, int n_ports)
{
    if (n_ports <= 0 || n_ports > 32) {
        return NULL;
    }

    const size_t host_bytes = KB_ATA_HOST_PORTS_OFFSET + ((size_t)n_ports * sizeof(uintptr_t));
    unsigned char *host = calloc(1, host_bytes);
    if (host == NULL) {
        return NULL;
    }
    write_ptr_local(host, KB_ATA_HOST_DEV_OFFSET, dev);
    write_u32_local(host, KB_ATA_HOST_N_PORTS_OFFSET, (uint32_t)n_ports);
    write_ptr_local(host, KB_ATA_HOST_HPRIV_OFFSET, NULL);

    const void *first_port_info = ppi == NULL ? NULL : read_ptr_local(ppi, 0);
    for (int i = 0; i < n_ports; i++) {
        unsigned char *port = calloc(1, KB_ATA_PORT_BYTES);
        if (port == NULL) {
            for (int j = 0; j < i; j++) {
                uintptr_t raw = 0;
                memcpy(&raw, host + KB_ATA_HOST_PORTS_OFFSET + ((size_t)j * sizeof(raw)), sizeof(raw));
                free((void *)raw);
            }
            free(host);
            return NULL;
        }
        const void *port_info = first_port_info;
        if (ppi != NULL) {
            void *indexed = read_ptr_local(ppi, (size_t)i * sizeof(uintptr_t));
            if (indexed != NULL) {
                port_info = indexed;
            }
        }
        void *port_ops = port_info == NULL ? NULL : read_ptr_local(port_info, KB_ATA_PORT_INFO_OPS_OFFSET);
        uint32_t flags = port_info == NULL ? 0 : read_u32_local(port_info, KB_ATA_PORT_INFO_FLAGS_OFFSET);
        write_ptr_local(port, KB_ATA_PORT_OPS_OFFSET, port_ops == NULL ? kb_ata_dummy_port_ops : port_ops);
        write_u32_local(port, KB_ATA_PORT_FLAGS_OFFSET, flags);
        write_u32_local(port, KB_ATA_PORT_PORT_NO_OFFSET, (uint32_t)i);
        write_ptr_local(port, KB_ATA_PORT_PRIVATE_DATA_OFFSET, host);
        write_u32_local(port, KB_ATA_PORT_PRINT_ID_OFFSET, (uint32_t)i);
        write_ptr_local(host, KB_ATA_HOST_PORTS_OFFSET + ((size_t)i * sizeof(uintptr_t)), port);
    }
    return host;
}

static int kb_ata_host_start(void *host);
static int kb_ata_host_register(void *host, const void *sht);

static int kb_ata_host_activate(
    void *host,
    int irq,
    int (*irq_handler)(int, void *),
    unsigned long irq_flags,
    const void *sht)
{
    if (host == NULL) {
        return -22;
    }

    const unsigned char *host_bytes = host;
    uint32_t n_ports = read_u32_local(host_bytes, KB_ATA_HOST_N_PORTS_OFFSET);
    void *dev = read_ptr_local(host_bytes, KB_ATA_HOST_DEV_OFFSET);
    void *hpriv = read_ptr_local(host_bytes, KB_ATA_HOST_HPRIV_OFFSET);
    if (n_ports == 0 || n_ports > 32 || hpriv == NULL) {
        return -22;
    }

    unsigned active_ports = 0;
    for (uint32_t i = 0; i < n_ports; i++) {
        unsigned char *port = read_ptr_local(host_bytes, KB_ATA_HOST_PORTS_OFFSET + ((size_t)i * sizeof(uintptr_t)));
        if (port == NULL) {
            return -22;
        }
        if (read_ptr_local(port, KB_ATA_PORT_OPS_OFFSET) != kb_ata_dummy_port_ops) {
            active_ports++;
        }
    }

    if (trace_ata_enabled()) {
        fprintf(
            stderr,
            "kobox ata: host_activate host=%p dev=%p hpriv=%p n_ports=%u active_ports=%u irq=%d flags=0x%lx\n",
            host,
            dev,
            hpriv,
            n_ports,
            active_ports,
            irq,
            irq_flags);
    }
    if (irq_handler != NULL) {
        int irq_result = kb_request_threaded_irq(
            (unsigned int)irq,
            irq_handler,
            NULL,
            irq_flags,
            "kobox-ata",
            host);
        if (irq_result != 0) {
            return irq_result;
        }
    }
    int status = kb_ata_host_start(host);
    if (status != 0) {
        return status;
    }
    return kb_ata_host_register(host, sht);
}

static int kb_ata_host_start(void *host)
{
    if (host == NULL) {
        return -22;
    }
    if (trace_ata_enabled()) {
        fprintf(stderr, "kobox ata: host_start host=%p\n", host);
    }
    return 0;
}

static int kb_ata_host_register(void *host, const void *sht)
{
    if (host == NULL) {
        return -22;
    }
    int result = kb_ata_subsystem_register_host(host, sht);
    if (result != 0) {
        return result;
    }
    if (trace_ata_enabled()) {
        fprintf(stderr, "kobox ata: host_register host=%p\n", host);
    }
    return 0;
}

static int kb_ata_scsi_queuecmd(void *scsi_host, void *scmd)
{
    return kb_ata_subsystem_queue_scsi_command(scsi_host, scmd);
}

static const kb_linux_symbol_t symbols[] = {
    {"ata_dev_next", (void *)(uintptr_t)&kb_return_zero},
    {"ata_dev_set_feature", (void *)(uintptr_t)&kb_return_zero},
    {"ata_dummy_port_ops", (void *)(uintptr_t)&kb_ata_dummy_port_ops},
    {"ata_ehi_clear_desc", (void *)(uintptr_t)&kb_noop_stub},
    {"ata_ehi_push_desc", (void *)(uintptr_t)&kb_noop_stub},
    {"ata_host_activate", (void *)(uintptr_t)&kb_ata_host_activate},
    {"ata_host_alloc_pinfo", (void *)(uintptr_t)&kb_ata_host_alloc_pinfo},
    {"ata_host_register", (void *)(uintptr_t)&kb_ata_host_register},
    {"ata_host_resume", (void *)(uintptr_t)&kb_return_zero},
    {"ata_host_start", (void *)(uintptr_t)&kb_ata_host_start},
    {"ata_host_suspend", (void *)(uintptr_t)&kb_return_zero},
    {"ata_link_abort", (void *)(uintptr_t)&kb_noop_stub},
    {"ata_link_next", (void *)(uintptr_t)&kb_return_zero},
    {"ata_msleep", (void *)(uintptr_t)&kb_msleep},
    {"ata_pci_remove_one", (void *)(uintptr_t)&kb_noop_stub},
    {"ata_pci_shutdown_one", (void *)(uintptr_t)&kb_noop_stub},
    {"ata_port_abort", (void *)(uintptr_t)&kb_noop_stub},
    {"ata_port_classify", (void *)(uintptr_t)&kb_return_zero},
    {"ata_port_desc", (void *)(uintptr_t)&kb_noop_stub},
    {"ata_port_freeze", (void *)(uintptr_t)&kb_noop_stub},
    {"ata_port_pbar_desc", (void *)(uintptr_t)&kb_noop_stub},
    {"ata_print_version", (void *)(uintptr_t)&kb_noop_stub},
    {"ata_qc_complete_multiple", (void *)(uintptr_t)&kb_return_zero},
    {"ata_ratelimit", (void *)(uintptr_t)&kb_return_one},
    {"ata_scsi_change_queue_depth", (void *)(uintptr_t)&kb_return_zero},
    {"ata_scsi_dma_need_drain", (void *)(uintptr_t)&kb_return_zero},
    {"ata_scsi_ioctl", (void *)(uintptr_t)&kb_return_zero},
    {"ata_scsi_queuecmd", (void *)(uintptr_t)&kb_ata_scsi_queuecmd},
    {"ata_scsi_slave_alloc", (void *)(uintptr_t)&kb_return_zero},
    {"ata_scsi_slave_config", (void *)(uintptr_t)&kb_return_zero},
    {"ata_scsi_slave_destroy", (void *)(uintptr_t)&kb_noop_stub},
    {"ata_scsi_unlock_native_capacity", (void *)(uintptr_t)&kb_noop_stub},
    {"ata_std_bios_param", (void *)(uintptr_t)&kb_return_zero},
    {"ata_std_postreset", (void *)(uintptr_t)&kb_noop_stub},
    {"ata_std_qc_defer", (void *)(uintptr_t)&kb_return_zero},
    {"ata_tf_from_fis", (void *)(uintptr_t)&kb_noop_stub},
    {"ata_tf_to_fis", (void *)(uintptr_t)&kb_noop_stub},
    {"ata_wait_after_reset", (void *)(uintptr_t)&kb_return_zero},
    {"ata_wait_register", (void *)(uintptr_t)&kb_return_zero},
    {"dev_attr_em_message", (void *)(uintptr_t)&kb_dev_attr_em_message},
    {"dev_attr_em_message_type", (void *)(uintptr_t)&kb_dev_attr_em_message_type},
    {"dev_attr_link_power_management_policy", (void *)(uintptr_t)&kb_dev_attr_link_power_management_policy},
    {"dev_attr_link_power_management_supported", (void *)(uintptr_t)&kb_dev_attr_link_power_management_supported},
    {"dev_attr_ncq_prio_enable", (void *)(uintptr_t)&kb_dev_attr_ncq_prio_enable},
    {"dev_attr_ncq_prio_supported", (void *)(uintptr_t)&kb_dev_attr_ncq_prio_supported},
    {"dev_attr_sw_activity", (void *)(uintptr_t)&kb_dev_attr_sw_activity},
    {"dev_attr_unload_heads", (void *)(uintptr_t)&kb_dev_attr_unload_heads},
    {"sata_async_notification", (void *)(uintptr_t)&kb_noop_stub},
    {"sata_deb_timing_hotplug", (void *)(uintptr_t)&kb_sata_deb_timing_hotplug},
    {"sata_deb_timing_normal", (void *)(uintptr_t)&kb_sata_deb_timing_normal},
    {"sata_link_hardreset", (void *)(uintptr_t)&kb_return_zero},
    {"sata_link_scr_lpm", (void *)(uintptr_t)&kb_return_zero},
    {"sata_lpm_ignore_phy_events", (void *)(uintptr_t)&kb_return_zero},
    {"sata_pmp_error_handler", (void *)(uintptr_t)&kb_noop_stub},
    {"sata_pmp_port_ops", (void *)(uintptr_t)&kb_sata_pmp_port_ops},
    {"sata_pmp_qc_defer_cmd_switch", (void *)(uintptr_t)&kb_return_zero},
    {"sata_scr_read", (void *)(uintptr_t)&kb_return_zero},
};

const kb_linux_symbol_t *kb_linux_ata_symbols(size_t *out_count)
{
    if (out_count != NULL) {
        *out_count = sizeof(symbols) / sizeof(symbols[0]);
    }
    return symbols;
}
