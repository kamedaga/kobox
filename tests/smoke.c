#include "kobox/backend.h"
#include "kobox/backend_linux_mock.h"
#include "kobox/elf.h"
#include "kobox/module.h"
#include "kobox/shim.h"

#include <stdint.h>
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

static int test_executable_module(kb_backend_t *backend)
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

int main(void)
{
    kb_backend_t *backend = 0;
    if (kb_linux_mock_create(&backend) != KB_OK || backend == 0) {
        return 1;
    }

    const kb_backend_ops_t *ops = kb_backend_get_ops(backend);
    if (ops == 0) {
        kb_backend_destroy(backend);
        return 2;
    }

    size_t count = 0;
    if (ops->device_count(backend, &count) != KB_OK || count != 1) {
        kb_backend_destroy(backend);
        return 3;
    }

    kb_device_t *device = 0;
    if (ops->device_at(backend, 0, &device) != KB_OK || device == 0) {
        kb_backend_destroy(backend);
        return 4;
    }

    kb_dma_buffer_t dma;
    memset(&dma, 0, sizeof(dma));
    if (ops->dma_alloc(device, 4096, 4096, KB_DMA_BIDIRECTIONAL, &dma) != KB_OK) {
        kb_backend_destroy(backend);
        return 5;
    }
    if (dma.cpu_addr == 0 || dma.dma_addr == 0 || dma.size != 4096) {
        ops->dma_free(device, &dma);
        kb_backend_destroy(backend);
        return 6;
    }
    ops->dma_free(device, &dma);

    int called = 0;
    kb_irq_t *irq = 0;
    if (ops->irq_register(device, 0, irq_callback, &called, &irq) != KB_OK) {
        kb_backend_destroy(backend);
        return 7;
    }
    if (ops->irq_wait(device, irq, 0) != KB_OK || called != 1) {
        ops->irq_unregister(device, irq);
        kb_backend_destroy(backend);
        return 8;
    }
    ops->irq_unregister(device, irq);

    void *mem = kb_kzalloc(16, 0);
    if (mem == 0) {
        kb_backend_destroy(backend);
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
        kb_backend_destroy(backend);
        return 10;
    }
    if (module != 0) {
        kb_backend_destroy(backend);
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
        kb_backend_destroy(backend);
        return 12;
    }
    if (kb_elf_section_count(&parsed_elf) != 7) {
        kb_backend_destroy(backend);
        return 13;
    }
    kb_elf_section_t section;
    if (kb_elf_section(&parsed_elf, 1, &section) != KB_OK) {
        kb_backend_destroy(backend);
        return 14;
    }
    if (strcmp(section.name, ".text") != 0 || section.type != KB_ELF_SHT_PROGBITS || section.size != 0x20) {
        kb_backend_destroy(backend);
        return 15;
    }
    size_t symbol_count = 0;
    if (kb_elf_symbol_count(&parsed_elf, 2, &symbol_count) != KB_OK || symbol_count != 3) {
        kb_backend_destroy(backend);
        return 16;
    }
    kb_elf_symbol_t symbol;
    if (kb_elf_symbol(&parsed_elf, 2, 1, &symbol) != KB_OK) {
        kb_backend_destroy(backend);
        return 17;
    }
    if (strcmp(symbol.name, "init_module") != 0 ||
        symbol.binding != KB_ELF_STB_GLOBAL ||
        symbol.type != KB_ELF_STT_FUNC ||
        symbol.section_index != 1)
    {
        kb_backend_destroy(backend);
        return 18;
    }
    if (kb_elf_symbol(&parsed_elf, 2, 2, &symbol) != KB_OK) {
        kb_backend_destroy(backend);
        return 19;
    }
    if (strcmp(symbol.name, "printk") != 0 || symbol.section_index != KB_ELF_SHN_UNDEF) {
        kb_backend_destroy(backend);
        return 20;
    }
    size_t relocation_count = 0;
    if (kb_elf_relocation_count(&parsed_elf, 4, &relocation_count) != KB_OK || relocation_count != 1) {
        kb_backend_destroy(backend);
        return 21;
    }
    kb_elf_relocation_t relocation;
    if (kb_elf_relocation(&parsed_elf, 4, 0, &relocation) != KB_OK) {
        kb_backend_destroy(backend);
        return 22;
    }
    if (relocation.offset != 4 ||
        relocation.type != KB_ELF_R_X86_64_PLT32 ||
        relocation.symbol_index != 2 ||
        relocation.addend != -4 ||
        relocation.target_section_index != 1 ||
        relocation.symbol_table_section_index != 2)
    {
        kb_backend_destroy(backend);
        return 23;
    }
    size_t modinfo_section = 0;
    if (kb_elf_modinfo_section(&parsed_elf, &modinfo_section) != KB_OK || modinfo_section != 5) {
        kb_backend_destroy(backend);
        return 24;
    }
    size_t modinfo_count = 0;
    if (kb_elf_modinfo_entry_count(&parsed_elf, modinfo_section, &modinfo_count) != KB_OK || modinfo_count != 2) {
        kb_backend_destroy(backend);
        return 25;
    }
    kb_elf_modinfo_entry_t modinfo_entry;
    if (kb_elf_modinfo_entry(&parsed_elf, modinfo_section, 0, &modinfo_entry) != KB_OK) {
        kb_backend_destroy(backend);
        return 26;
    }
    if (modinfo_entry.key_size != 8 ||
        strncmp(modinfo_entry.key, "vermagic", modinfo_entry.key_size) != 0 ||
        modinfo_entry.value_size != 10 ||
        strncmp(modinfo_entry.value, "6.6.0-test", modinfo_entry.value_size) != 0)
    {
        kb_backend_destroy(backend);
        return 27;
    }
    if (kb_elf_modinfo_entry(&parsed_elf, modinfo_section, 1, &modinfo_entry) != KB_OK) {
        kb_backend_destroy(backend);
        return 28;
    }
    if (modinfo_entry.key_size != 7 ||
        strncmp(modinfo_entry.key, "depends", modinfo_entry.key_size) != 0 ||
        modinfo_entry.value_size != 7 ||
        strncmp(modinfo_entry.value, "foo,bar", modinfo_entry.value_size) != 0)
    {
        kb_backend_destroy(backend);
        return 29;
    }

    int executable_status = test_executable_module(backend);
    if (executable_status != 0) {
        kb_backend_destroy(backend);
        return 30 + executable_status;
    }

    kb_backend_destroy(backend);
    return 0;
}
