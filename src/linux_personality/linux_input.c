#include "kobox/shim.h"
#include "linux_subsystem/input/input.h"
#include "linux_subsystem/kvm/kvm_symbols.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    KB_INPUT_SG_SIZE = 32,
    KB_INPUT_STRUCT_PAGE_SIZE = 64,
    KB_INPUT_PAGE_RECORD_MAX = 4096,
    KB_INPUT_PAGE_SIZE = 4096,
    KB_INPUT_PAGE_SHIFT = 12,
    KB_INPUT_PAGE_MASK = 0xfff,
    KB_INPUT_SG_PAGE_LINK_OFFSET = 0x0,
    KB_INPUT_SG_OFFSET_OFFSET = 0x8,
    KB_INPUT_SG_LENGTH_OFFSET = 0xc,
    KB_INPUT_SG_CHAIN = 0x1,
    KB_INPUT_SG_END = 0x2,
};

static const uintptr_t KB_INPUT_ENCODED_PAGE_TAG = (uintptr_t)1ull << 63;

static void write_ptr_at(void *base, size_t offset, const void *ptr)
{
    memcpy((unsigned char *)base + offset, &ptr, sizeof(ptr));
}

static void write_u32_at(void *base, size_t offset, uint32_t value)
{
    memcpy((unsigned char *)base + offset, &value, sizeof(value));
}

static int page_for_buffer(const void *buf, unsigned int buflen, void **out_page, uint32_t *out_offset)
{
    uintptr_t page_offset = kb_linux_kvm_page_offset_base();
    uintptr_t addr = (uintptr_t)buf;
    uintptr_t payload_size = (uintptr_t)KB_INPUT_PAGE_RECORD_MAX * KB_INPUT_PAGE_SIZE;
    if (page_offset == 0 || addr < page_offset || addr >= page_offset + payload_size) {
        return 0;
    }
    uintptr_t offset = addr - page_offset;
    if ((offset & KB_INPUT_PAGE_MASK) + buflen > KB_INPUT_PAGE_SIZE) {
        return 0;
    }
    *out_page = (void *)(kb_linux_kvm_vmemmap_base() + ((offset >> KB_INPUT_PAGE_SHIFT) * KB_INPUT_STRUCT_PAGE_SIZE));
    *out_offset = (uint32_t)(offset & KB_INPUT_PAGE_MASK);
    return 1;
}

static int encoded_page_for_buffer(const void *buf, unsigned int buflen, void **out_page, uint32_t *out_offset)
{
    if (buf == NULL || buflen == 0 || out_page == NULL || out_offset == NULL) {
        return 0;
    }
    uintptr_t addr = (uintptr_t)buf;
    uint32_t offset = (uint32_t)(addr & KB_INPUT_PAGE_MASK);
    uintptr_t page_base = addr & ~(uintptr_t)KB_INPUT_PAGE_MASK;
    *out_page = (void *)(KB_INPUT_ENCODED_PAGE_TAG |
                         (((uint64_t)page_base >> KB_INPUT_PAGE_SHIFT) * KB_INPUT_STRUCT_PAGE_SIZE));
    *out_offset = offset;
    return 1;
}

void *kb_kmalloc_alias(size_t size, unsigned int flags)
{
    return kb_kmalloc(size, flags);
}

void kb_might_resched(void)
{
}

void kb_ubsan_handle_load_invalid_value(void *data, void *ptr)
{
    (void)data;
    (void)ptr;
}

void kb_ubsan_handle_out_of_bounds(void *data, void *index)
{
    (void)data;
    (void)index;
}

