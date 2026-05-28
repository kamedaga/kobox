#include "kobox/elf.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
    elf_ident_size = 16,
    elf64_header_size = 64,
    elf64_section_header_size = 64,
    elf64_symbol_size = 24,
    elf64_rela_size = 24,
    elf64_rel_size = 16,
    elf_class_64 = 2,
    elf_data_little = 1,
    elf_version_current = 1,
    shn_xindex = 0xffff,
};

static uint16_t read_u16le(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_u32le(const uint8_t *p)
{
    return (uint32_t)p[0] |
        ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) |
        ((uint32_t)p[3] << 24);
}

static uint64_t read_u64le(const uint8_t *p)
{
    return (uint64_t)read_u32le(p) | ((uint64_t)read_u32le(p + 4) << 32);
}

static int range_fits(size_t size, uint64_t offset, uint64_t length)
{
    if (offset > (uint64_t)size) {
        return 0;
    }
    if (length > (uint64_t)size - offset) {
        return 0;
    }
    return 1;
}

static const char *bounded_section_name(
    const uint8_t *strings,
    uint64_t strings_size,
    uint32_t offset)
{
    uint64_t pos = offset;
    if (pos >= strings_size) {
        return "";
    }
    while (pos < strings_size) {
        if (strings[pos] == 0) {
            return (const char *)(strings + offset);
        }
        pos++;
    }
    return "";
}

static int section_bytes(const kb_elf_file_t *file, size_t section_index, const uint8_t **out_bytes, uint64_t *out_size)
{
    kb_elf_section_t section;
    if (kb_elf_section(file, section_index, &section) != KB_OK) {
        return 0;
    }
    if (!range_fits(file->size, section.offset, section.size)) {
        return 0;
    }
    *out_bytes = file->data + section.offset;
    *out_size = section.size;
    return 1;
}

static int next_nul_entry(const uint8_t *bytes, uint64_t size, uint64_t *cursor, const char **out_entry, size_t *out_entry_size)
{
    while (*cursor < size && bytes[*cursor] == 0) {
        (*cursor)++;
    }
    if (*cursor >= size) {
        return 0;
    }

    const uint64_t start = *cursor;
    while (*cursor < size && bytes[*cursor] != 0) {
        (*cursor)++;
    }
    if (*cursor >= size) {
        return -1;
    }

    *out_entry = (const char *)(bytes + start);
    *out_entry_size = (size_t)(*cursor - start);
    (*cursor)++;
    return 1;
}

kb_status_t kb_elf_open(const void *data, size_t size, kb_elf_file_t *out_file)
{
    if (data == 0 || out_file == 0 || size < elf64_header_size) {
        return KB_ERR_INVALID;
    }

    const uint8_t *bytes = (const uint8_t *)data;
    if (bytes[0] != 0x7f || bytes[1] != 'E' || bytes[2] != 'L' || bytes[3] != 'F') {
        return KB_ERR_INVALID;
    }
    if (bytes[4] != elf_class_64 || bytes[5] != elf_data_little || bytes[6] != elf_version_current) {
        return KB_ERR_UNSUPPORTED;
    }

    const uint16_t section_entry_size = read_u16le(bytes + 58);
    const uint16_t section_count = read_u16le(bytes + 60);
    const uint16_t section_name_index = read_u16le(bytes + 62);
    const uint64_t section_offset = read_u64le(bytes + 40);

    if (read_u16le(bytes + 52) != elf64_header_size) {
        return KB_ERR_UNSUPPORTED;
    }
    if (section_entry_size != elf64_section_header_size) {
        return KB_ERR_UNSUPPORTED;
    }
    if (section_count == 0 || section_name_index == shn_xindex || section_name_index >= section_count) {
        return KB_ERR_UNSUPPORTED;
    }
    if (!range_fits(size, section_offset, (uint64_t)section_entry_size * section_count)) {
        return KB_ERR_INVALID;
    }

    out_file->data = bytes;
    out_file->size = size;
    out_file->type = read_u16le(bytes + 16);
    out_file->machine = read_u16le(bytes + 18);
    out_file->entry = read_u64le(bytes + 24);
    out_file->section_header_offset = section_offset;
    out_file->section_header_entry_size = section_entry_size;
    out_file->section_count = section_count;
    out_file->section_name_index = section_name_index;
    return KB_OK;
}

