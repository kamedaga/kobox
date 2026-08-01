#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

enum {
    KB_INPUT_NAME_MAX = 128,
    KB_INPUT_PHYS_MAX = 128,
    KB_INPUT_UNIQ_MAX = 128,
    KB_INPUT_ABS_MAX = 64,
    KB_INPUT_KEY_WORDS = 12,
    KB_INPUT_FF_WORDS = 2,
    KB_INPUT_EVENT_QUEUE_MAX = 1024,
    KB_INPUT_LINUX_DEVICE_STORAGE_SIZE = 8192,
};

typedef struct kb_input_id {
    uint16_t bustype;
    uint16_t vendor;
    uint16_t product;
    uint16_t version;
} kb_input_id_t;

typedef struct kb_input_abs_params {
    int active;
    int value;
    int minimum;
    int maximum;
    int fuzz;
    int flat;
    int resolution;
} kb_input_abs_params_t;

typedef struct kb_input_device_snapshot {
    int active;
    unsigned int id;
    void *linux_dev;
    char name[KB_INPUT_NAME_MAX];
    char phys[KB_INPUT_PHYS_MAX];
    char uniq[KB_INPUT_UNIQ_MAX];
    kb_input_id_t input_id;
    uint64_t prop_bits;
    uint64_t event_bits;
    uint64_t key_bits[KB_INPUT_KEY_WORDS];
    uint64_t key_state[KB_INPUT_KEY_WORDS];
    uint64_t rel_bits;
    uint64_t abs_bits;
    uint64_t msc_bits;
    uint64_t led_bits;
    uint64_t snd_bits;
    uint64_t ff_bits[KB_INPUT_FF_WORDS];
    uint64_t sw_bits;
    uint64_t led_state;
    uint64_t snd_state;
    uint64_t sw_state;
    unsigned long event_count;
    unsigned long dropped_events;
    int opened;
    int open_result;
    unsigned int mt_slots;
    unsigned int mt_flags;
    kb_input_abs_params_t abs[KB_INPUT_ABS_MAX];
} kb_input_device_snapshot_t;

typedef struct kb_input_event {
    uint64_t sequence;
    unsigned int device_id;
    void *linux_dev;
    unsigned int type;
    unsigned int code;
    int value;
} kb_input_event_t;

void *kb_input_subsystem_allocate_device(void);
void kb_input_subsystem_free_device(void *dev);
int kb_input_subsystem_register_device(void *dev);
void kb_input_subsystem_unregister_device(void *dev);
void kb_input_subsystem_record_event(void *dev, unsigned int type, unsigned int code, int value);
void kb_input_subsystem_set_abs_params(
    void *dev,
    unsigned int axis,
    int minimum,
    int maximum,
    int fuzz,
    int flat);
void kb_input_subsystem_alloc_absinfo(void *dev);
int kb_input_subsystem_mt_init_slots(void *dev, unsigned int num_slots, unsigned int flags);
size_t kb_input_subsystem_device_count(void);
int kb_input_subsystem_for_each_device(
    int (*callback)(const kb_input_device_snapshot_t *device, void *ctx),
    void *ctx);
int kb_input_subsystem_open_registered_devices(void);
void kb_input_subsystem_close_registered_devices(void);
size_t kb_input_subsystem_pop_events(kb_input_event_t *events, size_t max_events);
size_t kb_input_subsystem_event_count(void);
void kb_input_subsystem_print_summary(FILE *out);
int kb_input_subsystem_run_mouse_smoke(FILE *out);
void kb_input_subsystem_reset(void);
