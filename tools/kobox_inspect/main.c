#include "kobox/elf.h"
#include "kobox/module.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct string_list {
    const char **items;
    size_t count;
    size_t capacity;
} string_list_t;

static const char *status_name(kb_status_t status)
{
    switch (status) {
    case KB_OK:
        return "ok";
    case KB_ERR_INVALID:
        return "invalid ELF";
    case KB_ERR_NOT_FOUND:
        return "not found";
    case KB_ERR_DENIED:
        return "denied";
    case KB_ERR_NOMEM:
        return "out of memory";
    case KB_ERR_IO:
        return "I/O error";
    case KB_ERR_UNSUPPORTED:
        return "unsupported ELF";
    default:
        return "unknown error";
    }
}

static kb_status_t read_file(const char *path, unsigned char **out_data, size_t *out_size)
{
    if (path == 0 || out_data == 0 || out_size == 0) {
        return KB_ERR_INVALID;
    }

    FILE *file = fopen(path, "rb");
    if (file == 0) {
        return KB_ERR_IO;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return KB_ERR_IO;
    }
    long size = ftell(file);
    if (size <= 0) {
        fclose(file);
        return KB_ERR_INVALID;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return KB_ERR_IO;
    }

    unsigned char *data = malloc((size_t)size);
    if (data == 0) {
        fclose(file);
        return KB_ERR_NOMEM;
    }

    const size_t read_count = fread(data, 1, (size_t)size, file);
    fclose(file);
    if (read_count != (size_t)size) {
        free(data);
        return KB_ERR_IO;
    }

    *out_data = data;
    *out_size = (size_t)size;
    return KB_OK;
}

static int compare_cstr_ptrs(const void *lhs, const void *rhs)
{
    const char *const *left = (const char *const *)lhs;
    const char *const *right = (const char *const *)rhs;
    return strcmp(*left, *right);
}

static void string_list_destroy(string_list_t *list)
{
    free(list->items);
    list->items = 0;
    list->count = 0;
    list->capacity = 0;
}

static int string_list_contains(const string_list_t *list, const char *value)
{
    for (size_t i = 0; i < list->count; i++) {
        if (strcmp(list->items[i], value) == 0) {
            return 1;
        }
    }
    return 0;
}

static kb_status_t string_list_add_unique(string_list_t *list, const char *value)
{
    if (value == 0 || value[0] == '\0' || string_list_contains(list, value)) {
        return KB_OK;
    }

    if (list->count == list->capacity) {
        const size_t next_capacity = list->capacity == 0 ? 16 : list->capacity * 2;
        const char **next_items = realloc(list->items, next_capacity * sizeof(*next_items));
        if (next_items == 0) {
            return KB_ERR_NOMEM;
        }
        list->items = next_items;
        list->capacity = next_capacity;
    }

    list->items[list->count] = value;
    list->count++;
    return KB_OK;
}

static void print_elf_header(const kb_elf_file_t *file)
{
    printf("loader: %s\n", kb_module_loader_version());
    printf("format: ELF64 little-endian\n");
    printf("type: %s (%u)\n", kb_elf_type_name(file->type), (unsigned)file->type);
    printf("machine: %s (%u)\n", kb_elf_machine_name(file->machine), (unsigned)file->machine);
    printf("entry: 0x%016" PRIx64 "\n", file->entry);
    printf("section_headers: offset=0x%016" PRIx64 " entry_size=%u count=%u shstrndx=%u\n",
        file->section_header_offset,
        (unsigned)file->section_header_entry_size,
        (unsigned)file->section_count,
        (unsigned)file->section_name_index);
}

static int print_sections(const kb_elf_file_t *file)
{
    printf("\nSections:\n");
    printf("  [Nr] %-20s %-10s %-10s %-10s %-10s %-10s\n",
        "Name",
        "Type",
        "Offset",
        "Size",
        "EntSize",
        "Align");

    const size_t count = kb_elf_section_count(file);
    for (size_t i = 0; i < count; i++) {
        kb_elf_section_t section;
        const kb_status_t status = kb_elf_section(file, i, &section);
        if (status != KB_OK) {
            fprintf(stderr, "failed to read section %zu: %s\n", i, status_name(status));
            return 1;
        }
        printf("  [%2zu] %-20s %-10s %010" PRIx64 " %010" PRIx64 " %010" PRIx64 " %010" PRIx64 "\n",
            i,
            section.name,
            kb_elf_section_type_name(section.type),
            section.offset,
            section.size,
            section.entry_size,
            section.alignment);
    }
    return 0;
}