size_t kb_elf_section_count(const kb_elf_file_t *file)
{
    if (file == 0) {
        return 0;
    }
    return file->section_count;
}

kb_status_t kb_elf_section(const kb_elf_file_t *file, size_t index, kb_elf_section_t *out_section)
{
    if (file == 0 || out_section == 0 || index >= file->section_count) {
        return KB_ERR_INVALID;
    }

    const uint64_t section_offset =
        file->section_header_offset + ((uint64_t)index * file->section_header_entry_size);
    if (!range_fits(file->size, section_offset, elf64_section_header_size)) {
        return KB_ERR_INVALID;
    }

    const uint64_t name_section_offset =
        file->section_header_offset + ((uint64_t)file->section_name_index * file->section_header_entry_size);
    if (!range_fits(file->size, name_section_offset, elf64_section_header_size)) {
        return KB_ERR_INVALID;
    }

    const uint8_t *section = file->data + section_offset;
    const uint8_t *name_section = file->data + name_section_offset;
    const uint64_t strings_offset = read_u64le(name_section + 24);
    const uint64_t strings_size = read_u64le(name_section + 32);

    const char *name = "";
    if (range_fits(file->size, strings_offset, strings_size)) {
        name = bounded_section_name(file->data + strings_offset, strings_size, read_u32le(section));
    }

    out_section->name = name;
    out_section->type = read_u32le(section + 4);
    out_section->flags = read_u64le(section + 8);
    out_section->address = read_u64le(section + 16);
    out_section->offset = read_u64le(section + 24);
    out_section->size = read_u64le(section + 32);
    out_section->link = read_u32le(section + 40);
    out_section->info = read_u32le(section + 44);
    out_section->alignment = read_u64le(section + 48);
    out_section->entry_size = read_u64le(section + 56);
    return KB_OK;
}

kb_status_t kb_elf_symbol_count(const kb_elf_file_t *file, size_t section_index, size_t *out_count)
{
    if (file == 0 || out_count == 0) {
        return KB_ERR_INVALID;
    }

    kb_elf_section_t section;
    kb_status_t status = kb_elf_section(file, section_index, &section);
    if (status != KB_OK) {
        return status;
    }
    if (section.type != KB_ELF_SHT_SYMTAB && section.type != KB_ELF_SHT_DYNSYM) {
        return KB_ERR_INVALID;
    }
    if (section.entry_size != elf64_symbol_size || section.size % elf64_symbol_size != 0) {
        return KB_ERR_UNSUPPORTED;
    }
    if (!range_fits(file->size, section.offset, section.size)) {
        return KB_ERR_INVALID;
    }

    *out_count = (size_t)(section.size / elf64_symbol_size);
    return KB_OK;
}

