#include "../src/linux_personality/linux_pci.c"

static kb_status_t routed_vector(kb_device_t *device, unsigned entry, uint32_t *out_vector)
{
    (void)device;
    if (out_vector == NULL || entry >= KB_DEVICE_ROUTED_MSIX_ENTRY_COUNT) {
        return KB_ERR_INVALID;
    }
    *out_vector = 0x60u + entry;
    return KB_OK;
}

int main(void)
{
    uint32_t table_words[32];
    for (size_t i = 0; i < sizeof(table_words) / sizeof(table_words[0]); i++) {
        table_words[i] = UINT32_C(0xaaaaaaaa);
    }
    const uint16_t routed_entries[] = {2, 7};
    table_words[2 * 4 + 3] = KB_PCI_MSIX_ENTRY_CTRL_MASKED;
    table_words[7 * 4 + 3] = KB_PCI_MSIX_ENTRY_CTRL_MASKED;
    const kb_mmio_region_t region = {
        .addr = table_words,
        .size = sizeof(table_words),
    };
    const kb_device_backend_ops_t routed_ops = {
        .msix_delivery_vector = routed_vector,
    };
    kb_device_t *const device = (kb_device_t *)(uintptr_t)1;

    if (program_msix_table_entries(
            device, &routed_ops, &region, 0, routed_entries, 2) != 0)
    {
        return 1;
    }
    if (table_words[2 * 4] != UINT32_C(0xfee00000) ||
        table_words[2 * 4 + 1] != 0 ||
        table_words[2 * 4 + 2] != 0x62 ||
        table_words[2 * 4 + 3] != 0 ||
        table_words[7 * 4] != UINT32_C(0xfee00000) ||
        table_words[7 * 4 + 1] != 0 ||
        table_words[7 * 4 + 2] != 0x67 ||
        table_words[7 * 4 + 3] != 0)
    {
        return 2;
    }

    const uint16_t invalid_entry = KB_DEVICE_ROUTED_MSIX_ENTRY_COUNT;
    if (program_msix_table_entries(
            device, &routed_ops, &region, 0, &invalid_entry, 1) != -22)
    {
        return 3;
    }

    const kb_device_backend_ops_t conventional_ops = {0};
    const uint16_t conventional_entry = 1;
    table_words[4] = UINT32_C(0x11111111);
    table_words[5] = UINT32_C(0x22222222);
    table_words[6] = UINT32_C(0x33333333);
    table_words[7] = KB_PCI_MSIX_ENTRY_CTRL_MASKED;
    if (program_msix_table_entries(
            device, &conventional_ops, &region, 0, &conventional_entry, 1) != 0)
    {
        return 4;
    }
    if (table_words[4] != UINT32_C(0x11111111) ||
        table_words[5] != UINT32_C(0x22222222) ||
        table_words[6] != UINT32_C(0x33333333) ||
        table_words[7] != 0)
    {
        return 5;
    }
    return 0;
}