void kb_sg_init_one(void *sg, const void *buf, unsigned int buflen)
{
    if (sg == NULL) {
        return;
    }
    memset(sg, 0, KB_INPUT_SG_SIZE);
    if (buf == NULL || buflen == 0) {
        return;
    }

    void *page = NULL;
    uint32_t offset = 0;
    if (!page_for_buffer(buf, buflen, &page, &offset)) {
        if (!encoded_page_for_buffer(buf, buflen, &page, &offset)) {
            return;
        }
    }
    uintptr_t page_link = ((uintptr_t)page) | KB_INPUT_SG_END;
    write_ptr_at(sg, KB_INPUT_SG_PAGE_LINK_OFFSET, (void *)page_link);
    write_u32_at(sg, KB_INPUT_SG_OFFSET_OFFSET, offset);
    write_u32_at(sg, KB_INPUT_SG_LENGTH_OFFSET, buflen);
}

void kb_sg_init_table(void *sg, unsigned int nents)
{
    if (sg == NULL || nents == 0) {
        return;
    }
    memset(sg, 0, (size_t)nents * KB_INPUT_SG_SIZE);
    uintptr_t end = KB_INPUT_SG_END;
    write_ptr_at((unsigned char *)sg + ((size_t)(nents - 1u) * KB_INPUT_SG_SIZE), KB_INPUT_SG_PAGE_LINK_OFFSET, (void *)end);
}

void *kb_input_allocate_device(void)
{
    return kb_input_subsystem_allocate_device();
}

void kb_input_free_device(void *dev)
{
    kb_input_subsystem_free_device(dev);
}

int kb_input_register_device(void *dev)
{
    return kb_input_subsystem_register_device(dev);
}

void kb_input_unregister_device(void *dev)
{
    kb_input_subsystem_unregister_device(dev);
}

void kb_input_event(void *dev, unsigned int type, unsigned int code, int value)
{
    kb_input_subsystem_record_event(dev, type, code, value);
}

void kb_input_set_abs_params(void *dev, unsigned int axis, int min, int max, int fuzz, int flat)
{
    kb_input_subsystem_set_abs_params(dev, axis, min, max, fuzz, flat);
}

void kb_input_alloc_absinfo(void *dev)
{
    kb_input_subsystem_alloc_absinfo(dev);
}

int kb_input_mt_init_slots(void *dev, unsigned int num_slots, unsigned int flags)
{
    return kb_input_subsystem_mt_init_slots(dev, num_slots, flags);
}

void *__kmalloc(size_t size, unsigned int flags)
{
    return kb_kmalloc_alias(size, flags);
}

void __SCT__might_resched(void)
{
    kb_might_resched();
}

void __ubsan_handle_load_invalid_value(void *data, void *ptr)
{
    kb_ubsan_handle_load_invalid_value(data, ptr);
}

void __ubsan_handle_out_of_bounds(void *data, void *index)
{
    kb_ubsan_handle_out_of_bounds(data, index);
}

void sg_init_one(void *sg, const void *buf, unsigned int buflen)
{
    kb_sg_init_one(sg, buf, buflen);
}

void sg_init_table(void *sg, unsigned int nents)
{
    kb_sg_init_table(sg, nents);
}

void *input_allocate_device(void)
{
    return kb_input_allocate_device();
}

void input_free_device(void *dev)
{
    kb_input_free_device(dev);
}

int input_register_device(void *dev)
{
    return kb_input_register_device(dev);
}

void input_unregister_device(void *dev)
{
    kb_input_unregister_device(dev);
}

void input_event(void *dev, unsigned int type, unsigned int code, int value)
{
    kb_input_event(dev, type, code, value);
}

void input_set_abs_params(void *dev, unsigned int axis, int min, int max, int fuzz, int flat)
{
    kb_input_set_abs_params(dev, axis, min, max, fuzz, flat);
}

void input_alloc_absinfo(void *dev)
{
    kb_input_alloc_absinfo(dev);
}

int input_mt_init_slots(void *dev, unsigned int num_slots, unsigned int flags)
{
    return kb_input_mt_init_slots(dev, num_slots, flags);
}

int snprintf(char *str, size_t size, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int result = vsnprintf(str, size, format, args);
    va_end(args);
    return result;
}