kb_status_t kb_elf_symbol(
    const kb_elf_file_t *file,
    size_t section_index,
    size_t symbol_index,
    kb_elf_symbol_t *out_symbol)
{
    if (file == 0 || out_symbol == 0) {
        return KB_ERR_INVALID;
    }

    kb_elf_section_t symbol_section;
    kb_status_t status = kb_elf_section(file, section_index, &symbol_section);
    if (status != KB_OK) {
        return status;
    }
    if (symbol_section.type != KB_ELF_SHT_SYMTAB && symbol_section.type != KB_ELF_SHT_DYNSYM) {
        return KB_ERR_INVALID;
    }
    if (symbol_section.entry_size != elf64_symbol_size || symbol_section.size % elf64_symbol_size != 0) {
        return KB_ERR_UNSUPPORTED;
    }
    if (symbol_index >= (size_t)(symbol_section.size / elf64_symbol_size)) {
        return KB_ERR_INVALID;
    }
    if (symbol_section.link >= file->section_count) {
        return KB_ERR_INVALID;
    }

    kb_elf_section_t string_section;
    status = kb_elf_section(file, symbol_section.link, &string_section);
    if (status != KB_OK) {
        return status;
    }
    if (string_section.type != KB_ELF_SHT_STRTAB) {
        return KB_ERR_INVALID;
    }
    if (!range_fits(file->size, string_section.offset, string_section.size)) {
        return KB_ERR_INVALID;
    }

    const uint64_t symbol_offset = symbol_section.offset + ((uint64_t)symbol_index * elf64_symbol_size);
    if (!range_fits(file->size, symbol_offset, elf64_symbol_size)) {
        return KB_ERR_INVALID;
    }

    const uint8_t *symbol = file->data + symbol_offset;
    const uint32_t name_offset = read_u32le(symbol);
    const uint8_t info = symbol[4];
    out_symbol->name = bounded_section_name(file->data + string_section.offset, string_section.size, name_offset);
    out_symbol->binding = info >> 4;
    out_symbol->type = info & 0x0f;
    out_symbol->visibility = symbol[5] & 0x03;
    out_symbol->section_index = read_u16le(symbol + 6);
    out_symbol->value = read_u64le(symbol + 8);
    out_symbol->size = read_u64le(symbol + 16);
    return KB_OK;
}

kb_status_t kb_elf_relocation_count(const kb_elf_file_t *file, size_t section_index, size_t *out_count)
{
    if (file == 0 || out_count == 0) {
        return KB_ERR_INVALID;
    }

    kb_elf_section_t section;
    kb_status_t status = kb_elf_section(file, section_index, &section);
    if (status != KB_OK) {
        return status;
    }
    if (section.type != KB_ELF_SHT_RELA && section.type != KB_ELF_SHT_REL) {
        return KB_ERR_INVALID;
    }

    const uint64_t expected_entry_size = section.type == KB_ELF_SHT_RELA ? elf64_rela_size : elf64_rel_size;
    if (section.entry_size != expected_entry_size || section.size % expected_entry_size != 0) {
        return KB_ERR_UNSUPPORTED;
    }
    if (!range_fits(file->size, section.offset, section.size)) {
        return KB_ERR_INVALID;
    }

    *out_count = (size_t)(section.size / expected_entry_size);
    return KB_OK;
}

kb_status_t kb_elf_relocation(
    const kb_elf_file_t *file,
    size_t section_index,
    size_t relocation_index,
    kb_elf_relocation_t *out_relocation)
{
    if (file == 0 || out_relocation == 0) {
        return KB_ERR_INVALID;
    }

    kb_elf_section_t section;
    kb_status_t status = kb_elf_section(file, section_index, &section);
    if (status != KB_OK) {
        return status;
    }
    if (section.type != KB_ELF_SHT_RELA && section.type != KB_ELF_SHT_REL) {
        return KB_ERR_INVALID;
    }
    if (section.link >= file->section_count || section.info >= file->section_count) {
        return KB_ERR_INVALID;
    }

    const uint64_t expected_entry_size = section.type == KB_ELF_SHT_RELA ? elf64_rela_size : elf64_rel_size;
    if (section.entry_size != expected_entry_size || section.size % expected_entry_size != 0) {
        return KB_ERR_UNSUPPORTED;
    }
    if (relocation_index >= (size_t)(section.size / expected_entry_size)) {
        return KB_ERR_INVALID;
    }

    const uint64_t relocation_offset = section.offset + ((uint64_t)relocation_index * expected_entry_size);
    if (!range_fits(file->size, relocation_offset, expected_entry_size)) {
        return KB_ERR_INVALID;
    }

    const uint8_t *relocation = file->data + relocation_offset;
    const uint64_t info = read_u64le(relocation + 8);
    out_relocation->offset = read_u64le(relocation);
    out_relocation->type = (uint32_t)(info & 0xffffffffu);
    out_relocation->symbol_index = (uint32_t)(info >> 32);
    out_relocation->has_addend = section.type == KB_ELF_SHT_RELA;
    out_relocation->addend = out_relocation->has_addend ? (int64_t)read_u64le(relocation + 16) : 0;
    out_relocation->target_section_index = section.info;
    out_relocation->symbol_table_section_index = section.link;
    return KB_OK;
}