static int print_symbols_for_section(const kb_elf_file_t *file, size_t section_index, const kb_elf_section_t *section)
{
    size_t symbol_count = 0;
    kb_status_t status = kb_elf_symbol_count(file, section_index, &symbol_count);
    if (status != KB_OK) {
        fprintf(stderr, "failed to read symbol count for %s: %s\n", section->name, status_name(status));
        return 1;
    }

    printf("\nSymbols from %s:\n", section->name);
    printf("  [Nr] %-28s %-8s %-8s %-8s %-10s %-10s\n",
        "Name",
        "Bind",
        "Type",
        "Shndx",
        "Value",
        "Size");

    for (size_t i = 0; i < symbol_count; i++) {
        kb_elf_symbol_t symbol;
        status = kb_elf_symbol(file, section_index, i, &symbol);
        if (status != KB_OK) {
            fprintf(stderr, "failed to read symbol %zu from %s: %s\n", i, section->name, status_name(status));
            return 1;
        }

        const char *section_label = "ABS";
        char section_index_text[16];
        if (symbol.section_index == KB_ELF_SHN_UNDEF) {
            section_label = "UND";
        } else {
            snprintf(section_index_text, sizeof(section_index_text), "%u", (unsigned)symbol.section_index);
            section_label = section_index_text;
        }

        printf("  [%3zu] %-28s %-8s %-8s %-8s %010" PRIx64 " %010" PRIx64 "\n",
            i,
            symbol.name,
            kb_elf_symbol_binding_name(symbol.binding),
            kb_elf_symbol_type_name(symbol.type),
            section_label,
            symbol.value,
            symbol.size);
    }

    return 0;
}

static int print_symbols(const kb_elf_file_t *file)
{
    const size_t section_count = kb_elf_section_count(file);
    for (size_t i = 0; i < section_count; i++) {
        kb_elf_section_t section;
        kb_status_t status = kb_elf_section(file, i, &section);
        if (status != KB_OK) {
            fprintf(stderr, "failed to read section %zu: %s\n", i, status_name(status));
            return 1;
        }
        if (section.type != KB_ELF_SHT_SYMTAB && section.type != KB_ELF_SHT_DYNSYM) {
            continue;
        }
        if (print_symbols_for_section(file, i, &section) != 0) {
            return 1;
        }
    }

    return 0;
}

static int print_undefined_symbols(const kb_elf_file_t *file)
{
    int printed_header = 0;
    const size_t section_count = kb_elf_section_count(file);
    for (size_t section_index = 0; section_index < section_count; section_index++) {
        kb_elf_section_t section;
        kb_status_t status = kb_elf_section(file, section_index, &section);
        if (status != KB_OK) {
            fprintf(stderr, "failed to read section %zu: %s\n", section_index, status_name(status));
            return 1;
        }
        if (section.type != KB_ELF_SHT_SYMTAB && section.type != KB_ELF_SHT_DYNSYM) {
            continue;
        }

        size_t symbol_count = 0;
        status = kb_elf_symbol_count(file, section_index, &symbol_count);
        if (status != KB_OK) {
            fprintf(stderr, "failed to read symbol count for %s: %s\n", section.name, status_name(status));
            return 1;
        }

        for (size_t symbol_index = 0; symbol_index < symbol_count; symbol_index++) {
            kb_elf_symbol_t symbol;
            status = kb_elf_symbol(file, section_index, symbol_index, &symbol);
            if (status != KB_OK) {
                fprintf(stderr, "failed to read symbol %zu from %s: %s\n", symbol_index, section.name, status_name(status));
                return 1;
            }
            if (symbol.section_index != KB_ELF_SHN_UNDEF || symbol.name[0] == '\0') {
                continue;
            }
            if (!printed_header) {
                printf("\nUndefined symbols:\n");
                printed_header = 1;
            }
            printf("  %s\n", symbol.name);
        }
    }

    if (!printed_header) {
        printf("\nUndefined symbols: none\n");
    }
    return 0;
}

