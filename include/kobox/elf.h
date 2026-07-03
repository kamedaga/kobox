#pragma once

#include <stddef.h>
#include <stdint.h>
#include "kobox/device.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    KB_ELF_ET_NONE = 0,
    KB_ELF_ET_REL = 1,
    KB_ELF_ET_EXEC = 2,
    KB_ELF_ET_DYN = 3,

    KB_ELF_EM_X86_64 = 62,

    KB_ELF_SHT_NULL = 0,
    KB_ELF_SHT_PROGBITS = 1,
    KB_ELF_SHT_SYMTAB = 2,
    KB_ELF_SHT_STRTAB = 3,
    KB_ELF_SHT_RELA = 4,
    KB_ELF_SHT_NOBITS = 8,
    KB_ELF_SHT_REL = 9,
    KB_ELF_SHT_DYNSYM = 11,

    KB_ELF_SHF_WRITE = 1,
    KB_ELF_SHF_ALLOC = 2,
    KB_ELF_SHF_EXECINSTR = 4,

    KB_ELF_SHN_UNDEF = 0,
    KB_ELF_SHN_ABS = 0xfff1,

    KB_ELF_STB_LOCAL = 0,
    KB_ELF_STB_GLOBAL = 1,
    KB_ELF_STB_WEAK = 2,

    KB_ELF_STT_NOTYPE = 0,
    KB_ELF_STT_OBJECT = 1,
    KB_ELF_STT_FUNC = 2,
    KB_ELF_STT_SECTION = 3,
    KB_ELF_STT_FILE = 4,
    KB_ELF_STT_COMMON = 5,
    KB_ELF_STT_TLS = 6,

    KB_ELF_R_X86_64_NONE = 0,
    KB_ELF_R_X86_64_64 = 1,
    KB_ELF_R_X86_64_PC32 = 2,
    KB_ELF_R_X86_64_GOT32 = 3,
    KB_ELF_R_X86_64_PLT32 = 4,
    KB_ELF_R_X86_64_GLOB_DAT = 6,
    KB_ELF_R_X86_64_JUMP_SLOT = 7,
    KB_ELF_R_X86_64_RELATIVE = 8,
    KB_ELF_R_X86_64_GOTPCREL = 9,
    KB_ELF_R_X86_64_32 = 10,
    KB_ELF_R_X86_64_32S = 11,
    KB_ELF_R_X86_64_PC64 = 24,
    KB_ELF_R_X86_64_GOTPCRELX = 41,
    KB_ELF_R_X86_64_REX_GOTPCRELX = 42,
};

typedef struct kb_elf_file {
    const uint8_t *data;
    size_t size;
    uint16_t type;
    uint16_t machine;
    uint64_t entry;
    uint64_t section_header_offset;
    uint16_t section_header_entry_size;
    uint16_t section_count;
    uint16_t section_name_index;
} kb_elf_file_t;

typedef struct kb_elf_section {
    const char *name;
    uint32_t type;
    uint64_t flags;
    uint64_t address;
    uint64_t offset;
    uint64_t size;
    uint32_t link;
    uint32_t info;
    uint64_t alignment;
    uint64_t entry_size;
} kb_elf_section_t;

typedef struct kb_elf_symbol {
    const char *name;
    uint8_t binding;
    uint8_t type;
    uint8_t visibility;
    uint16_t section_index;
    uint64_t value;
    uint64_t size;
} kb_elf_symbol_t;

typedef struct kb_elf_relocation {
    uint64_t offset;
    uint32_t type;
    uint32_t symbol_index;
    int64_t addend;
    int has_addend;
    uint32_t target_section_index;
    uint32_t symbol_table_section_index;
} kb_elf_relocation_t;

typedef struct kb_elf_modinfo_entry {
    const char *key;
    size_t key_size;
    const char *value;
    size_t value_size;
} kb_elf_modinfo_entry_t;

kb_status_t kb_elf_open(const void *data, size_t size, kb_elf_file_t *out_file);
size_t kb_elf_section_count(const kb_elf_file_t *file);
kb_status_t kb_elf_section(const kb_elf_file_t *file, size_t index, kb_elf_section_t *out_section);
kb_status_t kb_elf_symbol_count(const kb_elf_file_t *file, size_t section_index, size_t *out_count);
kb_status_t kb_elf_symbol(
    const kb_elf_file_t *file,
    size_t section_index,
    size_t symbol_index,
    kb_elf_symbol_t *out_symbol);
kb_status_t kb_elf_relocation_count(const kb_elf_file_t *file, size_t section_index, size_t *out_count);
kb_status_t kb_elf_relocation(
    const kb_elf_file_t *file,
    size_t section_index,
    size_t relocation_index,
    kb_elf_relocation_t *out_relocation);
kb_status_t kb_elf_modinfo_section(const kb_elf_file_t *file, size_t *out_section_index);
kb_status_t kb_elf_modinfo_entry_count(const kb_elf_file_t *file, size_t section_index, size_t *out_count);
kb_status_t kb_elf_modinfo_entry(
    const kb_elf_file_t *file,
    size_t section_index,
    size_t entry_index,
    kb_elf_modinfo_entry_t *out_entry);

const char *kb_elf_type_name(uint16_t type);
const char *kb_elf_machine_name(uint16_t machine);
const char *kb_elf_section_type_name(uint32_t type);
const char *kb_elf_symbol_binding_name(uint8_t binding);
const char *kb_elf_symbol_type_name(uint8_t type);
const char *kb_elf_x86_64_relocation_type_name(uint32_t type);

#ifdef __cplusplus
}
#endif