kb_status_t kb_elf_modinfo_section(const kb_elf_file_t *file, size_t *out_section_index)
{
    if (file == 0 || out_section_index == 0) {
        return KB_ERR_INVALID;
    }

    const size_t section_count = kb_elf_section_count(file);
    for (size_t i = 0; i < section_count; i++) {
        kb_elf_section_t section;
        kb_status_t status = kb_elf_section(file, i, &section);
        if (status != KB_OK) {
            return status;
        }
        if (section.type == KB_ELF_SHT_PROGBITS && strcmp(section.name, ".modinfo") == 0) {
            *out_section_index = i;
            return KB_OK;
        }
    }
    return KB_ERR_NOT_FOUND;
}

kb_status_t kb_elf_modinfo_entry_count(const kb_elf_file_t *file, size_t section_index, size_t *out_count)
{
    if (file == 0 || out_count == 0) {
        return KB_ERR_INVALID;
    }

    kb_elf_section_t section;
    kb_status_t status = kb_elf_section(file, section_index, &section);
    if (status != KB_OK) {
        return status;
    }
    if (strcmp(section.name, ".modinfo") != 0 || section.type != KB_ELF_SHT_PROGBITS) {
        return KB_ERR_INVALID;
    }

    const uint8_t *bytes = 0;
    uint64_t size = 0;
    if (!section_bytes(file, section_index, &bytes, &size)) {
        return KB_ERR_INVALID;
    }

    size_t count = 0;
    uint64_t cursor = 0;
    while (cursor < size) {
        const char *entry = 0;
        size_t entry_size = 0;
        const int result = next_nul_entry(bytes, size, &cursor, &entry, &entry_size);
        if (result < 0) {
            return KB_ERR_INVALID;
        }
        if (result == 0) {
            break;
        }
        (void)entry;
        count++;
    }

    *out_count = count;
    return KB_OK;
}

kb_status_t kb_elf_modinfo_entry(
    const kb_elf_file_t *file,
    size_t section_index,
    size_t entry_index,
    kb_elf_modinfo_entry_t *out_entry)
{
    if (file == 0 || out_entry == 0) {
        return KB_ERR_INVALID;
    }

    kb_elf_section_t section;
    kb_status_t status = kb_elf_section(file, section_index, &section);
    if (status != KB_OK) {
        return status;
    }
    if (strcmp(section.name, ".modinfo") != 0 || section.type != KB_ELF_SHT_PROGBITS) {
        return KB_ERR_INVALID;
    }

    const uint8_t *bytes = 0;
    uint64_t size = 0;
    if (!section_bytes(file, section_index, &bytes, &size)) {
        return KB_ERR_INVALID;
    }

    uint64_t cursor = 0;
    size_t current_index = 0;
    while (cursor < size) {
        const char *entry = 0;
        size_t entry_size = 0;
        const int result = next_nul_entry(bytes, size, &cursor, &entry, &entry_size);
        if (result < 0) {
            return KB_ERR_INVALID;
        }
        if (result == 0) {
            break;
        }
        if (current_index != entry_index) {
            current_index++;
            continue;
        }

        size_t equals_index = 0;
        while (equals_index < entry_size && entry[equals_index] != '=') {
            equals_index++;
        }
        if (equals_index == entry_size) {
            return KB_ERR_INVALID;
        }

        out_entry->key = entry;
        out_entry->key_size = equals_index;
        out_entry->value = entry + equals_index + 1;
        out_entry->value_size = entry_size - equals_index - 1;
        return KB_OK;
    }

    return KB_ERR_NOT_FOUND;
}