static int collect_required_shim_symbols(const kb_elf_file_t *file, string_list_t *out_symbols)
{
    const size_t section_count = kb_elf_section_count(file);
    for (size_t section_index = 0; section_index < section_count; section_index++) {
        kb_elf_section_t section;
        kb_status_t status = kb_elf_section(file, section_index, &section);
        if (status != KB_OK) {
            fprintf(stderr, "failed to read section %zu: %s\n", section_index, status_name(status));
            return 1;
        }
        if (section.type != KB_ELF_SHT_SYMTAB && section.type != KB_ELF_SHT_DYNSYM) {
            continue;
        }

        size_t symbol_count = 0;
        status = kb_elf_symbol_count(file, section_index, &symbol_count);
        if (status != KB_OK) {
            fprintf(stderr, "failed to read symbol count for %s: %s\n", section.name, status_name(status));
            return 1;
        }

        for (size_t symbol_index = 0; symbol_index < symbol_count; symbol_index++) {
            kb_elf_symbol_t symbol;
            status = kb_elf_symbol(file, section_index, symbol_index, &symbol);
            if (status != KB_OK) {
                fprintf(stderr, "failed to read symbol %zu from %s: %s\n", symbol_index, section.name, status_name(status));
                return 1;
            }
            if (symbol.section_index != KB_ELF_SHN_UNDEF) {
                continue;
            }
            status = string_list_add_unique(out_symbols, symbol.name);
            if (status != KB_OK) {
                fprintf(stderr, "failed to collect required shim symbol: %s\n", status_name(status));
                return 1;
            }
        }
    }
    return 0;
}

static int print_required_shim_symbols(const kb_elf_file_t *file)
{
    string_list_t symbols = {0};
    const int collect_result = collect_required_shim_symbols(file, &symbols);
    if (collect_result != 0) {
        string_list_destroy(&symbols);
        return collect_result;
    }

    if (symbols.count == 0) {
        printf("\nRequired shim symbols: none\n");
        string_list_destroy(&symbols);
        return 0;
    }

    qsort(symbols.items, symbols.count, sizeof(symbols.items[0]), compare_cstr_ptrs);

    printf("\nRequired shim symbols:\n");
    for (size_t i = 0; i < symbols.count; i++) {
        printf("  %s\n", symbols.items[i]);
    }

    string_list_destroy(&symbols);
    return 0;
}

static const char *section_name_or_unknown(const kb_elf_file_t *file, uint32_t section_index, kb_elf_section_t *out_section)
{
    if (section_index >= kb_elf_section_count(file)) {
        return "?";
    }
    if (kb_elf_section(file, section_index, out_section) != KB_OK) {
        return "?";
    }
    return out_section->name;
}

static int is_debug_section_name(const char *name)
{
    return strncmp(name, ".debug", 6) == 0 ||
        strncmp(name, ".rela.debug", 11) == 0 ||
        strncmp(name, ".rel.debug", 10) == 0;
}

static const char *relocation_symbol_name(const kb_elf_file_t *file, const kb_elf_relocation_t *relocation)
{
    static const char empty[] = "";
    kb_elf_symbol_t symbol;
    if (kb_elf_symbol(
            file,
            relocation->symbol_table_section_index,
            relocation->symbol_index,
            &symbol) != KB_OK)
    {
        return "?";
    }
    if (symbol.name[0] == '\0') {
        return empty;
    }
    return symbol.name;
}

static int print_relocations_for_section(const kb_elf_file_t *file, size_t section_index, const kb_elf_section_t *section)
{
    size_t relocation_count = 0;
    kb_status_t status = kb_elf_relocation_count(file, section_index, &relocation_count);
    if (status != KB_OK) {
        fprintf(stderr, "failed to read relocation count for %s: %s\n", section->name, status_name(status));
        return 1;
    }

    kb_elf_section_t target_section;
    const char *target_name = section_name_or_unknown(file, section->info, &target_section);
    printf("\nRelocations from %s targeting %s:\n", section->name, target_name);
    printf("  [Nr] %-10s %-24s %-8s %-28s %-10s\n",
        "Offset",
        "Type",
        "SymIdx",
        "Symbol",
        "Addend");

    for (size_t i = 0; i < relocation_count; i++) {
        kb_elf_relocation_t relocation;
        status = kb_elf_relocation(file, section_index, i, &relocation);
        if (status != KB_OK) {
            fprintf(stderr, "failed to read relocation %zu from %s: %s\n", i, section->name, status_name(status));
            return 1;
        }

        char addend_text[32];
        if (relocation.has_addend) {
            snprintf(addend_text, sizeof(addend_text), "%" PRId64, relocation.addend);
        } else {
            snprintf(addend_text, sizeof(addend_text), "%s", "-");
        }

        printf("  [%3zu] %010" PRIx64 " %-24s %-8u %-28s %-10s\n",
            i,
            relocation.offset,
            kb_elf_x86_64_relocation_type_name(relocation.type),
            relocation.symbol_index,
            relocation_symbol_name(file, &relocation),
            addend_text);
    }

    return 0;
}