const char *kb_elf_type_name(uint16_t type)
{
    switch (type) {
    case KB_ELF_ET_NONE:
        return "NONE";
    case KB_ELF_ET_REL:
        return "REL";
    case KB_ELF_ET_EXEC:
        return "EXEC";
    case KB_ELF_ET_DYN:
        return "DYN";
    default:
        return "UNKNOWN";
    }
}

const char *kb_elf_machine_name(uint16_t machine)
{
    switch (machine) {
    case KB_ELF_EM_X86_64:
        return "x86_64";
    default:
        return "unknown";
    }
}

const char *kb_elf_section_type_name(uint32_t type)
{
    switch (type) {
    case KB_ELF_SHT_NULL:
        return "NULL";
    case KB_ELF_SHT_PROGBITS:
        return "PROGBITS";
    case KB_ELF_SHT_SYMTAB:
        return "SYMTAB";
    case KB_ELF_SHT_STRTAB:
        return "STRTAB";
    case KB_ELF_SHT_RELA:
        return "RELA";
    case KB_ELF_SHT_NOBITS:
        return "NOBITS";
    case KB_ELF_SHT_REL:
        return "REL";
    case KB_ELF_SHT_DYNSYM:
        return "DYNSYM";
    default:
        return "UNKNOWN";
    }
}

const char *kb_elf_symbol_binding_name(uint8_t binding)
{
    switch (binding) {
    case KB_ELF_STB_LOCAL:
        return "LOCAL";
    case KB_ELF_STB_GLOBAL:
        return "GLOBAL";
    case KB_ELF_STB_WEAK:
        return "WEAK";
    default:
        return "UNKNOWN";
    }
}

const char *kb_elf_symbol_type_name(uint8_t type)
{
    switch (type) {
    case KB_ELF_STT_NOTYPE:
        return "NOTYPE";
    case KB_ELF_STT_OBJECT:
        return "OBJECT";
    case KB_ELF_STT_FUNC:
        return "FUNC";
    case KB_ELF_STT_SECTION:
        return "SECTION";
    case KB_ELF_STT_FILE:
        return "FILE";
    case KB_ELF_STT_COMMON:
        return "COMMON";
    case KB_ELF_STT_TLS:
        return "TLS";
    default:
        return "UNKNOWN";
    }
}

const char *kb_elf_x86_64_relocation_type_name(uint32_t type)
{
    switch (type) {
    case KB_ELF_R_X86_64_NONE:
        return "R_X86_64_NONE";
    case KB_ELF_R_X86_64_64:
        return "R_X86_64_64";
    case KB_ELF_R_X86_64_PC32:
        return "R_X86_64_PC32";
    case KB_ELF_R_X86_64_GOT32:
        return "R_X86_64_GOT32";
    case KB_ELF_R_X86_64_PLT32:
        return "R_X86_64_PLT32";
    case KB_ELF_R_X86_64_GLOB_DAT:
        return "R_X86_64_GLOB_DAT";
    case KB_ELF_R_X86_64_JUMP_SLOT:
        return "R_X86_64_JUMP_SLOT";
    case KB_ELF_R_X86_64_RELATIVE:
        return "R_X86_64_RELATIVE";
    case KB_ELF_R_X86_64_GOTPCREL:
        return "R_X86_64_GOTPCREL";
    case KB_ELF_R_X86_64_32:
        return "R_X86_64_32";
    case KB_ELF_R_X86_64_32S:
        return "R_X86_64_32S";
    case KB_ELF_R_X86_64_PC64:
        return "R_X86_64_PC64";
    case KB_ELF_R_X86_64_GOTPCRELX:
        return "R_X86_64_GOTPCRELX";
    case KB_ELF_R_X86_64_REX_GOTPCRELX:
        return "R_X86_64_REX_GOTPCRELX";
    default:
        return "R_X86_64_UNKNOWN";
    }
}