static int print_relocations(const kb_elf_file_t *file)
{
    int found = 0;
    size_t skipped_debug = 0;
    const size_t section_count = kb_elf_section_count(file);
    for (size_t i = 0; i < section_count; i++) {
        kb_elf_section_t section;
        kb_status_t status = kb_elf_section(file, i, &section);
        if (status != KB_OK) {
            fprintf(stderr, "failed to read section %zu: %s\n", i, status_name(status));
            return 1;
        }
        if (section.type != KB_ELF_SHT_RELA && section.type != KB_ELF_SHT_REL) {
            continue;
        }

        kb_elf_section_t target_section;
        const char *target_name = section_name_or_unknown(file, section.info, &target_section);
        if (is_debug_section_name(section.name) || is_debug_section_name(target_name)) {
            skipped_debug++;
            continue;
        }

        found = 1;
        if (print_relocations_for_section(file, i, &section) != 0) {
            return 1;
        }
    }

    if (!found) {
        printf("\nRelocations: none\n");
    }
    if (skipped_debug != 0) {
        printf("\nRelocations: skipped %zu debug relocation section(s)\n", skipped_debug);
    }
    return 0;
}

static int print_modinfo(const kb_elf_file_t *file)
{
    size_t section_index = 0;
    kb_status_t status = kb_elf_modinfo_section(file, &section_index);
    if (status == KB_ERR_NOT_FOUND) {
        printf("\nModinfo: none\n");
        return 0;
    }
    if (status != KB_OK) {
        fprintf(stderr, "failed to locate modinfo: %s\n", status_name(status));
        return 1;
    }

    size_t entry_count = 0;
    status = kb_elf_modinfo_entry_count(file, section_index, &entry_count);
    if (status != KB_OK) {
        fprintf(stderr, "failed to read modinfo count: %s\n", status_name(status));
        return 1;
    }

    printf("\nModinfo:\n");
    for (size_t i = 0; i < entry_count; i++) {
        kb_elf_modinfo_entry_t entry;
        status = kb_elf_modinfo_entry(file, section_index, i, &entry);
        if (status != KB_OK) {
            fprintf(stderr, "failed to read modinfo entry %zu: %s\n", i, status_name(status));
            return 1;
        }
        printf("  %.*s=%.*s\n",
            (int)entry.key_size,
            entry.key,
            (int)entry.value_size,
            entry.value);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: kobox-inspect <module.ko>\n");
        return 2;
    }

    unsigned char *data = 0;
    size_t size = 0;
    kb_status_t status = read_file(argv[1], &data, &size);
    if (status != KB_OK) {
        fprintf(stderr, "kobox-inspect: failed to read %s: %s\n", argv[1], status_name(status));
        return 1;
    }

    kb_elf_file_t elf;
    status = kb_elf_open(data, size, &elf);
    if (status != KB_OK) {
        fprintf(stderr, "kobox-inspect: failed to parse %s: %s\n", argv[1], status_name(status));
        free(data);
        return 1;
    }

    printf("kobox-inspect: %s\n", argv[1]);
    print_elf_header(&elf);
    int result = print_sections(&elf);
    if (result == 0) {
        result = print_symbols(&elf);
    }
    if (result == 0) {
        result = print_undefined_symbols(&elf);
    }
    if (result == 0) {
        result = print_required_shim_symbols(&elf);
    }
    if (result == 0) {
        result = print_relocations(&elf);
    }
    if (result == 0) {
        result = print_modinfo(&elf);
    }
    free(data);
    return result;
}
