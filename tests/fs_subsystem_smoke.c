#include "kobox/device.h"
#include "kobox/device_linux_mock.h"
#include "kobox/interface_linux.h"
#include "kobox/platform.h"
#include "kobox/shim.h"
#include "loader/module_context.h"
#include "linux_subsystem/fs/fs.h"
#include "linux_subsystem/kvm/kvm_symbols.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

typedef struct fs_ipc_smoke {
    int mount_seen;
    int read_seen;
    int write_seen;
    int readdir_seen;
} fs_ipc_smoke_t;

static int kthread_step_count;
static void *execution_context_parent_task;
static void *execution_context_child_task;
static unsigned long wait_bit_word;
static int wait_bit_stage;
static int sync_dirty_inode_count;
static int sync_write_inode_count;
static int sync_super_count;
static int sync_write_inode_wait;
static int evict_inode_count;
static int evict_inode_saw_freeing;
static unsigned long write_cache_indices[4];
static size_t write_cache_count;
static int rcu_callback_count;
static int mempool_allow_alloc;
static int mempool_free_count;
static int shrinker_count_calls;
static int shrinker_scan_calls;
static int acl_get_calls;
static int acl_set_calls;
static uint16_t acl_set_user_permission;
static uint16_t acl_set_mask_permission;
static uint16_t acl_set_other_permission;
static unsigned int readahead_smoke_calls;
static unsigned int readahead_smoke_error;
static unsigned long readahead_smoke_indices[16];

static void readahead_smoke_aop(void *opaque_control)
{
    enum {
        RACTL_MAPPING_OFFSET = 0x08,
        RACTL_INDEX_OFFSET = 0x18,
        RACTL_NR_PAGES_OFFSET = 0x20,
        RACTL_BATCH_COUNT_OFFSET = 0x24,
    };
    unsigned char *control = opaque_control;
    for (;;) {
        unsigned int nr_pages = 0;
        unsigned int batch_count = 0;
        unsigned long index = 0;
        memcpy(&nr_pages, control + RACTL_NR_PAGES_OFFSET, sizeof(nr_pages));
        memcpy(&batch_count,
            control + RACTL_BATCH_COUNT_OFFSET,
            sizeof(batch_count));
        memcpy(&index, control + RACTL_INDEX_OFFSET, sizeof(index));
        if (batch_count > nr_pages || index > ULONG_MAX - batch_count) {
            readahead_smoke_error = 1;
            return;
        }
        nr_pages -= batch_count;
        index += batch_count;
        memcpy(control + RACTL_NR_PAGES_OFFSET, &nr_pages, sizeof(nr_pages));
        memcpy(control + RACTL_INDEX_OFFSET, &index, sizeof(index));
        if (nr_pages == 0) {
            batch_count = 0;
            memcpy(control + RACTL_BATCH_COUNT_OFFSET,
                &batch_count,
                sizeof(batch_count));
            return;
        }
        void *mapping = NULL;
        memcpy(&mapping, control + RACTL_MAPPING_OFFSET, sizeof(mapping));
        void *folio = kb_fs_subsystem_xa_load(
            (unsigned char *)mapping + 8u,
            index);
        uint64_t flags = 0;
        if (folio == NULL || readahead_smoke_calls >= 16u) {
            readahead_smoke_error = 2;
            return;
        }
        memcpy(&flags, folio, sizeof(flags));
        if ((flags & 1u) == 0) {
            readahead_smoke_error = 3;
            return;
        }
        batch_count = 1;
        memcpy(control + RACTL_BATCH_COUNT_OFFSET,
            &batch_count,
            sizeof(batch_count));
        kb_fs_subsystem_folio_put(folio);
        unsigned char *payload = kb_linux_kvm_page_payload(folio, 0, 4096);
        if (payload == NULL) {
            readahead_smoke_error = 4;
            return;
        }
        memset(payload, (int)(index & 0xffu), 4096);
        readahead_smoke_indices[readahead_smoke_calls++] = index;
        kb_fs_subsystem_folio_end_read(folio, 1);
    }
}

static int mmap_smoke_read_folio(void *file, void *folio)
{
    (void)file;
    unsigned char *payload = kb_linux_kvm_page_payload(folio, 0, 4096);
    if (payload == NULL) {
        kb_fs_subsystem_folio_end_read(folio, 0);
        return -12;
    }
    memset(payload, 0x6d, 4096);
    kb_fs_subsystem_folio_end_read(folio, 1);
    return 0;
}

static int mmap_smoke_get_block(
    void *inode,
    uint64_t logical_block,
    void *buffer_head,
    int create)
{
    (void)inode;
    if (buffer_head == NULL || create != 1) {
        return -22;
    }
    uint64_t state = 0;
    memcpy(&state, buffer_head, sizeof(state));
    state |= (1ull << 4) | (1ull << 5);
    memcpy(buffer_head, &state, sizeof(state));
    memcpy((unsigned char *)buffer_head + 0x18,
        &logical_block,
        sizeof(logical_block));
    return 0;
}

static int fiemap_iomap_smoke_begin(
    void *inode,
    int64_t position,
    int64_t length,
    unsigned int flags,
    void *iomap,
    void *source_iomap)
{
    (void)inode;
    (void)length;
    if (iomap == NULL || source_iomap == NULL || flags != (1u << 2) ||
        position < 0 || position >= 8192)
    {
        return -22;
    }
    const uint64_t offset = (uint64_t)position;
    const uint64_t map_length = 4096;
    const uint64_t address = position < 4096 ? 0x10000u : UINT64_MAX;
    const uint16_t type = position < 4096 ? 2u : 0u;
    const uint16_t map_flags = position < 4096 ? (1u << 2) | (1u << 3) : 0;
    memcpy((uint8_t *)iomap + 0, &address, sizeof(address));
    memcpy((uint8_t *)iomap + 8, &offset, sizeof(offset));
    memcpy((uint8_t *)iomap + 16, &map_length, sizeof(map_length));
    memcpy((uint8_t *)iomap + 24, &type, sizeof(type));
    memcpy((uint8_t *)iomap + 26, &map_flags, sizeof(map_flags));
    return 0;
}

static int seek_iomap_smoke_begin(
    void *inode,
    int64_t position,
    int64_t length,
    unsigned int flags,
    void *iomap,
    void *source_iomap)
{
    (void)inode;
    (void)length;
    if (iomap == NULL || source_iomap == NULL || flags != (1u << 2) ||
        position < 0 || position >= 12288)
    {
        return -22;
    }
    const uint64_t offset = (uint64_t)position & ~(uint64_t)4095u;
    const uint64_t map_length = 4096;
    const uint64_t address = offset == 4096 ? 0x20000u : UINT64_MAX;
    const uint16_t type = offset == 4096 ? 2u : 0u;
    memcpy((uint8_t *)iomap + 0, &address, sizeof(address));
    memcpy((uint8_t *)iomap + 8, &offset, sizeof(offset));
    memcpy((uint8_t *)iomap + 16, &map_length, sizeof(map_length));
    memcpy((uint8_t *)iomap + 24, &type, sizeof(type));
    return 0;
}

static int unwritten_iomap_smoke_begin(
    void *inode,
    int64_t position,
    int64_t length,
    unsigned int flags,
    void *iomap,
    void *source_iomap)
{
    (void)inode;
    (void)length;
    if (iomap == NULL || source_iomap == NULL || flags != (1u << 2) ||
        position < 0 || position >= 12288)
    {
        return -22;
    }
    const uint64_t offset = 0;
    const uint64_t map_length = 12288;
    const uint64_t address = 0x30000u;
    const uint16_t type = 3u;
    memcpy((uint8_t *)iomap + 0, &address, sizeof(address));
    memcpy((uint8_t *)iomap + 8, &offset, sizeof(offset));
    memcpy((uint8_t *)iomap + 16, &map_length, sizeof(map_length));
    memcpy((uint8_t *)iomap + 24, &type, sizeof(type));
    return 0;
}

static void *acl_smoke_get(void *inode, int type, int rcu)
{
    (void)inode;
    if (type != 0x4000 || rcu != 0) {
        return (void *)(intptr_t)-22;
    }
    acl_get_calls++;
    uint8_t *acl = kb_fs_subsystem_posix_acl_alloc(5, 0);
    if (acl == NULL) {
        return NULL;
    }
    static const uint16_t tags[5] = {0x01, 0x02, 0x04, 0x10, 0x20};
    static const uint16_t permissions[5] = {7, 7, 7, 7, 7};
    for (size_t i = 0; i < 5; ++i) {
        memcpy(acl + 28u + i * 8u, &tags[i], sizeof(tags[i]));
        memcpy(acl + 30u + i * 8u, &permissions[i], sizeof(permissions[i]));
    }
    return acl;
}

static int acl_smoke_set(void *idmap, void *dentry, void *opaque_acl, int type)
{
    (void)idmap;
    (void)dentry;
    if (opaque_acl == NULL || type != 0x8000) {
        return -22;
    }
    const uint8_t *acl = opaque_acl;
    acl_set_calls++;
    memcpy(&acl_set_user_permission, acl + 30u, sizeof(acl_set_user_permission));
    memcpy(&acl_set_mask_permission, acl + 30u + 3u * 8u, sizeof(acl_set_mask_permission));
    memcpy(&acl_set_other_permission, acl + 30u + 4u * 8u, sizeof(acl_set_other_permission));
    return 0;
}

static unsigned long core_helpers_shrinker_count(void *shrinker, void *control)
{
    (void)shrinker;
    if (control != NULL) {
        shrinker_count_calls++;
    }
    return 8;
}

static unsigned long core_helpers_shrinker_scan(void *shrinker, void *control)
{
    (void)shrinker;
    unsigned long nr_to_scan = 0;
    if (control != NULL) {
        memcpy(&nr_to_scan, (const unsigned char *)control + 8u, sizeof(nr_to_scan));
        shrinker_scan_calls++;
    }
    return nr_to_scan;
}

static void core_helpers_rcu_callback(void *head)
{
    if (head != NULL) {
        rcu_callback_count++;
    }
}

static void *core_helpers_mempool_alloc(unsigned int gfp, void *pool_data)
{
    (void)gfp;
    if (!mempool_allow_alloc || pool_data == NULL) {
        return NULL;
    }
    return kb_kzalloc(*(const size_t *)pool_data, 0);
}

static void core_helpers_mempool_free(void *element, void *pool_data)
{
    (void)pool_data;
    mempool_free_count++;
    kb_kfree(element);
}

static void sync_smoke_dirty_inode(void *inode, int flags)
{
    if (inode != NULL && flags != 0) {
        sync_dirty_inode_count++;
    }
}

static int sync_smoke_write_inode(void *inode, void *writeback_control)
{
    uint32_t sync_mode = 0;
    if (inode == NULL || writeback_control == NULL) {
        return -22;
    }
    memcpy(&sync_mode, (const unsigned char *)writeback_control + 0x20, sizeof(sync_mode));
    sync_write_inode_count++;
    sync_write_inode_wait = sync_mode == 1;
    return 0;
}

static int sync_smoke_super(void *super_block, int wait)
{
    if (super_block == NULL || (wait != 0 && wait != 1)) {
        return -22;
    }
    sync_super_count++;
    return 0;
}

static void inode_reference_smoke_evict(void *inode)
{
    uint64_t state = 0;
    if (inode != NULL) {
        memcpy(&state, (const unsigned char *)inode + 0x90, sizeof(state));
    }
    evict_inode_count++;
    evict_inode_saw_freeing = (state & (1u << 7)) != 0;
}

static int write_cache_smoke_page(void *folio, void *writeback_control, void *data)
{
    (void)writeback_control;
    if (folio == NULL || data == NULL || write_cache_count >= 4) {
        return -22;
    }
    uint64_t flags = 0;
    unsigned long index = 0;
    memcpy(&flags, folio, sizeof(flags));
    memcpy(&index, (const unsigned char *)folio + 0x20, sizeof(index));
    if ((flags & 0x1u) == 0) {
        return -5;
    }
    write_cache_indices[write_cache_count++] = index;
    (void)kb_fs_subsystem_folio_clear_dirty_for_io(folio);
    kb_fs_subsystem_folio_unlock(folio);
    return 0;
}

static int cooperative_kthread_smoke(void *data)
{
    int *value = data;
    (*value)++;
    kb_run_deferred_work();
    (*value)++;
    return 0;
}

static int execution_context_kthread_smoke(void *data)
{
    (void)data;
    void *task = kb_kthread_current_task();
    execution_context_child_task = task;
    if (task == NULL || task == execution_context_parent_task ||
        kb_loader_current_task() != task ||
        kb_loader_task_journal_info(task) != NULL)
    {
        return -160;
    }

    void *child_marker = (void *)(uintptr_t)0x4b42544852454144ull;
    memcpy((unsigned char *)task + 0x930, &child_marker, sizeof(child_marker));
    kb_run_deferred_work();
    if (kb_loader_current_task() != task ||
        kb_loader_task_journal_info(task) != child_marker)
    {
        return -161;
    }
    child_marker = NULL;
    memcpy((unsigned char *)task + 0x930, &child_marker, sizeof(child_marker));
    return 0;
}

static int run_execution_context_kthread_smoke(void)
{
    void *parent = kb_loader_current_task();
    void *parent_marker = (void *)(uintptr_t)0x4b42504152454e54ull;
    execution_context_parent_task = parent;
    execution_context_child_task = NULL;
    memcpy((unsigned char *)parent + 0x930, &parent_marker, sizeof(parent_marker));

    void *task = kb_kthread_create_on_node(
        execution_context_kthread_smoke,
        NULL,
        -1,
        "kobox-context-smoke");
    if (task == NULL || kb_wake_up_process(task) != 1) {
        parent_marker = NULL;
        memcpy((unsigned char *)parent + 0x930, &parent_marker, sizeof(parent_marker));
        return 160;
    }
    kb_run_deferred_work();
    const int result = kb_kthread_stop(task);
    const int parent_restored = kb_loader_current_task() == parent &&
        kb_loader_task_journal_info(parent) == parent_marker;
    parent_marker = NULL;
    memcpy((unsigned char *)parent + 0x930, &parent_marker, sizeof(parent_marker));
    if (result != 0 || !parent_restored ||
        execution_context_child_task == NULL ||
        execution_context_child_task == parent)
    {
        return 161;
    }
    return 0;
}

static int cooperative_bit_clear_smoke(void *data)
{
    unsigned long *word = data;
    __atomic_fetch_and(word, ~(1ul << 3), __ATOMIC_RELEASE);
    kb_wake_up_bit(word, 3);
    return 0;
}

static int cooperative_bit_waiter_smoke(void *data)
{
    unsigned long *word = data;
    wait_bit_stage = 1;
    const int result = kb_out_of_line_wait_on_bit(
        word,
        3,
        kb_bit_wait_action,
        2);
    if (result != 0) {
        return result;
    }
    wait_bit_stage = 3;
    return 0;
}

static int cooperative_bit_waker_smoke(void *data)
{
    unsigned long *word = data;
    if (wait_bit_stage != 1) {
        return -22;
    }
    wait_bit_stage = 2;
    __atomic_fetch_and(word, ~(1ul << 3), __ATOMIC_RELEASE);
    kb_wake_up_bit(word, 3);
    return 0;
}

static int cooperative_folio_unlock_smoke(void *data)
{
    kb_fs_subsystem_folio_unlock(data);
    return 0;
}

typedef struct cooperative_waitqueue_smoke_state {
    _Alignas(void *) unsigned char head[sizeof(void *) * 3u];
    _Alignas(void *) unsigned char entry[sizeof(void *) * 5u];
    int stage;
    int wake_count;
} cooperative_waitqueue_smoke_state_t;

static int cooperative_waitqueue_waiter_smoke(void *data)
{
    cooperative_waitqueue_smoke_state_t *state = data;
    kb_init_wait_entry(state->entry, 0);
    kb_prepare_to_wait(state->head, state->entry, 2);
    state->stage = 1;
    kb_schedule();
    state->stage = 3;
    kb_finish_wait(state->head, state->entry);
    return 0;
}

static int cooperative_waitqueue_waker_smoke(void *data)
{
    cooperative_waitqueue_smoke_state_t *state = data;
    if (state->stage != 1) {
        return -22;
    }
    state->stage = 2;
    state->wake_count = kb_wake_up_waitqueue(state->head, 3u, 0, NULL);
    return 0;
}

static int run_cooperative_waitqueue_smoke(void)
{
    cooperative_waitqueue_smoke_state_t state = {0};
    kb_init_waitqueue_head(state.head);

    /* Records are inserted at the head: create the waker first so the
     * waiter runs, blocks, and is then made runnable by the waitqueue. */
    void *waker = kb_kthread_create_on_node(
        cooperative_waitqueue_waker_smoke, &state, -1, "kobox-wq-waker");
    void *waiter = kb_kthread_create_on_node(
        cooperative_waitqueue_waiter_smoke, &state, -1, "kobox-wq-waiter");
    if (waker == NULL || waiter == NULL ||
        kb_wake_up_process(waker) != 1 || kb_wake_up_process(waiter) != 1)
    {
        return 133;
    }
    kb_run_deferred_work();
    if (state.stage != 3 || state.wake_count != 1 ||
        kb_kthread_stop(waiter) != 0 || kb_kthread_stop(waker) != 0)
    {
        return 134;
    }
    return 0;
}

static int run_cooperative_kthread_smoke(void)
{
    kthread_step_count = 0;
    void *task = kb_kthread_create_on_node(
        cooperative_kthread_smoke, &kthread_step_count, -1, "kobox-smoke");
    if (task == NULL || kb_wake_up_process(task) != 1) {
        return 77;
    }
    kb_run_deferred_work();
    if (kthread_step_count != 2 || kb_kthread_stop(task) != 0) {
        return 78;
    }
    return 0;
}

static int run_cooperative_bit_wait_smoke(void)
{
    wait_bit_word = 1ul << 3;
    void *task = kb_kthread_create_on_node(
        cooperative_bit_clear_smoke,
        &wait_bit_word,
        -1,
        "kobox-bit-smoke");
    if (task == NULL || kb_wake_up_process(task) != 1 ||
        kb_out_of_line_wait_on_bit(&wait_bit_word, 3, kb_bit_wait_action, 0) != 0 ||
        wait_bit_word != 0)
    {
        return 111;
    }
    if (kb_kthread_stop(task) != 0 ||
        kb_out_of_line_wait_on_bit_lock(
            &wait_bit_word,
            3,
            kb_bit_wait_action,
            0) != 0 ||
        wait_bit_word != (1ul << 3))
    {
        return 112;
    }

    wait_bit_word = 1ul << 3;
    wait_bit_stage = 0;
    void *waker = kb_kthread_create_on_node(
        cooperative_bit_waker_smoke,
        &wait_bit_word,
        -1,
        "kobox-bit-waker");
    void *waiter = kb_kthread_create_on_node(
        cooperative_bit_waiter_smoke,
        &wait_bit_word,
        -1,
        "kobox-bit-waiter");
    if (waker == NULL || waiter == NULL ||
        kb_wake_up_process(waker) != 1 || kb_wake_up_process(waiter) != 1)
    {
        return 152;
    }
    kb_run_deferred_work();
    if (wait_bit_stage != 3 || wait_bit_word != 0 ||
        kb_kthread_stop(waiter) != 0 || kb_kthread_stop(waker) != 0)
    {
        return 153;
    }

    wait_bit_word = 0;
    wait_bit_word = 1ul;
    task = kb_kthread_create_on_node(
        cooperative_folio_unlock_smoke,
        &wait_bit_word,
        -1,
        "kobox-folio-lock-smoke");
    if (task == NULL || kb_wake_up_process(task) != 1) {
        return 130;
    }
    kb_fs_subsystem_folio_lock(&wait_bit_word);
    if (wait_bit_word != 1ul || kb_kthread_stop(task) != 0) {
        return 131;
    }
    kb_fs_subsystem_folio_unlock(&wait_bit_word);
    if (wait_bit_word != 0) {
        return 132;
    }
    return 0;
}

static int run_linux_core_helpers_smoke(void)
{
    uint32_t raw_lock = 0;
    if (kb_raw_spin_trylock(&raw_lock) != 1 ||
        kb_raw_spin_trylock(&raw_lock) != 0)
    {
        return 133;
    }
    kb_raw_spin_unlock_irqrestore(&raw_lock, 0);
    if (kb_raw_spin_trylock(&raw_lock) != 1) {
        return 134;
    }
    kb_raw_spin_unlock(&raw_lock);

    uintptr_t mutex = 0;
    kb_mutex_init(&mutex);
    if (kb_mutex_trylock(&mutex) != 1 ||
        kb_mutex_trylock(&mutex) != 0 ||
        !kb_mutex_is_locked(&mutex))
    {
        return 135;
    }
    kb_mutex_unlock(&mutex);
    if (kb_mutex_is_locked(&mutex)) {
        return 136;
    }

    unsigned char rwsem[64] = {0};
    kb_init_rwsem(rwsem);
    if (kb_down_write_trylock(rwsem) != 1 ||
        kb_down_read_trylock(rwsem) != 0)
    {
        return 153;
    }
    kb_downgrade_write(rwsem);
    if (kb_down_write_trylock(rwsem) != 0 ||
        kb_down_read_trylock(rwsem) != 1)
    {
        return 154;
    }
    kb_up_read(rwsem);
    kb_up_read(rwsem);
    if (kb_down_write_trylock(rwsem) != 1) {
        return 155;
    }
    kb_up_write(rwsem);

    static const unsigned char vector[] = "123456789";
    uint32_t crc32c = 0;
    if (kb_crypto_shash_tfm_digest_stub(
            NULL,
            vector,
            (unsigned int)(sizeof(vector) - 1u),
            &crc32c) != 0 ||
        crc32c != UINT32_C(0x1cf96d7c) ||
        kb_crc32_le(UINT32_MAX, vector, sizeof(vector) - 1u) != UINT32_C(0x340bc6d9) ||
        kb_crc32_be(UINT32_MAX, vector, sizeof(vector) - 1u) != UINT32_C(0x0376e6e7))
    {
        return 137;
    }
    unsigned char uniform[8];
    memset(uniform, 0xa5, sizeof(uniform));
    if (kb_memchr_inv(uniform, 0xa5, sizeof(uniform)) != NULL) {
        return 138;
    }
    uniform[5] = 0x5a;
    if (kb_memchr_inv(uniform, 0xa5, sizeof(uniform)) != &uniform[5]) {
        return 139;
    }

    unsigned char xarray[16] = {0};
    unsigned long first_index = 3;
    unsigned long second_index = 7;
    int first_value = 31;
    int second_value = 71;
    if (kb_fs_subsystem_xa_insert(xarray, second_index, &second_value, 0) != 0 ||
        kb_fs_subsystem_xa_insert(xarray, first_index, &first_value, 0) != 0 ||
        kb_fs_subsystem_xa_insert(xarray, first_index, &first_value, 0) != -16 ||
        kb_fs_subsystem_xa_load(xarray, second_index) != &second_value)
    {
        return 140;
    }
    unsigned long cursor = 0;
    if (kb_fs_subsystem_xa_find(xarray, &cursor, ULONG_MAX, 8) != &first_value ||
        cursor != first_index ||
        kb_fs_subsystem_xa_find_after(xarray, &cursor, ULONG_MAX, 8) != &second_value ||
        cursor != second_index ||
        kb_fs_subsystem_xa_erase(xarray, first_index) != &first_value ||
        kb_fs_subsystem_xa_load(xarray, first_index) != NULL)
    {
        return 141;
    }
    kb_fs_subsystem_xa_destroy(xarray);
    void *head = (void *)(uintptr_t)1;
    memcpy(&head, xarray + 8u, sizeof(head));
    if (head != NULL) {
        return 142;
    }

    rcu_callback_count = 0;
    int rcu_head = 1;
    kb_rcu_read_lock();
    kb_call_rcu(&rcu_head, core_helpers_rcu_callback);
    if (rcu_callback_count != 0) {
        return 143;
    }
    kb_rcu_read_unlock();
    kb_rcu_barrier();
    if (rcu_callback_count != 1) {
        return 144;
    }

    int waited_var = 0;
    int unrelated_var = 0;
    unsigned char var_wait_entry[80] = {0};
    void *var_queue = kb_var_waitqueue(&waited_var);
    if (var_queue == NULL || var_queue != kb_var_waitqueue(&waited_var)) {
        return 148;
    }
    void *bit_queue = kb_bit_waitqueue(&waited_var, 3);
    if (bit_queue == NULL || bit_queue != kb_bit_waitqueue(&waited_var, 3)) {
        return 151;
    }
    kb_init_wait_var_entry(var_wait_entry, &waited_var, 0);
    void *embedded_entry = var_wait_entry + 3u * sizeof(void *);
    kb_prepare_to_wait_event(var_queue, embedded_entry, 2);
    struct {
        void *flags;
        int bit_nr;
        unsigned long timeout;
    } unrelated_key = {&unrelated_var, -1, 0},
      waited_key = {&waited_var, -1, 0};
    if (kb_wake_up_waitqueue(var_queue, 3u, 0, &unrelated_key) != 0 ||
        kb_wake_up_waitqueue(var_queue, 3u, 0, &waited_key) != 1)
    {
        kb_finish_wait(var_queue, embedded_entry);
        return 149;
    }
    kb_finish_wait(var_queue, embedded_entry);

    const size_t pool_element_size = 48;
    mempool_allow_alloc = 1;
    mempool_free_count = 0;
    void *pool = kb_mempool_create(
        2,
        core_helpers_mempool_alloc,
        core_helpers_mempool_free,
        (void *)&pool_element_size);
    if (pool == NULL) {
        return 145;
    }
    mempool_allow_alloc = 0;
    void *reserved = kb_mempool_alloc(pool, 0);
    if (reserved == NULL) {
        kb_mempool_destroy(pool);
        return 146;
    }
    kb_mempool_free(reserved, pool);
    kb_mempool_destroy(pool);
    if (mempool_free_count != 2) {
        return 147;
    }

    void *shrinker = kb_shrinker_alloc(0, "kobox-smoke");
    if (shrinker == NULL) {
        return 150;
    }
    void *count_fn = (void *)(uintptr_t)&core_helpers_shrinker_count;
    void *scan_fn = (void *)(uintptr_t)&core_helpers_shrinker_scan;
    memcpy(shrinker, &count_fn, sizeof(count_fn));
    memcpy((unsigned char *)shrinker + sizeof(void *), &scan_fn, sizeof(scan_fn));
    shrinker_count_calls = 0;
    shrinker_scan_calls = 0;
    kb_shrinker_register(shrinker);
    if (kb_shrinker_reclaim(5, 0) != 5 ||
        shrinker_count_calls != 1 || shrinker_scan_calls != 1)
    {
        kb_shrinker_free(shrinker);
        return 151;
    }
    kb_shrinker_free(shrinker);
    if (kb_shrinker_reclaim(5, 0) != 0) {
        return 152;
    }
    return 0;
}

static int run_page_model_smoke(void)
{
    kb_device_backend_t *backend = NULL;
    if (kb_linux_mock_device_create(&backend) != KB_OK || backend == NULL) {
        return 79;
    }
    kb_shim_set_device_backend(backend);
    void *page = kb_kvm_alloc_pages_stub(0, 0);
    kb_shim_set_device_backend(NULL);
    if (page == NULL) {
        kb_device_backend_destroy(backend);
        return 80;
    }

    const uintptr_t page_addr = (uintptr_t)page;
    const uintptr_t vmemmap = kb_linux_kvm_exported_vmemmap_base();
    const uintptr_t page_offset = kb_linux_kvm_exported_page_offset_base();
    const uintptr_t inline_address =
        page_offset + (((page_addr - vmemmap) / 64u) << 12u);
    void *payload = kb_linux_kvm_page_payload(page, 0, 4096);
    const int result = inline_address == (uintptr_t)payload ? 0 : 81;
    kb_kvm_release_pages(page, 0);
    kb_device_backend_destroy(backend);
    return result;
}

static int run_pagecache_truncate_smoke(void)
{
    enum {
        PAGE_SIZE = 4096,
        INODE_MAPPING_OFFSET = 0x30,
        FOLIO_BATCH_BYTES = 8 + 31 * sizeof(void *),
    };
    unsigned char mapping[256] = {0};
    void *folios[3] = {0};
    unsigned char *payloads[3] = {0};

    if ((intptr_t)kb_fs_subsystem_filemap_get_folio(mapping, 99, 0, 0) != -2) {
        return 88;
    }

    for (unsigned long index = 0; index < 3; ++index) {
        folios[index] = kb_fs_subsystem_filemap_get_folio(mapping, index, 0x4u, 0);
        if (folios[index] == NULL || (uintptr_t)folios[index] >= (uintptr_t)-4095) {
            return 89;
        }
        payloads[index] = kb_linux_kvm_page_payload(folios[index], 0, PAGE_SIZE);
        if (payloads[index] == NULL) {
            return 90;
        }
        memset(payloads[index], (int)(0x31 + index), PAGE_SIZE);
        kb_fs_subsystem_folio_put(folios[index]);
    }

    kb_fs_subsystem_truncate_inode_pages_range(
        mapping,
        PAGE_SIZE + 4,
        -1);
    for (size_t i = 0; i < 4; ++i) {
        if (payloads[1][i] != 0x32) {
            return 91;
        }
    }
    for (size_t i = 4; i < PAGE_SIZE; ++i) {
        if (payloads[1][i] != 0) {
            return 92;
        }
    }

    unsigned char batch[FOLIO_BATCH_BYTES];
    memset(batch, 0, sizeof(batch));
    unsigned long start = 0;
    if (kb_fs_subsystem_filemap_get_folios(
            mapping,
            &start,
            (unsigned long)-1,
            batch) != 2)
    {
        return 93;
    }
    kb_fs_subsystem_folio_batch_release(batch);

    unsigned char inode[256] = {0};
    void *mapping_pointer = mapping;
    memcpy(inode + INODE_MAPPING_OFFSET, &mapping_pointer, sizeof(mapping_pointer));
    memset(payloads[0], 0x5a, PAGE_SIZE);
    kb_fs_subsystem_pagecache_isize_extended(inode, 10, 20);
    for (size_t i = 0; i < PAGE_SIZE; ++i) {
        const unsigned char expected = (i >= 10 && i < 20) ? 0 : 0x5a;
        if (payloads[0][i] != expected) {
            return 94;
        }
    }

    kb_fs_subsystem_truncate_inode_pages_final(mapping);
    memset(batch, 0, sizeof(batch));
    start = 0;
    return kb_fs_subsystem_filemap_get_folios(
        mapping,
        &start,
        (unsigned long)-1,
        batch) == 0 ? 0 : 95;
}

static int run_pagecache_readahead_smoke(void)
{
    enum {
        MAPPING_HOST_OFFSET = 0x00,
        MAPPING_AOPS_OFFSET = 0x68,
        AOPS_READ_FOLIO_OFFSET = 0x08,
        AOPS_READAHEAD_OFFSET = 0x20,
        FILE_MODE_OFFSET = 0x0c,
        FILE_MAPPING_OFFSET = 0x18,
        FILE_INODE_OFFSET = 0x28,
        FILE_RA_OFFSET = 0x98,
        INODE_SIZE_OFFSET = 0x50,
        RACTL_FILE_OFFSET = 0x00,
        RACTL_MAPPING_OFFSET = 0x08,
        RACTL_RA_OFFSET = 0x10,
        RACTL_INDEX_OFFSET = 0x18,
        RACTL_NR_PAGES_OFFSET = 0x20,
        RACTL_BATCH_COUNT_OFFSET = 0x24,
        RA_START_OFFSET = 0x00,
        RA_SIZE_OFFSET = 0x08,
        RA_ASYNC_SIZE_OFFSET = 0x0c,
        RA_PAGES_OFFSET = 0x10,
        RA_PREV_POS_OFFSET = 0x18,
        KIOCB_FILE_OFFSET = 0x00,
        KIOCB_POS_OFFSET = 0x08,
        ITER_COUNT_OFFSET = 0x18,
        ITER_BUFFER_OFFSET = 0x20,
        ITER_BUFFER_CAPACITY_OFFSET = 0x78,
    };
    unsigned char inode[0x270] = {0};
    unsigned char mapping[0x100] = {0};
    unsigned char aops[0x80] = {0};
    unsigned char file[0xc0] = {0};
    unsigned char control[56] = {0};
    void *pointer = inode;
    memcpy(mapping + MAPPING_HOST_OFFSET, &pointer, sizeof(pointer));
    pointer = aops;
    memcpy(mapping + MAPPING_AOPS_OFFSET, &pointer, sizeof(pointer));
    pointer = (void *)(uintptr_t)&readahead_smoke_aop;
    memcpy(aops + AOPS_READAHEAD_OFFSET, &pointer, sizeof(pointer));
    pointer = (void *)(uintptr_t)&mmap_smoke_read_folio;
    memcpy(aops + AOPS_READ_FOLIO_OFFSET, &pointer, sizeof(pointer));
    const int64_t file_size = 64 * 1024;
    memcpy(inode + INODE_SIZE_OFFSET, &file_size, sizeof(file_size));
    pointer = mapping;
    memcpy(file + FILE_MAPPING_OFFSET, &pointer, sizeof(pointer));
    pointer = inode;
    memcpy(file + FILE_INODE_OFFSET, &pointer, sizeof(pointer));
    const uint32_t readable = 1;
    memcpy(file + FILE_MODE_OFFSET, &readable, sizeof(readable));
    const uint32_t ra_pages = 8;
    const uint64_t previous_position = UINT64_MAX;
    memcpy(file + FILE_RA_OFFSET + RA_PAGES_OFFSET,
        &ra_pages,
        sizeof(ra_pages));
    memcpy(file + FILE_RA_OFFSET + RA_PREV_POS_OFFSET,
        &previous_position,
        sizeof(previous_position));
    pointer = file;
    memcpy(control + RACTL_FILE_OFFSET, &pointer, sizeof(pointer));
    pointer = mapping;
    memcpy(control + RACTL_MAPPING_OFFSET, &pointer, sizeof(pointer));
    pointer = file + FILE_RA_OFFSET;
    memcpy(control + RACTL_RA_OFFSET, &pointer, sizeof(pointer));

    kb_fs_storage_trace_t before;
    kb_fs_storage_trace_t after;
    kb_fs_storage_trace_snapshot(&before);
    readahead_smoke_calls = 0;
    readahead_smoke_error = 0;
    memset(readahead_smoke_indices, 0, sizeof(readahead_smoke_indices));
    kb_fs_subsystem_page_cache_sync_ra(control, 2);
    kb_fs_storage_trace_snapshot(&after);
    uint32_t state_size = 0;
    uint32_t state_async_size = 0;
    uint32_t pending = 1;
    uint32_t batch = 1;
    memcpy(&state_size, file + FILE_RA_OFFSET + RA_SIZE_OFFSET, sizeof(state_size));
    memcpy(&state_async_size,
        file + FILE_RA_OFFSET + RA_ASYNC_SIZE_OFFSET,
        sizeof(state_async_size));
    memcpy(&pending, control + RACTL_NR_PAGES_OFFSET, sizeof(pending));
    memcpy(&batch, control + RACTL_BATCH_COUNT_OFFSET, sizeof(batch));
    if (readahead_smoke_error != 0 || readahead_smoke_calls != 4u ||
        readahead_smoke_indices[0] != 0 ||
        readahead_smoke_indices[1] != 1 ||
        readahead_smoke_indices[2] != 2 ||
        readahead_smoke_indices[3] != 3 ||
        state_size != 4 || state_async_size != 2 ||
        pending != 0 || batch != 0 ||
        after.readahead_aops_calls != before.readahead_aops_calls + 1u ||
        after.readahead_folios != before.readahead_folios + 4u)
    {
        kb_fs_subsystem_truncate_inode_pages_final(mapping);
        return 170;
    }
    for (unsigned long index = 0; index < 4; ++index) {
        void *folio = kb_fs_subsystem_filemap_get_folio(mapping, index, 0, 0);
        uint64_t flags = 0;
        unsigned char *payload = NULL;
        if (folio != NULL && (uintptr_t)folio < (uintptr_t)-4095) {
            memcpy(&flags, folio, sizeof(flags));
            payload = kb_linux_kvm_page_payload(folio, 0, 4096);
        }
        if (folio == NULL || (uintptr_t)folio >= (uintptr_t)-4095 ||
            (flags & 0x8u) == 0 || payload == NULL ||
            payload[0] != (unsigned char)index)
        {
            if (folio != NULL && (uintptr_t)folio < (uintptr_t)-4095) {
                kb_fs_subsystem_folio_put(folio);
            }
            kb_fs_subsystem_truncate_inode_pages_final(mapping);
            return 171;
        }
        kb_fs_subsystem_folio_put(folio);
    }
    kb_fs_subsystem_truncate_inode_pages_final(mapping);

    void *existing = kb_fs_subsystem_filemap_get_folio(mapping, 6, 0x4u, 0);
    if (existing == NULL || (uintptr_t)existing >= (uintptr_t)-4095) {
        return 172;
    }
    kb_fs_subsystem_folio_end_read(existing, 1);
    kb_fs_subsystem_folio_put(existing);
    memset(control, 0, sizeof(control));
    pointer = file;
    memcpy(control + RACTL_FILE_OFFSET, &pointer, sizeof(pointer));
    pointer = mapping;
    memcpy(control + RACTL_MAPPING_OFFSET, &pointer, sizeof(pointer));
    pointer = file + FILE_RA_OFFSET;
    memcpy(control + RACTL_RA_OFFSET, &pointer, sizeof(pointer));
    const unsigned long start = 5;
    memcpy(control + RACTL_INDEX_OFFSET, &start, sizeof(start));
    readahead_smoke_calls = 0;
    readahead_smoke_error = 0;
    kb_fs_subsystem_page_cache_ra_unbounded(control, 3, 1);
    if (readahead_smoke_error != 0 || readahead_smoke_calls != 2u ||
        readahead_smoke_indices[0] != 5 ||
        readahead_smoke_indices[1] != 7)
    {
        kb_fs_subsystem_truncate_inode_pages_final(mapping);
        return 173;
    }
    void *marked = kb_fs_subsystem_filemap_get_folio(mapping, 7, 0, 0);
    uint64_t marked_flags = 0;
    if (marked != NULL && (uintptr_t)marked < (uintptr_t)-4095) {
        memcpy(&marked_flags, marked, sizeof(marked_flags));
    }
    if (marked == NULL || (uintptr_t)marked >= (uintptr_t)-4095 ||
        (marked_flags & (1u << 16)) == 0)
    {
        if (marked != NULL && (uintptr_t)marked < (uintptr_t)-4095) {
            kb_fs_subsystem_folio_put(marked);
        }
        kb_fs_subsystem_truncate_inode_pages_final(mapping);
        return 174;
    }
    kb_fs_subsystem_folio_put(marked);
    kb_fs_subsystem_truncate_inode_pages_final(mapping);

    memset(aops + AOPS_READAHEAD_OFFSET, 0, sizeof(void *));
    memset(control, 0, sizeof(control));
    pointer = file;
    memcpy(control + RACTL_FILE_OFFSET, &pointer, sizeof(pointer));
    pointer = mapping;
    memcpy(control + RACTL_MAPPING_OFFSET, &pointer, sizeof(pointer));
    const unsigned long fallback_start = 10;
    memcpy(control + RACTL_INDEX_OFFSET, &fallback_start, sizeof(fallback_start));
    kb_fs_storage_trace_snapshot(&before);
    kb_fs_subsystem_page_cache_ra_unbounded(control, 2, 0);
    kb_fs_storage_trace_snapshot(&after);
    void *fallback = kb_fs_subsystem_filemap_get_folio(mapping, 10, 0, 0);
    uint64_t fallback_flags = 0;
    if (fallback != NULL && (uintptr_t)fallback < (uintptr_t)-4095) {
        memcpy(&fallback_flags, fallback, sizeof(fallback_flags));
    }
    if (fallback == NULL || (uintptr_t)fallback >= (uintptr_t)-4095 ||
        (fallback_flags & 0x8u) == 0 ||
        after.readahead_fallback_calls !=
            before.readahead_fallback_calls + 1u)
    {
        if (fallback != NULL && (uintptr_t)fallback < (uintptr_t)-4095) {
            kb_fs_subsystem_folio_put(fallback);
        }
        kb_fs_subsystem_truncate_inode_pages_final(mapping);
        return 175;
    }
    kb_fs_subsystem_folio_put(fallback);
    kb_fs_subsystem_truncate_inode_pages_final(mapping);

    pointer = (void *)(uintptr_t)&readahead_smoke_aop;
    memcpy(aops + AOPS_READAHEAD_OFFSET, &pointer, sizeof(pointer));
    memset(file + FILE_RA_OFFSET, 0, 32);
    memcpy(file + FILE_RA_OFFSET + RA_PAGES_OFFSET,
        &ra_pages,
        sizeof(ra_pages));
    memcpy(file + FILE_RA_OFFSET + RA_PREV_POS_OFFSET,
        &previous_position,
        sizeof(previous_position));
    unsigned char kiocb[64] = {0};
    unsigned char iter[128] = {0};
    unsigned char readback[8192] = {0};
    pointer = file;
    memcpy(kiocb + KIOCB_FILE_OFFSET, &pointer, sizeof(pointer));
    const uint64_t read_count = sizeof(readback);
    pointer = readback;
    memcpy(iter + ITER_COUNT_OFFSET, &read_count, sizeof(read_count));
    memcpy(iter + ITER_BUFFER_OFFSET, &pointer, sizeof(pointer));
    memcpy(iter + ITER_BUFFER_CAPACITY_OFFSET,
        &read_count,
        sizeof(read_count));
    readahead_smoke_calls = 0;
    readahead_smoke_error = 0;
    memset(readahead_smoke_indices, 0, sizeof(readahead_smoke_indices));
    kb_fs_storage_trace_snapshot(&before);
    const long generic_read = kb_fs_subsystem_generic_file_read_iter(
        kiocb,
        iter);
    kb_fs_storage_trace_snapshot(&after);
    uint64_t final_position = 0;
    uint64_t final_previous_position = 0;
    memcpy(&final_position, kiocb + KIOCB_POS_OFFSET, sizeof(final_position));
    memcpy(&final_previous_position,
        file + FILE_RA_OFFSET + RA_PREV_POS_OFFSET,
        sizeof(final_previous_position));
    if (generic_read != (long)sizeof(readback) ||
        readahead_smoke_error != 0 || readahead_smoke_calls != 4u ||
        readahead_smoke_indices[0] != 0 ||
        readahead_smoke_indices[1] != 1 ||
        readahead_smoke_indices[2] != 2 ||
        readahead_smoke_indices[3] != 3 ||
        readback[0] != 0 || readback[4095] != 0 ||
        readback[4096] != 1 || readback[8191] != 1 ||
        final_position != sizeof(readback) ||
        final_previous_position != sizeof(readback) ||
        after.readahead_aops_calls != before.readahead_aops_calls + 1u)
    {
        kb_fs_subsystem_truncate_inode_pages_final(mapping);
        return 176;
    }
    kb_fs_subsystem_truncate_inode_pages_final(mapping);

    memset(file + FILE_RA_OFFSET, 0, 32);
    memcpy(file + FILE_RA_OFFSET + RA_PAGES_OFFSET,
        &ra_pages,
        sizeof(ra_pages));
    memcpy(file + FILE_RA_OFFSET + RA_PREV_POS_OFFSET,
        &previous_position,
        sizeof(previous_position));
    readahead_smoke_calls = 0;
    readahead_smoke_error = 0;
    memset(readahead_smoke_indices, 0, sizeof(readahead_smoke_indices));
    for (uint64_t page = 0; page < 2; ++page) {
        unsigned char page_readback[4096] = {0};
        memset(kiocb, 0, sizeof(kiocb));
        memset(iter, 0, sizeof(iter));
        pointer = file;
        memcpy(kiocb + KIOCB_FILE_OFFSET, &pointer, sizeof(pointer));
        const uint64_t position = page * sizeof(page_readback);
        memcpy(kiocb + KIOCB_POS_OFFSET, &position, sizeof(position));
        const uint64_t page_count = sizeof(page_readback);
        pointer = page_readback;
        memcpy(iter + ITER_COUNT_OFFSET, &page_count, sizeof(page_count));
        memcpy(iter + ITER_BUFFER_OFFSET, &pointer, sizeof(pointer));
        memcpy(iter + ITER_BUFFER_CAPACITY_OFFSET,
            &page_count,
            sizeof(page_count));
        if (kb_fs_subsystem_generic_file_read_iter(kiocb, iter) !=
                (long)sizeof(page_readback) ||
            page_readback[0] != (unsigned char)page ||
            page_readback[sizeof(page_readback) - 1u] != (unsigned char)page)
        {
            kb_fs_subsystem_truncate_inode_pages_final(mapping);
            return 177;
        }
    }
    if (readahead_smoke_error != 0 || readahead_smoke_calls != 6u ||
        readahead_smoke_indices[0] != 0 ||
        readahead_smoke_indices[1] != 1 ||
        readahead_smoke_indices[2] != 2 ||
        readahead_smoke_indices[3] != 3 ||
        readahead_smoke_indices[4] != 4 ||
        readahead_smoke_indices[5] != 5)
    {
        kb_fs_subsystem_truncate_inode_pages_final(mapping);
        return 178;
    }
    kb_fs_subsystem_truncate_inode_pages_final(mapping);
    return 0;
}

static int run_pagecache_invalidate_smoke(void)
{
    unsigned char mapping[256] = {0};
    void *folios[3] = {0};
    for (unsigned long index = 0; index < 3; ++index) {
        folios[index] = kb_fs_subsystem_filemap_get_folio(mapping, index, 0x4u, 0);
        if (folios[index] == NULL ||
            (uintptr_t)folios[index] >= (uintptr_t)-4095)
        {
            kb_fs_subsystem_truncate_inode_pages_final(mapping);
            return 118;
        }
    }
    if (!kb_fs_subsystem_folio_mark_dirty(folios[1])) {
        kb_fs_subsystem_truncate_inode_pages_final(mapping);
        return 119;
    }
    kb_fs_subsystem_folio_put(folios[0]);
    kb_fs_subsystem_folio_put(folios[1]);
    if (kb_fs_subsystem_invalidate_mapping_pages(mapping, 0, 2) != 1) {
        kb_fs_subsystem_folio_put(folios[2]);
        kb_fs_subsystem_truncate_inode_pages_final(mapping);
        return 120;
    }
    uint64_t nrpages = 0;
    memcpy(&nrpages, mapping + 0x58, sizeof(nrpages));
    if (nrpages != 2) {
        kb_fs_subsystem_folio_put(folios[2]);
        kb_fs_subsystem_truncate_inode_pages_final(mapping);
        return 121;
    }
    (void)kb_fs_subsystem_folio_clear_dirty_for_io(folios[1]);
    kb_fs_subsystem_folio_put(folios[2]);
    if (kb_fs_subsystem_invalidate_mapping_pages(mapping, 0, 2) != 2) {
        kb_fs_subsystem_truncate_inode_pages_final(mapping);
        return 122;
    }
    memcpy(&nrpages, mapping + 0x58, sizeof(nrpages));
    return nrpages == 0 ? 0 : 123;
}

static int run_buffer_folio_helpers_smoke(void)
{
    enum {
        PAGE_SIZE = 4096,
        BH_NEW = 1u << 5,
        FOLIO_PRIVATE_OFFSET = 0x28,
        BH_ASSOC_MAP_OFFSET = 0x58,
        BH_ASSOC_LIST_OFFSET = 0x48,
        BH_REFCOUNT_OFFSET = 0x60,
    };
    void *allocated_buffer_head = kb_fs_subsystem_alloc_buffer_head(0);
    if (allocated_buffer_head == NULL) {
        return 126;
    }
    void *association_next = NULL;
    void *association_prev = NULL;
    uint32_t allocated_refcount = 0;
    memcpy(&association_next,
        (const unsigned char *)allocated_buffer_head + BH_ASSOC_LIST_OFFSET,
        sizeof(association_next));
    memcpy(&association_prev,
        (const unsigned char *)allocated_buffer_head + BH_ASSOC_LIST_OFFSET + sizeof(void *),
        sizeof(association_prev));
    memcpy(&allocated_refcount,
        (const unsigned char *)allocated_buffer_head + BH_REFCOUNT_OFFSET,
        sizeof(allocated_refcount));
    if (association_next != (unsigned char *)allocated_buffer_head + BH_ASSOC_LIST_OFFSET ||
        association_prev != association_next || allocated_refcount != 1u)
    {
        kb_fs_subsystem_free_buffer_head(allocated_buffer_head);
        return 127;
    }
    kb_fs_subsystem_free_buffer_head(allocated_buffer_head);
    unsigned char mapping[256] = {0};
    void *folio = kb_fs_subsystem_filemap_get_folio(mapping, 8, 0x4u, 0);
    if (folio == NULL || (uintptr_t)folio >= (uintptr_t)-4095) {
        return 100;
    }
    unsigned char *payload = kb_linux_kvm_page_payload(folio, 0, PAGE_SIZE);
    if (payload == NULL ||
        kb_fs_subsystem_create_empty_buffers(folio, 1024, BH_NEW) == NULL)
    {
        kb_fs_subsystem_folio_put(folio);
        kb_fs_subsystem_truncate_inode_pages_final(mapping);
        return 101;
    }
    void *buffer_head = NULL;
    memcpy(
        &buffer_head,
        (const unsigned char *)folio + FOLIO_PRIVATE_OFFSET,
        sizeof(buffer_head));
    unsigned char inode[256] = {0};
    void *mapping_pointer = mapping;
    memcpy(inode + 0x30, &mapping_pointer, sizeof(mapping_pointer));
    kb_fs_subsystem_mark_buffer_dirty_inode(buffer_head, mapping);
    void *association = NULL;
    memcpy(
        &association,
        (const unsigned char *)buffer_head + BH_ASSOC_MAP_OFFSET,
        sizeof(association));
    if (association != mapping) {
        kb_fs_subsystem_folio_put(folio);
        kb_fs_subsystem_truncate_inode_pages_final(mapping);
        return 124;
    }
    kb_fs_subsystem_invalidate_inode_buffers(inode);
    memcpy(
        &association,
        (const unsigned char *)buffer_head + BH_ASSOC_MAP_OFFSET,
        sizeof(association));
    if (association != NULL) {
        kb_fs_subsystem_folio_put(folio);
        kb_fs_subsystem_truncate_inode_pages_final(mapping);
        return 125;
    }
    memset(payload, 0xa5, PAGE_SIZE);
    kb_fs_subsystem_folio_zero_new_buffers(folio, 100, 200);
    for (size_t i = 0; i < PAGE_SIZE; ++i) {
        const unsigned char expected = (i >= 100 && i < 200) ? 0 : 0xa5;
        if (payload[i] != expected) {
            kb_fs_subsystem_folio_put(folio);
            kb_fs_subsystem_truncate_inode_pages_final(mapping);
            return 102;
        }
    }
    if (!kb_fs_subsystem_block_is_partially_uptodate(folio, 0, 512)) {
        kb_fs_subsystem_folio_put(folio);
        kb_fs_subsystem_truncate_inode_pages_final(mapping);
        return 103;
    }
    kb_fs_subsystem_block_commit_write(folio, 1024, 2048);
    if (!kb_fs_subsystem_block_is_partially_uptodate(folio, 1024, 1024)) {
        kb_fs_subsystem_folio_put(folio);
        kb_fs_subsystem_truncate_inode_pages_final(mapping);
        return 104;
    }
    kb_fs_subsystem_block_invalidate_folio(folio, 0, PAGE_SIZE);
    void *private_data = (void *)(uintptr_t)1;
    memcpy(&private_data,
        (const unsigned char *)folio + FOLIO_PRIVATE_OFFSET,
        sizeof(private_data));
    if (private_data != NULL ||
        !kb_fs_subsystem_filemap_release_folio(folio, 0) ||
        kb_fs_subsystem_folio_mkclean(folio) != 0)
    {
        kb_fs_subsystem_folio_put(folio);
        kb_fs_subsystem_truncate_inode_pages_final(mapping);
        return 130;
    }
    kb_fs_subsystem_folio_put(folio);
    kb_fs_subsystem_truncate_inode_pages_final(mapping);
    return 0;
}

static int run_write_cache_pages_smoke(void)
{
    enum { WRITEBACK_CONTROL_BYTES = 384 };
    unsigned char mapping[256] = {0};
    const unsigned long indices[] = {4, 2};
    for (size_t i = 0; i < 2; ++i) {
        void *folio = kb_fs_subsystem_filemap_get_folio(mapping, indices[i], 0x4u, 0);
        if (folio == NULL || (uintptr_t)folio >= (uintptr_t)-4095) {
            kb_fs_subsystem_truncate_inode_pages_final(mapping);
            return 106;
        }
        if (!kb_fs_subsystem_folio_mark_dirty(folio)) {
            kb_fs_subsystem_folio_put(folio);
            kb_fs_subsystem_truncate_inode_pages_final(mapping);
            return 107;
        }
        kb_fs_subsystem_folio_put(folio);
    }
    unsigned char writeback_control[WRITEBACK_CONTROL_BYTES] = {0};
    int64_t nr_to_write = 2;
    int64_t range_start = 0;
    int64_t range_end = INT64_MAX;
    memcpy(writeback_control, &nr_to_write, sizeof(nr_to_write));
    memcpy(writeback_control + 0x10, &range_start, sizeof(range_start));
    memcpy(writeback_control + 0x18, &range_end, sizeof(range_end));
    write_cache_count = 0;
    memset(write_cache_indices, 0, sizeof(write_cache_indices));
    int callback_context = 1;
    const int status = kb_fs_subsystem_write_cache_pages(
        mapping,
        writeback_control,
        write_cache_smoke_page,
        &callback_context);
    memcpy(&nr_to_write, writeback_control, sizeof(nr_to_write));
    kb_fs_subsystem_truncate_inode_pages_final(mapping);
    return status == 0 && write_cache_count == 2 &&
        write_cache_indices[0] == 2 && write_cache_indices[1] == 4 &&
        nr_to_write == 0 ? 0 : 108;
}

static int run_setattr_helpers_smoke(void)
{
    unsigned char inode[0x100] = {0};
    unsigned char dentry[0x80] = {0};
    struct {
        uint32_t valid;
        uint16_t mode;
        uint16_t padding;
        uint32_t uid;
        uint32_t gid;
        int64_t size;
        struct { int64_t sec; int64_t nsec; } atime;
        struct { int64_t sec; int64_t nsec; } mtime;
        struct { int64_t sec; int64_t nsec; } ctime;
        void *file;
    } iattr = {0};
    void *inode_pointer = inode;
    memcpy(dentry + 0x30, &inode_pointer, sizeof(inode_pointer));
    const uint16_t regular_mode = 0100644;
    memcpy(inode, &regular_mode, sizeof(regular_mode));
    iattr.valid = (1u << 0) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 6);
    iattr.mode = 0100600;
    iattr.size = 8192;
    iattr.atime.sec = 11;
    iattr.atime.nsec = 12;
    iattr.mtime.sec = 21;
    iattr.mtime.nsec = 22;
    iattr.ctime.sec = 31;
    iattr.ctime.nsec = 32;
    if (kb_fs_subsystem_setattr_prepare(NULL, dentry, &iattr) != 0) {
        return 82;
    }
    kb_fs_subsystem_setattr_copy(NULL, inode, &iattr);
    uint16_t mode = 0;
    uint64_t size = 0;
    int64_t atime_sec = 0;
    uint32_t mtime_nsec = 0;
    memcpy(&mode, inode, sizeof(mode));
    memcpy(&size, inode + 0x50, sizeof(size));
    memcpy(&atime_sec, inode + 0x58, sizeof(atime_sec));
    memcpy(&mtime_nsec, inode + 0x74, sizeof(mtime_nsec));
    if (mode != 0100600 || size != 8192 || atime_sec != 11 || mtime_nsec != 22) {
        return 83;
    }
    iattr.atime.nsec = 1000000000ll;
    return kb_fs_subsystem_setattr_prepare(NULL, dentry, &iattr) == -22 ? 0 : 84;
}

static int run_simple_get_link_smoke(void)
{
    unsigned char inode[0x260] = {0};
    const char target[] = "../../devices/pci0000:00/input0";
    const char *link = target;
    memcpy(inode + 0x240, &link, sizeof(link));
    return kb_fs_subsystem_simple_get_link(NULL, inode, NULL) == target ? 0 : 85;
}

static int run_inode_reference_smoke(void)
{
    unsigned char super_block[2048] = {0};
    unsigned char super_ops[0x48] = {0};
    void *super_ops_pointer = super_ops;
    void (*evict_inode)(void *) = inode_reference_smoke_evict;
    memcpy(super_block + 0x30, &super_ops_pointer, sizeof(super_ops_pointer));
    memcpy(super_ops + 0x30, &evict_inode, sizeof(evict_inode));
    super_block[0x14] = 12;
    evict_inode_count = 0;
    evict_inode_saw_freeing = 0;
    void *first = kb_fs_subsystem_iget_locked(super_block, 42);
    void *second = kb_fs_subsystem_iget_locked(super_block, 42);
    uint32_t references = 0;
    if (first == NULL || second != first) {
        return 86;
    }
    memcpy(&references, (const unsigned char *)first + 0x150, sizeof(references));
    if (references != 2 || kb_fs_subsystem_igrab(first) != first) {
        return 87;
    }
    kb_fs_subsystem_ihold(first);
    memcpy(&references, (const unsigned char *)first + 0x150, sizeof(references));
    if (references != 4) {
        return 88;
    }
    kb_fs_subsystem_iput(first);
    kb_fs_subsystem_iput(first);
    kb_fs_subsystem_iput(first);
    kb_fs_subsystem_iput(first);
    return evict_inode_count == 1 && evict_inode_saw_freeing ? 0 : 113;
}

static int run_insert_inode_smoke(void)
{
    unsigned char super_block[2048] = {0};
    super_block[0x14] = 12;
    void *first = kb_fs_subsystem_new_inode(super_block);
    void *second = kb_fs_subsystem_new_inode(super_block);
    const uint64_t inode_number = 77;
    if (first == NULL || second == NULL) {
        kb_fs_subsystem_iput(first);
        kb_fs_subsystem_iput(second);
        return 126;
    }
    memcpy((unsigned char *)first + 0x40, &inode_number, sizeof(inode_number));
    memcpy((unsigned char *)second + 0x40, &inode_number, sizeof(inode_number));
    if (kb_fs_subsystem_insert_inode_locked(first) != 0) {
        kb_fs_subsystem_iput(first);
        kb_fs_subsystem_iput(second);
        return 127;
    }
    uint64_t state = 0;
    memcpy(&state, (const unsigned char *)first + 0x90, sizeof(state));
    if ((state & ((1u << 0) | (1u << 14))) !=
        ((1u << 0) | (1u << 14)))
    {
        kb_fs_subsystem_iput(first);
        kb_fs_subsystem_iput(second);
        return 128;
    }
    kb_fs_subsystem_unlock_new_inode(first);
    memcpy(&state, (const unsigned char *)first + 0x90, sizeof(state));
    if ((state & ((1u << 0) | (1u << 14))) != 0 ||
        kb_fs_subsystem_insert_inode_locked(second) != -16)
    {
        kb_fs_subsystem_iput(first);
        kb_fs_subsystem_iput(second);
        return 129;
    }
    kb_fs_subsystem_iput(first);
    kb_fs_subsystem_iput(second);
    return 0;
}

static int run_errseq_smoke(void)
{
    uint32_t sequence = 0;
    uint32_t since = 0;
    if (kb_fs_subsystem_errseq_sample(&sequence) != 0 ||
        kb_fs_subsystem_errseq_set(&sequence, -5) != 0 ||
        kb_fs_subsystem_errseq_check(&sequence, since) != -5 ||
        kb_fs_subsystem_errseq_sample(&sequence) != 0 ||
        kb_fs_subsystem_errseq_check_and_advance(&sequence, &since) != -5 ||
        (since & (1u << 12)) == 0 ||
        kb_fs_subsystem_errseq_check(&sequence, since) != 0)
    {
        return 114;
    }
    const uint32_t previous = since;
    if (kb_fs_subsystem_errseq_set(&sequence, -28) != previous ||
        kb_fs_subsystem_errseq_check(&sequence, since) != -28 ||
        kb_fs_subsystem_errseq_check_and_advance(&sequence, &since) != -28 ||
        kb_fs_subsystem_errseq_check(&sequence, since) != 0)
    {
        return 115;
    }

    unsigned char mapping[256] = {0};
    unsigned char file[256] = {0};
    void *mapping_pointer = mapping;
    unsigned long flags = 3;
    memcpy(file + 0x18, &mapping_pointer, sizeof(mapping_pointer));
    memcpy(mapping + 0x70, &flags, sizeof(flags));
    kb_fs_subsystem_filemap_set_wb_err(mapping, -28);
    if (kb_fs_subsystem_file_check_and_advance_wb_err(file) != -28 ||
        kb_fs_subsystem_file_check_and_advance_wb_err(file) != 0)
    {
        return 116;
    }
    memcpy(&flags, mapping + 0x70, sizeof(flags));
    return flags == 0 ? 0 : 117;
}

static int run_inode_sync_smoke(void)
{
    enum {
        SUPER_OPS_OFFSET = 0x30,
        DIRTY_INODE_OFFSET = 0x18,
        WRITE_INODE_OFFSET = 0x20,
        SYNC_FS_OFFSET = 0x40,
    };
    unsigned char super_block[2048] = {0};
    unsigned char super_ops[0x48] = {0};
    void *super_ops_pointer = super_ops;
    void (*dirty_inode)(void *, int) = sync_smoke_dirty_inode;
    int (*write_inode)(void *, void *) = sync_smoke_write_inode;
    int (*sync_fs)(void *, int) = sync_smoke_super;
    memcpy(super_block + SUPER_OPS_OFFSET, &super_ops_pointer, sizeof(super_ops_pointer));
    memcpy(super_ops + DIRTY_INODE_OFFSET, &dirty_inode, sizeof(dirty_inode));
    memcpy(super_ops + WRITE_INODE_OFFSET, &write_inode, sizeof(write_inode));
    memcpy(super_ops + SYNC_FS_OFFSET, &sync_fs, sizeof(sync_fs));
    super_block[0x14] = 12;

    sync_dirty_inode_count = 0;
    sync_write_inode_count = 0;
    sync_super_count = 0;
    sync_write_inode_wait = 0;
    void *inode = kb_fs_subsystem_iget_locked(super_block, 314);
    if (inode == NULL) {
        return 96;
    }
    kb_fs_subsystem_mark_inode_dirty(inode, 1u << 3);
    if (sync_dirty_inode_count != 1 ||
        kb_fs_subsystem_sync_inode_metadata(inode, 1) != 0 ||
        sync_write_inode_count != 1 || !sync_write_inode_wait)
    {
        kb_fs_subsystem_iput(inode);
        return 97;
    }

    kb_fs_subsystem_mark_inode_dirty(inode, 1u << 3);
    kb_fs_subsystem_try_to_writeback_inodes_sb(super_block, 0);
    if (sync_dirty_inode_count != 2 ||
        sync_write_inode_count != 2 || sync_write_inode_wait)
    {
        kb_fs_subsystem_iput(inode);
        return 98;
    }
    if (kb_fs_subsystem_sync_super(super_block, 1) != 0 || sync_super_count != 1) {
        kb_fs_subsystem_iput(inode);
        return 99;
    }
    unsigned char file[64] = {0};
    void *inode_pointer = inode;
    memcpy(file + 0x28, &inode_pointer, sizeof(inode_pointer));
    if (kb_fs_subsystem_file_modified(file) != 0 || sync_dirty_inode_count != 3) {
        kb_fs_subsystem_iput(inode);
        return 109;
    }
    int64_t mtime = 0;
    int64_t ctime = 0;
    memcpy(&mtime, (const unsigned char *)inode + 0x60, sizeof(mtime));
    memcpy(&ctime, (const unsigned char *)inode + 0x68, sizeof(ctime));
    if (mtime <= 0 || ctime != mtime) {
        kb_fs_subsystem_iput(inode);
        return 110;
    }
    kb_fs_subsystem_iput(inode);
    return 0;
}

static int run_percpu_rwsem_lifecycle_smoke(void)
{
    unsigned char sem[0x60] = {0};
    if (kb_percpu_init_rwsem(sem, "kobox-test", NULL) != 0) {
        return 70;
    }
    void *read_count = NULL;
    memcpy(&read_count, sem + 0x30, sizeof(read_count));
    if (read_count == NULL || kb_percpu_is_read_locked(sem)) {
        kb_percpu_free_rwsem(sem);
        return 71;
    }
    if (!kb_percpu_down_read(sem, 0) || !kb_percpu_is_read_locked(sem)) {
        kb_percpu_free_rwsem(sem);
        return 72;
    }
    kb_percpu_down_write(sem);
    if (kb_percpu_is_read_locked(sem)) {
        kb_percpu_free_rwsem(sem);
        return 73;
    }
    kb_percpu_up_write(sem);
    if (!kb_percpu_is_read_locked(sem)) {
        kb_percpu_free_rwsem(sem);
        return 74;
    }
    kb_percpu_up_read(sem);
    if (kb_percpu_is_read_locked(sem)) {
        kb_percpu_free_rwsem(sem);
        return 75;
    }
    kb_percpu_free_rwsem(sem);
    memcpy(&read_count, sem + 0x30, sizeof(read_count));
    return read_count == NULL ? 0 : 76;
}

static void *load_pointer_field(const void *base, size_t offset)
{
    void *value = NULL;
    memcpy(&value, (const uint8_t *)base + offset, sizeof(value));
    return value;
}

static int run_inode_init_once_smoke(void)
{
    enum {
        INODE_BYTES = 0x270,
        INODE_HASH_OFFSET = 0xd0,
        INODE_IO_LIST_OFFSET = 0xe0,
        INODE_LRU_OFFSET = 0x100,
        INODE_SB_LIST_OFFSET = 0x110,
        INODE_WB_LIST_OFFSET = 0x120,
        INODE_DATA_PRIVATE_LIST_OFFSET = 0x1f0,
        INODE_DEVICES_OFFSET = 0x230,
    };
    uint8_t inode[INODE_BYTES];
    memset(inode, 0xa5, sizeof(inode));
    kb_fs_subsystem_inode_init_once(inode);

    const size_t list_offsets[] = {
        INODE_IO_LIST_OFFSET,
        INODE_LRU_OFFSET,
        INODE_SB_LIST_OFFSET,
        INODE_WB_LIST_OFFSET,
        INODE_DATA_PRIVATE_LIST_OFFSET,
        INODE_DEVICES_OFFSET,
    };
    for (size_t i = 0; i < sizeof(list_offsets) / sizeof(list_offsets[0]); ++i) {
        void *head = inode + list_offsets[i];
        if (load_pointer_field(inode, list_offsets[i]) != head ||
            load_pointer_field(inode, list_offsets[i] + sizeof(void *)) != head)
        {
            return 32;
        }
    }
    if (load_pointer_field(inode, INODE_HASH_OFFSET) != NULL ||
        load_pointer_field(inode, INODE_HASH_OFFSET + sizeof(void *)) != NULL ||
        inode[0] != 0 || inode[sizeof(inode) - 1] != 0)
    {
        return 33;
    }
    return 0;
}

static int run_inode_version_and_flags_smoke(void)
{
    enum {
        INODE_FLAGS_OFFSET = 0x0c,
        INODE_VERSION_OFFSET = 0x140,
    };
    unsigned char inode[0x270] = {0};
    uint32_t flags = 0xa5a500f0u;
    uint64_t version = 8u;
    memcpy(inode + INODE_FLAGS_OFFSET, &flags, sizeof(flags));
    memcpy(inode + INODE_VERSION_OFFSET, &version, sizeof(version));

    if (kb_fs_subsystem_inode_maybe_inc_iversion(inode, 0) != 0 ||
        kb_fs_subsystem_inode_query_iversion(inode) != 4u)
    {
        return 199;
    }
    memcpy(&version, inode + INODE_VERSION_OFFSET, sizeof(version));
    if (version != 9u ||
        kb_fs_subsystem_inode_maybe_inc_iversion(inode, 0) != 1)
    {
        return 200;
    }
    memcpy(&version, inode + INODE_VERSION_OFFSET, sizeof(version));
    if (version != 10u ||
        kb_fs_subsystem_inode_query_iversion(inode) != 5u ||
        kb_fs_subsystem_inode_maybe_inc_iversion(inode, 1) != 1)
    {
        return 201;
    }
    memcpy(&version, inode + INODE_VERSION_OFFSET, sizeof(version));
    if (version != 12u) {
        return 202;
    }

    kb_fs_subsystem_inode_set_flags(inode, 0x00001234u, 0x0000ffffu);
    memcpy(&flags, inode + INODE_FLAGS_OFFSET, sizeof(flags));
    if (flags != 0xa5a51234u) {
        return 203;
    }
    kb_fs_subsystem_inode_set_flags(inode, 0x00f00000u, 0x00ff0000u);
    memcpy(&flags, inode + INODE_FLAGS_OFFSET, sizeof(flags));
    return flags == 0xa5f01234u ? 0 : 204;
}

static int run_atime_and_file_time_smoke(void)
{
    enum {
        INODE_FLAGS_OFFSET = 0x0c,
        INODE_SB_OFFSET = 0x28,
        INODE_ATIME_SEC_OFFSET = 0x58,
        INODE_MTIME_SEC_OFFSET = 0x60,
        INODE_CTIME_SEC_OFFSET = 0x68,
        INODE_STATE_OFFSET = 0x90,
        INODE_VERSION_OFFSET = 0x140,
        DENTRY_INODE_OFFSET = 0x30,
        SUPER_FLAGS_OFFSET = 0x50,
        MOUNT_FLAGS_OFFSET = 0x10,
        MNT_RELATIME = 0x20,
        SB_I_VERSION = 1u << 23,
        SB_LAZYTIME = 1u << 25,
        I_DIRTY_SYNC = 1u << 3,
        I_DIRTY_TIME = 1u << 11,
        S_NOATIME = 1u << 1,
    };
    unsigned char super_block[2048] = {0};
    unsigned char inode[0x270] = {0};
    unsigned char dentry[512] = {0};
    unsigned char mount[32] = {0};
    unsigned char path[16] = {0};
    unsigned char file[0x90] = {0};
    void *pointer = super_block;
    memcpy(inode + INODE_SB_OFFSET, &pointer, sizeof(pointer));
    pointer = inode;
    memcpy(dentry + DENTRY_INODE_OFFSET, &pointer, sizeof(pointer));
    pointer = mount;
    memcpy(path, &pointer, sizeof(pointer));
    pointer = dentry;
    memcpy(path + sizeof(pointer), &pointer, sizeof(pointer));
    uint32_t mount_flags = MNT_RELATIME;
    memcpy(mount + MOUNT_FLAGS_OFFSET, &mount_flags, sizeof(mount_flags));
    uint16_t mode = 0100000 | 0644;
    memcpy(inode, &mode, sizeof(mode));
    int64_t atime = 100;
    int64_t mtime = 200;
    int64_t ctime = 150;
    memcpy(inode + INODE_ATIME_SEC_OFFSET, &atime, sizeof(atime));
    memcpy(inode + INODE_MTIME_SEC_OFFSET, &mtime, sizeof(mtime));
    memcpy(inode + INODE_CTIME_SEC_OFFSET, &ctime, sizeof(ctime));

    kb_fs_subsystem_touch_atime(path);
    uint64_t state = 0;
    memcpy(&atime, inode + INODE_ATIME_SEC_OFFSET, sizeof(atime));
    memcpy(&state, inode + INODE_STATE_OFFSET, sizeof(state));
    if (atime <= 200 || (state & I_DIRTY_SYNC) == 0) {
        return 205;
    }

    uint32_t inode_flags = S_NOATIME;
    memcpy(inode + INODE_FLAGS_OFFSET, &inode_flags, sizeof(inode_flags));
    const int64_t preserved_atime = atime - 10;
    memcpy(inode + INODE_ATIME_SEC_OFFSET, &preserved_atime,
        sizeof(preserved_atime));
    kb_fs_subsystem_touch_atime(path);
    memcpy(&atime, inode + INODE_ATIME_SEC_OFFSET, sizeof(atime));
    if (atime != preserved_atime) {
        return 206;
    }

    inode_flags = 0;
    state = 0;
    uint64_t super_flags = SB_LAZYTIME;
    mount_flags = 0;
    memcpy(inode + INODE_FLAGS_OFFSET, &inode_flags, sizeof(inode_flags));
    memcpy(inode + INODE_STATE_OFFSET, &state, sizeof(state));
    memcpy(super_block + SUPER_FLAGS_OFFSET, &super_flags, sizeof(super_flags));
    memcpy(mount + MOUNT_FLAGS_OFFSET, &mount_flags, sizeof(mount_flags));
    memcpy(inode + INODE_ATIME_SEC_OFFSET, &preserved_atime,
        sizeof(preserved_atime));
    kb_fs_subsystem_touch_atime(path);
    memcpy(&state, inode + INODE_STATE_OFFSET, sizeof(state));
    if ((state & I_DIRTY_TIME) == 0 || (state & I_DIRTY_SYNC) != 0) {
        return 207;
    }

    super_flags = SB_I_VERSION;
    state = 0;
    uint64_t version = 4;
    memcpy(super_block + SUPER_FLAGS_OFFSET, &super_flags, sizeof(super_flags));
    memcpy(inode + INODE_STATE_OFFSET, &state, sizeof(state));
    memcpy(inode + INODE_VERSION_OFFSET, &version, sizeof(version));
    pointer = inode;
    memcpy(file + 0x28, &pointer, sizeof(pointer));
    if (kb_fs_subsystem_file_update_time(file) != 0) {
        return 208;
    }
    memcpy(&version, inode + INODE_VERSION_OFFSET, sizeof(version));
    memcpy(&state, inode + INODE_STATE_OFFSET, sizeof(state));
    return version == 6 && (state & I_DIRTY_SYNC) != 0 ? 0 : 209;
}

static int run_inode_timestamp_and_sync_policy_smoke(void)
{
    enum {
        INODE_MODE_OFFSET = 0x0,
        INODE_FLAGS_OFFSET = 0x0c,
        INODE_SB_OFFSET = 0x28,
        INODE_ATIME_SEC_OFFSET = 0x58,
        INODE_MTIME_SEC_OFFSET = 0x60,
        INODE_CTIME_SEC_OFFSET = 0x68,
        INODE_ATIME_NSEC_OFFSET = 0x70,
        INODE_MTIME_NSEC_OFFSET = 0x74,
        INODE_CTIME_NSEC_OFFSET = 0x78,
        SUPER_FLAGS_OFFSET = 0x50,
        SB_SYNCHRONOUS = 1u << 4,
        SB_DIRSYNC = 1u << 7,
        S_SYNC = 1u << 0,
        S_DIRSYNC = 1u << 6,
    };
    unsigned char super_block[2048] = {0};
    unsigned char inode[0x270] = {0};
    void *super_pointer = super_block;
    memcpy(inode + INODE_SB_OFFSET, &super_pointer, sizeof(super_pointer));

    kb_fs_timespec64_t timestamp =
        kb_fs_subsystem_simple_inode_init_ts(inode);
    int64_t atime = 0;
    int64_t mtime = 0;
    int64_t ctime = 0;
    uint32_t atime_nsec = 1;
    uint32_t mtime_nsec = 1;
    uint32_t ctime_nsec = 1;
    memcpy(&atime, inode + INODE_ATIME_SEC_OFFSET, sizeof(atime));
    memcpy(&mtime, inode + INODE_MTIME_SEC_OFFSET, sizeof(mtime));
    memcpy(&ctime, inode + INODE_CTIME_SEC_OFFSET, sizeof(ctime));
    memcpy(&atime_nsec, inode + INODE_ATIME_NSEC_OFFSET, sizeof(atime_nsec));
    memcpy(&mtime_nsec, inode + INODE_MTIME_NSEC_OFFSET, sizeof(mtime_nsec));
    memcpy(&ctime_nsec, inode + INODE_CTIME_NSEC_OFFSET, sizeof(ctime_nsec));
    if (timestamp.tv_sec <= 0 || timestamp.tv_nsec != 0 ||
        atime != timestamp.tv_sec || mtime != timestamp.tv_sec ||
        ctime != timestamp.tv_sec || atime_nsec != 0 ||
        mtime_nsec != 0 || ctime_nsec != 0)
    {
        return 240;
    }

    uint16_t mode = 0100000u | 0644u;
    memcpy(inode + INODE_MODE_OFFSET, &mode, sizeof(mode));
    if (kb_fs_subsystem_inode_needs_sync(inode) != 0) {
        return 241;
    }
    uint64_t super_flags = SB_DIRSYNC;
    memcpy(super_block + SUPER_FLAGS_OFFSET, &super_flags, sizeof(super_flags));
    if (kb_fs_subsystem_inode_needs_sync(inode) != 0) {
        return 242;
    }
    mode = 0040000u | 0755u;
    memcpy(inode + INODE_MODE_OFFSET, &mode, sizeof(mode));
    if (kb_fs_subsystem_inode_needs_sync(inode) != 1) {
        return 243;
    }
    mode = 0100000u | 0644u;
    super_flags = SB_SYNCHRONOUS;
    memcpy(inode + INODE_MODE_OFFSET, &mode, sizeof(mode));
    memcpy(super_block + SUPER_FLAGS_OFFSET, &super_flags, sizeof(super_flags));
    if (kb_fs_subsystem_inode_needs_sync(inode) != 1) {
        return 244;
    }
    super_flags = 0;
    uint32_t inode_flags = S_SYNC;
    memcpy(super_block + SUPER_FLAGS_OFFSET, &super_flags, sizeof(super_flags));
    memcpy(inode + INODE_FLAGS_OFFSET, &inode_flags, sizeof(inode_flags));
    if (kb_fs_subsystem_inode_needs_sync(inode) != 1) {
        return 245;
    }
    inode_flags = S_DIRSYNC;
    memcpy(inode + INODE_FLAGS_OFFSET, &inode_flags, sizeof(inode_flags));
    if (kb_fs_subsystem_inode_needs_sync(inode) != 0) {
        return 246;
    }
    mode = 0040000u | 0755u;
    memcpy(inode + INODE_MODE_OFFSET, &mode, sizeof(mode));
    return kb_fs_subsystem_inode_needs_sync(inode) == 1 ? 0 : 247;
}

static int run_generic_metadata_seek_and_error_smoke(void)
{
    enum {
        INODE_MODE_OFFSET = 0x00,
        INODE_UID_OFFSET = 0x04,
        INODE_GID_OFFSET = 0x08,
        INODE_SB_OFFSET = 0x28,
        INODE_MAPPING_OFFSET = 0x30,
        INODE_NUMBER_OFFSET = 0x40,
        INODE_NLINK_OFFSET = 0x48,
        INODE_RDEV_OFFSET = 0x4c,
        INODE_SIZE_OFFSET = 0x50,
        INODE_ATIME_SEC_OFFSET = 0x58,
        INODE_MTIME_SEC_OFFSET = 0x60,
        INODE_CTIME_SEC_OFFSET = 0x68,
        INODE_ATIME_NSEC_OFFSET = 0x70,
        INODE_MTIME_NSEC_OFFSET = 0x74,
        INODE_CTIME_NSEC_OFFSET = 0x78,
        INODE_BLKBITS_OFFSET = 0x86,
        INODE_BLOCKS_OFFSET = 0x88,
        INODE_VERSION_OFFSET = 0x140,
        MAPPING_HOST_OFFSET = 0x00,
        SUPER_DEVICE_OFFSET = 0x10,
        SUPER_FLAGS_OFFSET = 0x50,
        SB_I_VERSION = 1u << 23,
        STATX_CHANGE_COOKIE = 0x40000000u,
        FILE_VERSION_OFFSET = 0x30,
        FILE_POSITION_OFFSET = 0x70,
    };
    unsigned char super_block[2048] = {0};
    unsigned char inode[0x270] = {0};
    unsigned char mapping[256] = {0};
    unsigned char stat[160] = {0};
    void *pointer = super_block;
    memcpy(inode + INODE_SB_OFFSET, &pointer, sizeof(pointer));
    pointer = mapping;
    memcpy(inode + INODE_MAPPING_OFFSET, &pointer, sizeof(pointer));
    pointer = inode;
    memcpy(mapping + MAPPING_HOST_OFFSET, &pointer, sizeof(pointer));
    const uint16_t mode = 0100000u | 0640u;
    const uint32_t uid = 123;
    const uint32_t gid = 456;
    const uint32_t nlink = 2;
    const uint32_t rdev = 0x1234;
    const uint32_t device = 0x5678;
    const uint64_t ino = 99;
    const uint64_t size = 12345;
    const uint64_t blocks = 32;
    const uint8_t block_bits = 12;
    const uint64_t super_flags = SB_I_VERSION;
    const uint64_t version = 10;
    const int64_t times[] = {101, 202, 303};
    const uint32_t nsecs[] = {11, 22, 33};
    memcpy(inode + INODE_MODE_OFFSET, &mode, sizeof(mode));
    memcpy(inode + INODE_UID_OFFSET, &uid, sizeof(uid));
    memcpy(inode + INODE_GID_OFFSET, &gid, sizeof(gid));
    memcpy(inode + INODE_NLINK_OFFSET, &nlink, sizeof(nlink));
    memcpy(inode + INODE_RDEV_OFFSET, &rdev, sizeof(rdev));
    memcpy(inode + INODE_NUMBER_OFFSET, &ino, sizeof(ino));
    memcpy(inode + INODE_SIZE_OFFSET, &size, sizeof(size));
    memcpy(inode + INODE_BLOCKS_OFFSET, &blocks, sizeof(blocks));
    memcpy(inode + INODE_BLKBITS_OFFSET, &block_bits, sizeof(block_bits));
    memcpy(inode + INODE_VERSION_OFFSET, &version, sizeof(version));
    memcpy(inode + INODE_ATIME_SEC_OFFSET, &times[0], sizeof(times[0]));
    memcpy(inode + INODE_MTIME_SEC_OFFSET, &times[1], sizeof(times[1]));
    memcpy(inode + INODE_CTIME_SEC_OFFSET, &times[2], sizeof(times[2]));
    memcpy(inode + INODE_ATIME_NSEC_OFFSET, &nsecs[0], sizeof(nsecs[0]));
    memcpy(inode + INODE_MTIME_NSEC_OFFSET, &nsecs[1], sizeof(nsecs[1]));
    memcpy(inode + INODE_CTIME_NSEC_OFFSET, &nsecs[2], sizeof(nsecs[2]));
    memcpy(super_block + SUPER_DEVICE_OFFSET, &device, sizeof(device));
    memcpy(super_block + SUPER_FLAGS_OFFSET, &super_flags, sizeof(super_flags));

    kb_fs_subsystem_generic_fillattr(
        NULL,
        STATX_CHANGE_COOKIE,
        inode,
        stat);
    uint32_t observed32 = 0;
    uint64_t observed64 = 0;
#define CHECK_KSTAT32(offset, expected, error) do { \
    memcpy(&observed32, stat + (offset), sizeof(observed32)); \
    if (observed32 != (uint32_t)(expected)) return (error); \
} while (0)
#define CHECK_KSTAT64(offset, expected, error) do { \
    memcpy(&observed64, stat + (offset), sizeof(observed64)); \
    if (observed64 != (uint64_t)(expected)) return (error); \
} while (0)
    CHECK_KSTAT32(0x00, STATX_CHANGE_COOKIE, 248);
    CHECK_KSTAT32(0x04, mode, 249);
    CHECK_KSTAT32(0x08, nlink, 250);
    CHECK_KSTAT32(0x0c, 4096, 251);
    CHECK_KSTAT64(0x20, ino, 252);
    CHECK_KSTAT32(0x28, device, 253);
    CHECK_KSTAT32(0x2c, rdev, 254);
    CHECK_KSTAT32(0x30, uid, 255);
    CHECK_KSTAT32(0x34, gid, 256);
    CHECK_KSTAT64(0x38, size, 257);
    CHECK_KSTAT64(0x40, times[0], 258);
    CHECK_KSTAT64(0x48, nsecs[0], 259);
    CHECK_KSTAT64(0x50, times[1], 260);
    CHECK_KSTAT64(0x58, nsecs[1], 261);
    CHECK_KSTAT64(0x60, times[2], 262);
    CHECK_KSTAT64(0x68, nsecs[2], 263);
    CHECK_KSTAT64(0x80, blocks, 264);
    CHECK_KSTAT64(0x98, 5, 265);
#undef CHECK_KSTAT32
#undef CHECK_KSTAT64

    unsigned char file[512] = {0};
    uint64_t file_version = 77;
    memcpy(file + FILE_VERSION_OFFSET, &file_version, sizeof(file_version));
    if (kb_fs_subsystem_generic_file_llseek_size(file, 10, 0, 1000, 100) != 10 ||
        kb_fs_subsystem_generic_file_llseek_size(file, 5, 1, 1000, 100) != 15 ||
        kb_fs_subsystem_generic_file_llseek_size(file, -10, 2, 1000, 100) != 90 ||
        kb_fs_subsystem_generic_file_llseek_size(file, 5, 3, 1000, 100) != 5 ||
        kb_fs_subsystem_generic_file_llseek_size(file, 5, 4, 1000, 100) != 100 ||
        kb_fs_subsystem_generic_file_llseek_size(file, 100, 3, 1000, 100) != -6 ||
        kb_fs_subsystem_generic_file_llseek_size(file, 0, 99, 1000, 100) != -22)
    {
        return 266;
    }
    memcpy(&observed64, file + FILE_POSITION_OFFSET, sizeof(observed64));
    memcpy(&file_version, file + FILE_VERSION_OFFSET, sizeof(file_version));
    if (observed64 != 100 || file_version != 0) {
        return 267;
    }

    void *folio = kb_fs_subsystem_filemap_get_folio(mapping, 7, 0x4u, 0);
    if (folio == NULL || (uintptr_t)folio >= (uintptr_t)-4095) {
        return 268;
    }
    if (kb_fs_subsystem_generic_error_remove_folio(mapping, folio) != 0) {
        return 269;
    }
    unsigned char batch[8 + 31 * sizeof(void *)] = {0};
    unsigned long start = 0;
    if (kb_fs_subsystem_filemap_get_folios(
            mapping,
            &start,
            (unsigned long)-1,
            batch) != 0)
    {
        return 270;
    }
    kb_fs_subsystem_folio_put(folio);

    unsigned char lock_inode1[0x270] = {0};
    unsigned char lock_inode2[0x270] = {0};
    kb_fs_subsystem_inode_init_once(lock_inode1);
    kb_fs_subsystem_inode_init_once(lock_inode2);
    kb_fs_subsystem_lock_two_nondirectories(lock_inode2, lock_inode1);
    kb_fs_subsystem_unlock_two_nondirectories(lock_inode2, lock_inode1);
    kb_fs_subsystem_lock_two_nondirectories(lock_inode1, lock_inode1);
    kb_fs_subsystem_unlock_two_nondirectories(lock_inode1, lock_inode1);

    unsigned char fileattr[28];
    memset(fileattr, 0xa5, sizeof(fileattr));
    kb_fs_subsystem_fileattr_fill_flags(
        fileattr,
        0x00000008u | 0x00000010u | 0x00000020u |
            0x00000040u | 0x00000080u | 0x02000000u | 0x20000000u);
    uint32_t fileattr_flags = 0;
    uint32_t fileattr_xflags = 0;
    memcpy(&fileattr_flags, fileattr, sizeof(fileattr_flags));
    memcpy(&fileattr_xflags, fileattr + 4, sizeof(fileattr_xflags));
    if (fileattr_flags != 0x220000f8u || fileattr_xflags != 0x000082f8u ||
        fileattr[24] != 1 || fileattr[25] != 0 || fileattr[27] != 0)
    {
        return 271;
    }

    unsigned char fiemap_info[24] = {0};
    unsigned char extents[112];
    memset(extents, 0xa5, sizeof(extents));
    uint32_t fiemap_max = 2;
    memcpy(fiemap_info + 8, &fiemap_max, sizeof(fiemap_max));
    pointer = extents;
    memcpy(fiemap_info + 16, &pointer, sizeof(pointer));
    if (kb_fs_subsystem_fiemap_fill_next_extent(
            fiemap_info,
            4096,
            8192,
            12288,
            0x00000004u | 0x00000080u | 0x00000200u) != 0)
    {
        return 272;
    }
    uint32_t mapped = 0;
    uint32_t extent_flags = 0;
    memcpy(&mapped, fiemap_info + 4, sizeof(mapped));
    memcpy(&observed64, extents + 0, sizeof(observed64));
    memcpy(&extent_flags, extents + 40, sizeof(extent_flags));
    if (mapped != 1 || observed64 != 4096 ||
        extent_flags != 0x0000038eu)
    {
        return 273;
    }
    if (kb_fs_subsystem_fiemap_fill_next_extent(
            fiemap_info,
            16384,
            32768,
            4096,
            0x00000001u) != 1)
    {
        return 274;
    }
    memcpy(&mapped, fiemap_info + 4, sizeof(mapped));
    memcpy(&observed64, extents + 56, sizeof(observed64));
    if (mapped != 2 || observed64 != 16384) {
        return 275;
    }

    const int64_t maxbytes = 1024 * 1024;
    memcpy(super_block + 0x20, &maxbytes, sizeof(maxbytes));
    uint64_t fiemap_length = UINT64_MAX;
    uint32_t fiemap_flags = 1;
    memcpy(fiemap_info, &fiemap_flags, sizeof(fiemap_flags));
    if (kb_fs_subsystem_fiemap_prep(
            inode,
            fiemap_info,
            4096,
            &fiemap_length,
            0) != 0 ||
        fiemap_length != (uint64_t)maxbytes - 4096u)
    {
        return 276;
    }
    fiemap_flags = 4;
    fiemap_length = 4096;
    memcpy(fiemap_info, &fiemap_flags, sizeof(fiemap_flags));
    if (kb_fs_subsystem_fiemap_prep(
            inode,
            fiemap_info,
            0,
            &fiemap_length,
            0) != -53)
    {
        return 277;
    }

    memset(fiemap_info, 0, sizeof(fiemap_info));
    memset(extents, 0, sizeof(extents));
    fiemap_max = 2;
    memcpy(fiemap_info + 8, &fiemap_max, sizeof(fiemap_max));
    pointer = extents;
    memcpy(fiemap_info + 16, &pointer, sizeof(pointer));
    void *iomap_ops[2] = {
        (void *)(uintptr_t)&fiemap_iomap_smoke_begin,
        NULL,
    };
    if (kb_fs_subsystem_iomap_fiemap(
            inode,
            fiemap_info,
            0,
            8192,
            iomap_ops) != 0)
    {
        return 278;
    }
    memcpy(&mapped, fiemap_info + 4, sizeof(mapped));
    memcpy(&observed64, extents + 8, sizeof(observed64));
    memcpy(&extent_flags, extents + 40, sizeof(extent_flags));
    if (mapped != 1 || observed64 != 0x10000u ||
        extent_flags != 0x00003001u)
    {
        return 279;
    }

    unsigned char seek_inode[0x270] = {0};
    unsigned char seek_mapping[256] = {0};
    const int64_t seek_size = 12288;
    const uint8_t seek_block_bits = 12;
    pointer = seek_mapping;
    memcpy(seek_inode + INODE_MAPPING_OFFSET, &pointer, sizeof(pointer));
    pointer = seek_inode;
    memcpy(seek_mapping + MAPPING_HOST_OFFSET, &pointer, sizeof(pointer));
    memcpy(seek_inode + INODE_SIZE_OFFSET, &seek_size, sizeof(seek_size));
    memcpy(
        seek_inode + INODE_BLKBITS_OFFSET,
        &seek_block_bits,
        sizeof(seek_block_bits));
    void *seek_iomap_ops[2] = {
        (void *)(uintptr_t)&seek_iomap_smoke_begin,
        NULL,
    };
    if (kb_fs_subsystem_iomap_seek_data(
            seek_inode,
            0,
            seek_iomap_ops) != 4096 ||
        kb_fs_subsystem_iomap_seek_hole(
            seek_inode,
            0,
            seek_iomap_ops) != 0 ||
        kb_fs_subsystem_iomap_seek_hole(
            seek_inode,
            4096,
            seek_iomap_ops) != 8192 ||
        kb_fs_subsystem_iomap_seek_data(
            seek_inode,
            8192,
            seek_iomap_ops) != -6 ||
        kb_fs_subsystem_iomap_seek_data(
            seek_inode,
            seek_size,
            seek_iomap_ops) != -6)
    {
        return 280;
    }

    void *cached_data = kb_fs_subsystem_filemap_get_folio(
        seek_mapping,
        1,
        0x4u,
        0);
    if (cached_data == NULL || (uintptr_t)cached_data >= (uintptr_t)-4095) {
        return 281;
    }
    kb_fs_subsystem_folio_end_read(cached_data, 1);
    void *unwritten_iomap_ops[2] = {
        (void *)(uintptr_t)&unwritten_iomap_smoke_begin,
        NULL,
    };
    if (kb_fs_subsystem_iomap_seek_data(
            seek_inode,
            0,
            unwritten_iomap_ops) != 4096 ||
        kb_fs_subsystem_iomap_seek_hole(
            seek_inode,
            0,
            unwritten_iomap_ops) != 0 ||
        kb_fs_subsystem_iomap_seek_hole(
            seek_inode,
            4096,
            unwritten_iomap_ops) != 8192)
    {
        kb_fs_subsystem_truncate_inode_pages_range(seek_mapping, 0, -1);
        kb_fs_subsystem_folio_put(cached_data);
        return 282;
    }
    kb_fs_subsystem_truncate_inode_pages_range(seek_mapping, 0, -1);
    kb_fs_subsystem_folio_put(cached_data);
    return 0;
}

static int run_posix_acl_smoke(void)
{
    enum {
        SUPER_BYTES = 2048,
        INODE_OP_OFFSET = 0x20,
        INODE_MODE_OFFSET = 0x0,
        DENTRY_INODE_OFFSET = 0x30,
    };
    uint8_t super_block[SUPER_BYTES] = {0};
    uint8_t inode_operations[0xc0] = {0};
    void *get_operation = (void *)(uintptr_t)&acl_smoke_get;
    void *set_operation = (void *)(uintptr_t)&acl_smoke_set;
    memcpy(inode_operations + 0x18, &get_operation, sizeof(get_operation));
    memcpy(inode_operations + 0xa8, &set_operation, sizeof(set_operation));

    void *inode = kb_fs_subsystem_iget_locked(super_block, 71);
    if (inode == NULL) {
        return 143;
    }
    void *operation_pointer = inode_operations;
    uint16_t inode_mode = 0100644;
    memcpy((uint8_t *)inode + INODE_OP_OFFSET, &operation_pointer, sizeof(operation_pointer));
    memcpy((uint8_t *)inode + INODE_MODE_OFFSET, &inode_mode, sizeof(inode_mode));

    acl_get_calls = 0;
    void *first = kb_fs_subsystem_get_inode_acl(inode, 0x4000);
    void *second = kb_fs_subsystem_get_inode_acl(inode, 0x4000);
    if (first == NULL || second != first || acl_get_calls != 1) {
        kb_fs_subsystem_posix_acl_release(first);
        kb_fs_subsystem_posix_acl_release(second);
        kb_fs_subsystem_iput(inode);
        return 144;
    }
    kb_fs_subsystem_posix_acl_release(first);
    kb_fs_subsystem_posix_acl_release(second);

    uint16_t create_mode = 0100640;
    void *default_acl = NULL;
    void *access_acl = NULL;
    if (kb_fs_subsystem_posix_acl_create(
            inode,
            &create_mode,
            &default_acl,
            &access_acl) != 0 ||
        default_acl != NULL || access_acl == NULL || create_mode != 0100640)
    {
        kb_fs_subsystem_posix_acl_release(default_acl);
        kb_fs_subsystem_posix_acl_release(access_acl);
        kb_fs_subsystem_iput(inode);
        return 145;
    }

    kb_fs_subsystem_set_cached_acl(inode, 0x8000, access_acl);
    uint8_t dentry[512] = {0};
    memcpy(dentry + DENTRY_INODE_OFFSET, &inode, sizeof(inode));
    acl_set_calls = 0;
    acl_set_user_permission = UINT16_MAX;
    acl_set_mask_permission = UINT16_MAX;
    acl_set_other_permission = UINT16_MAX;
    if (kb_fs_subsystem_posix_acl_chmod(NULL, dentry, 0100600) != 0 ||
        acl_set_calls != 1 || acl_set_user_permission != 6 ||
        acl_set_mask_permission != 0 || acl_set_other_permission != 0)
    {
        kb_fs_subsystem_posix_acl_release(access_acl);
        kb_fs_subsystem_iput(inode);
        return 146;
    }

    uint8_t *equivalent_acl = kb_fs_subsystem_posix_acl_alloc(3, 0);
    if (equivalent_acl == NULL) {
        kb_fs_subsystem_posix_acl_release(access_acl);
        kb_fs_subsystem_iput(inode);
        return 147;
    }
    static const uint16_t equivalent_tags[3] = {0x01, 0x04, 0x20};
    static const uint16_t equivalent_permissions[3] = {6, 4, 0};
    for (size_t i = 0; i < 3; ++i) {
        memcpy(equivalent_acl + 28u + i * 8u, &equivalent_tags[i], sizeof(uint16_t));
        memcpy(equivalent_acl + 30u + i * 8u, &equivalent_permissions[i], sizeof(uint16_t));
    }
    void *updated_acl = equivalent_acl;
    uint16_t updated_mode = 0;
    if (kb_fs_subsystem_posix_acl_update_mode(
            NULL,
            inode,
            &updated_mode,
            &updated_acl) != 0 ||
        updated_acl != NULL || updated_mode != 0100640)
    {
        kb_fs_subsystem_posix_acl_release(equivalent_acl);
        kb_fs_subsystem_posix_acl_release(access_acl);
        kb_fs_subsystem_iput(inode);
        return 148;
    }
    kb_fs_subsystem_posix_acl_release(equivalent_acl);
    kb_fs_subsystem_posix_acl_release(access_acl);
    kb_fs_subsystem_iput(inode);
    return 0;
}

static int run_storage_limit_helpers_smoke(void)
{
    unsigned char super_block[2048] = {0};
    unsigned char inode[0x270] = {0};
    void *pointer = super_block;
    memcpy(inode + 0x28, &pointer, sizeof(pointer));
    uint64_t old_size = 100;
    int64_t max_bytes = 1000;
    memcpy(inode + 0x50, &old_size, sizeof(old_size));
    memcpy(super_block + 0x20, &max_bytes, sizeof(max_bytes));

    if (kb_fs_subsystem_inode_newsize_ok(inode, -1) != -22 ||
        kb_fs_subsystem_inode_newsize_ok(inode, 1001) != -27 ||
        kb_fs_subsystem_inode_newsize_ok(inode, 1000) != 0 ||
        kb_fs_subsystem_generic_check_addressable(8, 1) != -22 ||
        kb_fs_subsystem_generic_check_addressable(13, 1) != -22 ||
        kb_fs_subsystem_generic_check_addressable(12, UINT64_MAX) != -27 ||
        kb_fs_subsystem_generic_check_addressable(12, 1024) != 0 ||
        kb_fs_subsystem_set_blocksize(NULL, 4096) != -22)
    {
        return 210;
    }
    uint32_t inode_flags = 1u << 8;
    memcpy(inode + 0x0c, &inode_flags, sizeof(inode_flags));
    return kb_fs_subsystem_inode_newsize_ok(inode, 50) == -26 ? 0 : 211;
}

static int run_dentry_lifecycle_smoke(void)
{
    enum {
        INODE_BYTES = 0x270,
        DENTRY_BYTES = 512,
        INODE_MODE_OFFSET = 0x0,
        INODE_SB_OFFSET = 0x28,
        INODE_NUMBER_OFFSET = 0x40,
        INODE_NLINK_OFFSET = 0x48,
        DENTRY_FLAGS_OFFSET = 0x0,
        DENTRY_PARENT_OFFSET = 0x18,
        DENTRY_NAME_LENGTH_OFFSET = 0x24,
        DENTRY_NAME_POINTER_OFFSET = 0x28,
        DENTRY_INODE_OFFSET = 0x30,
        DENTRY_INLINE_NAME_OFFSET = 0x38,
        FILE_PATH_DENTRY_OFFSET = 0x48,
    };
    uint8_t parent[DENTRY_BYTES] = {0};
    for (size_t iteration = 0; iteration < 4096; ++iteration) {
        void *dentry = kb_fs_subsystem_d_alloc_name(parent, "native-name");
        if (dentry == NULL ||
            load_pointer_field(dentry, DENTRY_PARENT_OFFSET) != parent)
        {
            return 34;
        }
        uint32_t name_length = 0;
        memcpy(
            &name_length,
            (const uint8_t *)dentry + DENTRY_NAME_LENGTH_OFFSET,
            sizeof(name_length));
        const char *name = load_pointer_field(dentry, DENTRY_NAME_POINTER_OFFSET);
        if (name_length != strlen("native-name") ||
            name != (const char *)dentry + DENTRY_INLINE_NAME_OFFSET ||
            strcmp(name, "native-name") != 0)
        {
            kb_fs_subsystem_dput(dentry);
            return 35;
        }
        kb_fs_subsystem_d_instantiate(dentry, parent);
        if (load_pointer_field(dentry, DENTRY_INODE_OFFSET) != parent) {
            kb_fs_subsystem_dput(dentry);
            return 36;
        }
        kb_fs_subsystem_dput(dentry);
    }

    uint8_t super_block[2048] = {0};
    uint8_t root_inode[INODE_BYTES] = {0};
    uint8_t child_inode[INODE_BYTES] = {0};
    void *super_pointer = super_block;
    uint16_t directory_mode = 0040755;
    uint16_t regular_mode = 0100644;
    memcpy(root_inode + INODE_MODE_OFFSET, &directory_mode, sizeof(directory_mode));
    memcpy(root_inode + INODE_SB_OFFSET, &super_pointer, sizeof(super_pointer));
    memcpy(child_inode + INODE_MODE_OFFSET, &regular_mode, sizeof(regular_mode));
    memcpy(child_inode + INODE_SB_OFFSET, &super_pointer, sizeof(super_pointer));

    void *root = kb_fs_subsystem_d_make_root(root_inode);
    const char counted_name[3] = {'a', 'b', 'c'};
    struct {
        uint32_t hash;
        uint32_t length;
        const char *name;
    } qstr = {
        .hash = 0x1234u,
        .length = sizeof(counted_name),
        .name = counted_name,
    };
    void *child = kb_fs_subsystem_d_alloc(root, &qstr);
    if (root == NULL || child == NULL ||
        load_pointer_field(child, DENTRY_PARENT_OFFSET) != root ||
        strcmp(load_pointer_field(child, DENTRY_NAME_POINTER_OFFSET), "abc") != 0)
    {
        kb_fs_subsystem_dput(child);
        kb_fs_subsystem_dput(root);
        return 138;
    }
    kb_fs_subsystem_d_instantiate(child, child_inode);
    void *alias = kb_fs_subsystem_d_find_any_alias(child_inode);
    void *held_parent = kb_fs_subsystem_dget_parent(child);
    if (alias != child || held_parent != root) {
        kb_fs_subsystem_dput(alias);
        kb_fs_subsystem_dput(held_parent);
        kb_fs_subsystem_dput(child);
        kb_fs_subsystem_dput(root);
        return 139;
    }
    kb_fs_subsystem_dput(alias);
    kb_fs_subsystem_dput(held_parent);
    kb_fs_subsystem_d_mark_dontcache(child);
    uint32_t dentry_flags = 0;
    memcpy(
        &dentry_flags,
        (const uint8_t *)child + DENTRY_FLAGS_OFFSET,
        sizeof(dentry_flags));
    if ((dentry_flags & (1u << 7)) == 0) {
        kb_fs_subsystem_dput(child);
        kb_fs_subsystem_dput(root);
        return 140;
    }
    kb_fs_subsystem_d_drop(child);

    void *obtained = kb_fs_subsystem_d_obtain_alias(child_inode);
    if (obtained != child ||
        (intptr_t)kb_fs_subsystem_d_obtain_alias(NULL) != -116)
    {
        kb_fs_subsystem_dput(obtained);
        kb_fs_subsystem_dput(child);
        kb_fs_subsystem_dput(root);
        return 141;
    }
    kb_fs_subsystem_dput(obtained);

    void *temporary = kb_fs_subsystem_d_alloc_name(root, "");
    uint8_t file[0x100] = {0};
    void *temporary_pointer = temporary;
    uint64_t inode_number = 42;
    uint32_t nlink = 1;
    memcpy(file + FILE_PATH_DENTRY_OFFSET, &temporary_pointer, sizeof(temporary_pointer));
    memcpy(child_inode + INODE_NUMBER_OFFSET, &inode_number, sizeof(inode_number));
    memcpy(child_inode + INODE_NLINK_OFFSET, &nlink, sizeof(nlink));
    kb_fs_subsystem_d_tmpfile(file, child_inode);
    memcpy(&nlink, child_inode + INODE_NLINK_OFFSET, sizeof(nlink));
    if (temporary == NULL || nlink != 0 ||
        load_pointer_field(temporary, DENTRY_INODE_OFFSET) != child_inode ||
        strcmp(load_pointer_field(temporary, DENTRY_NAME_POINTER_OFFSET), "#42") != 0)
    {
        kb_fs_subsystem_dput(temporary);
        kb_fs_subsystem_dput(child);
        kb_fs_subsystem_dput(root);
        return 142;
    }
    kb_fs_subsystem_dput(temporary);
    kb_fs_subsystem_dput(child);
    kb_fs_subsystem_dput(root);
    return 0;
}

static int run_devpts_dentry_layout_smoke(void)
{
    enum {
        INODE_BYTES = 0x270,
        SUPER_BYTES = 2048,
        INODE_SB_OFFSET = 0x28,
        SUPER_MAGIC_OFFSET = 0x60,
        LINUX_612_DENTRY_SB_OFFSET = 0x68,
        LINUX_68_DENTRY_SB_OFFSET = 0x70,
        DEVPTS_SUPER_MAGIC = 0x1cd1,
    };
    uint8_t super_block[SUPER_BYTES] = {0};
    uint8_t root_inode[INODE_BYTES] = {0};
    uint8_t child_inode[INODE_BYTES] = {0};
    void *super_pointer = super_block;
    uint64_t magic = DEVPTS_SUPER_MAGIC;
    memcpy(super_block + SUPER_MAGIC_OFFSET, &magic, sizeof(magic));
    memcpy(root_inode + INODE_SB_OFFSET, &super_pointer, sizeof(super_pointer));
    memcpy(child_inode + INODE_SB_OFFSET, &super_pointer, sizeof(super_pointer));

    void *root = kb_fs_subsystem_d_make_root(root_inode);
    if (root == NULL ||
        load_pointer_field(root, LINUX_612_DENTRY_SB_OFFSET) != NULL ||
        load_pointer_field(root, LINUX_68_DENTRY_SB_OFFSET) != super_block)
    {
        kb_fs_subsystem_dput(root);
        return 37;
    }

    void *child = kb_fs_subsystem_d_alloc_name(root, "0");
    if (child == NULL ||
        load_pointer_field(child, LINUX_612_DENTRY_SB_OFFSET) != NULL ||
        load_pointer_field(child, LINUX_68_DENTRY_SB_OFFSET) != super_block)
    {
        kb_fs_subsystem_dput(child);
        kb_fs_subsystem_dput(root);
        return 38;
    }
    kb_fs_subsystem_d_instantiate(child, child_inode);
    if (load_pointer_field(child, LINUX_612_DENTRY_SB_OFFSET) != NULL ||
        load_pointer_field(child, LINUX_68_DENTRY_SB_OFFSET) != super_block)
    {
        kb_fs_subsystem_dput(child);
        kb_fs_subsystem_dput(root);
        return 39;
    }
    kb_fs_subsystem_dput(child);
    kb_fs_subsystem_dput(root);
    return 0;
}

static kb_status_t fs_ipc_dispatch(void *ctx, const void *message, size_t message_size)
{
    fs_ipc_smoke_t *smoke = (fs_ipc_smoke_t *)ctx;
    kb_fs_ipc_request_t *request = (kb_fs_ipc_request_t *)message;
    if (smoke == NULL ||
        request == NULL ||
        message_size != sizeof(*request) ||
        request->version != 1)
    {
        return KB_ERR_INVALID;
    }

    switch (request->operation) {
    case KB_FS_OPERATION_MOUNT:
        if (strcmp(request->source, "/dev/kobox0") != 0 ||
            strcmp(request->path, "/mnt/kobox") != 0 ||
            strcmp(request->fs_type, "ext4") != 0)
        {
            return KB_ERR_INVALID;
        }
        smoke->mount_seen++;
        request->handled = 1;
        request->handle = 7;
        request->result_code = 0;
        return KB_OK;
    case KB_FS_OPERATION_READ:
        if (request->handle != 7 || request->output == NULL || request->output_capacity < 5) {
            return KB_ERR_INVALID;
        }
        if (request->path != NULL && strcmp(request->path, "/mnt/kobox/empty") == 0) {
            request->handled = 1;
            request->output_size = 0;
            request->result_code = 0;
            smoke->read_seen++;
            return KB_OK;
        }
        memcpy(request->output, "hello", 5);
        request->handled = 1;
        request->output_size = 5;
        request->result_code = 0;
        smoke->read_seen++;
        return KB_OK;
    case KB_FS_OPERATION_WRITE:
        if (request->handle != 7 ||
            request->input == NULL ||
            request->input_size != 5 ||
            memcmp(request->input, "world", 5) != 0)
        {
            return KB_ERR_INVALID;
        }
        request->handled = 1;
        request->output_size = request->input_size;
        request->result_code = 0;
        smoke->write_seen++;
        return KB_OK;
    case KB_FS_OPERATION_READDIR:
        if (request->handle != 7 || request->output == NULL || request->output_capacity < 8) {
            return KB_ERR_INVALID;
        }
        memcpy(request->output, "a\nb\n", 4);
        request->handled = 1;
        request->output_size = 4;
        request->result_code = 0;
        smoke->readdir_seen++;
        return KB_OK;
    default:
        return KB_ERR_INVALID;
    }
}

typedef struct fake_file_system_type {
    const char *name;
} fake_file_system_type_t;

static int run_local_fs_model_smoke(void)
{
    fake_file_system_type_t ext4_type = {
        .name = "ext4",
    };
    if (kb_fs_subsystem_register_filesystem(&ext4_type) != 0 ||
        kb_fs_subsystem_registered_type_count() == 0)
    {
        return 20;
    }

    kb_fs_type_snapshot_t type_snapshot;
    if (kb_fs_subsystem_type_snapshot("ext4", &type_snapshot) != 0 ||
        type_snapshot.fs_type != &ext4_type ||
        strcmp(type_snapshot.name, "ext4") != 0 ||
        type_snapshot.register_count != 1)
    {
        return 21;
    }

    kb_device_backend_t *backend = NULL;
    if (kb_linux_mock_device_create(&backend) != KB_OK || backend == NULL) {
        return 22;
    }

    kb_interface_t *fs_interface = NULL;
    kb_linux_interface_desc_t fs_interface_desc = {
        .name = "linux-ipc-fs-local",
        .subsystem = "fs",
        .endpoint = "kobox.fs",
    };
    if (kb_linux_ipc_interface_create(&fs_interface_desc, &fs_interface) != KB_OK || fs_interface == NULL) {
        kb_device_backend_destroy(backend);
        return 23;
    }

    kb_interface_t *interfaces[] = {
        fs_interface,
    };
    kb_platform_desc_t platform_desc = {
        "linux-fs-local-platform",
        backend,
        interfaces,
        1,
    };
    kb_platform_t *platform = NULL;
    if (kb_platform_create(&platform_desc, &platform) != KB_OK || platform == NULL) {
        kb_interface_destroy(fs_interface);
        kb_device_backend_destroy(backend);
        return 24;
    }

    kb_interface_t *bound = NULL;
    if (kb_fs_subsystem_bind_ipc_interface(platform, &bound) != KB_OK || bound != fs_interface) {
        kb_platform_destroy(platform);
        return 25;
    }

    uint64_t handle = 0;
    void *block_disk = (void *)(uintptr_t)0x55;
    kb_fs_mount_desc_t mount_desc = {
        .source = "/dev/kobox-ext4",
        .target = "/mnt/ext4",
        .fs_type = "ext4",
        .block_disk = block_disk,
    };
    if (kb_fs_subsystem_mount(bound, &mount_desc, &handle) != KB_OK || handle == 0) {
        kb_platform_destroy(platform);
        return 26;
    }

    kb_fs_mount_snapshot_t mount_snapshot;
    if (kb_fs_subsystem_mount_snapshot(handle, &mount_snapshot) != 0 ||
        strcmp(mount_snapshot.fs_type, "ext4") != 0 ||
        strcmp(mount_snapshot.target, "/mnt/ext4") != 0 ||
        mount_snapshot.block_disk != block_disk)
    {
        kb_platform_destroy(platform);
        return 27;
    }

    size_t written = 0;
    kb_fs_write_desc_t write_desc = {
        .handle = handle,
        .path = "/mnt/ext4/hello.txt",
        .offset = 0,
        .buffer = "real-ext4-path",
        .byte_count = 14,
        .out_bytes = &written,
    };
    if (kb_fs_subsystem_write(bound, &write_desc) != KB_OK || written != 14) {
        kb_platform_destroy(platform);
        return 28;
    }

    char read_buffer[32] = {0};
    size_t read = 0;
    kb_fs_read_desc_t read_desc = {
        .handle = handle,
        .path = "/mnt/ext4/hello.txt",
        .offset = 5,
        .buffer = read_buffer,
        .byte_count = sizeof(read_buffer),
        .out_bytes = &read,
    };
    if (kb_fs_subsystem_read(bound, &read_desc) != KB_OK ||
        read != 9 ||
        memcmp(read_buffer, "ext4-path", 9) != 0)
    {
        kb_platform_destroy(platform);
        return 29;
    }

    char dir_buffer[128] = {0};
    size_t dir_bytes = 0;
    kb_fs_readdir_desc_t readdir_desc = {
        .handle = handle,
        .path = "/mnt/ext4",
        .buffer = dir_buffer,
        .byte_count = sizeof(dir_buffer),
        .out_bytes = &dir_bytes,
    };
    if (kb_fs_subsystem_readdir(bound, &readdir_desc) != KB_OK ||
        dir_bytes == 0 ||
        strstr(dir_buffer, "/mnt/ext4/hello.txt") == NULL)
    {
        kb_platform_destroy(platform);
        return 30;
    }

    kb_interface_unbind(bound);
    kb_platform_destroy(platform);
    if (kb_fs_subsystem_unregister_filesystem(&ext4_type) != 0) {
        return 31;
    }
    return 0;
}

typedef int (*fs_parser_smoke_type_fn_t)(void *, const void *, void *, void *);

typedef struct fs_parser_smoke_spec {
    const char *name;
    fs_parser_smoke_type_fn_t type;
    uint8_t option;
    uint8_t reserved0;
    uint16_t flags;
    uint32_t reserved1;
    const void *data;
} fs_parser_smoke_spec_t;

typedef struct fs_parser_smoke_parameter {
    const char *key;
    uint8_t type;
    uint8_t reserved0[7];
    union {
        char *string;
        void *pointer;
    } value;
    size_t size;
    int dirfd;
    uint32_t reserved1;
} fs_parser_smoke_parameter_t;

typedef struct fs_parser_smoke_result {
    uint8_t negated;
    uint8_t reserved[7];
    union {
        uint8_t boolean;
        int32_t signed_32;
        uint32_t unsigned_32;
        uint64_t unsigned_64;
    } value;
} fs_parser_smoke_result_t;

typedef struct fs_parser_smoke_constant {
    const char *name;
    int value;
    uint32_t reserved;
} fs_parser_smoke_constant_t;

static int run_fs_parser_and_statfs_smoke(void)
{
    static const fs_parser_smoke_constant_t errors[] = {
        {"continue", 1, 0},
        {"remount-ro", 3, 0},
        {NULL, 0, 0},
    };
    static const fs_parser_smoke_spec_t specs[] = {
        {"acl", NULL, 11, 0, 0, 0, NULL},
        {"barrier", NULL, 12, 0, 0x0002, 0, NULL},
        {"commit", kb_fs_subsystem_fs_param_is_u32, 13, 0, 0, 0, NULL},
        {"errors", kb_fs_subsystem_fs_param_is_enum, 14, 0, 0, 0, errors},
        {"scan", kb_fs_subsystem_fs_param_is_s32, 15, 0, 0, 0, NULL},
        {"quota", kb_fs_subsystem_fs_param_is_string, 16, 0, 0, 0, NULL},
        {NULL, NULL, 0, 0, 0, 0, NULL},
    };
    fs_parser_smoke_parameter_t parameter = {
        .key = "commit",
        .type = 2,
        .value.string = "15",
        .size = 2,
        .dirfd = -100,
    };
    fs_parser_smoke_result_t result;
    if (kb_fs_subsystem_fs_parse(NULL, specs, &parameter, &result) != 13 ||
        result.negated != 0 || result.value.unsigned_32 != 15)
    {
        return 90;
    }
    parameter.value.string = "4294967296";
    if (kb_fs_subsystem_fs_parse(NULL, specs, &parameter, &result) != -22) {
        return 91;
    }
    parameter.key = "acl";
    parameter.type = 1;
    parameter.value.string = NULL;
    if (kb_fs_subsystem_fs_parse(NULL, specs, &parameter, &result) != 11 ||
        result.negated != 0 || result.value.boolean != 1)
    {
        return 92;
    }
    parameter.key = "nobarrier";
    if (kb_fs_subsystem_fs_parse(NULL, specs, &parameter, &result) != 12 ||
        result.negated != 1 || result.value.boolean != 0)
    {
        return 93;
    }
    parameter.key = "errors";
    parameter.type = 2;
    parameter.value.string = "remount-ro";
    if (kb_fs_subsystem_fs_parse(NULL, specs, &parameter, &result) != 14 ||
        result.value.unsigned_32 != 3)
    {
        return 94;
    }
    parameter.value.string = "silently-ignore";
    if (kb_fs_subsystem_fs_parse(NULL, specs, &parameter, &result) != -22) {
        return 95;
    }
    parameter.key = "unknown";
    if (kb_fs_subsystem_fs_parse(NULL, specs, &parameter, &result) != -519) {
        return 96;
    }
    parameter.key = "quota";
    parameter.value.string = "";
    if (kb_fs_subsystem_fs_parse(NULL, specs, &parameter, &result) != -22) {
        return 97;
    }
    void *path[2] = {(void *)(uintptr_t)1, (void *)(uintptr_t)2};
    parameter.key = "journal_path";
    parameter.value.string = "/dev/external-journal";
    if (kb_fs_subsystem_fs_lookup_param(NULL, &parameter, 1, 1, path) != -95 ||
        path[0] != NULL || path[1] != NULL)
    {
        return 98;
    }

    uint8_t super_block[128] = {0};
    uint8_t dentry[128] = {0};
    uint8_t statfs[128];
    memset(statfs, 0xa5, sizeof(statfs));
    const uint32_t device = (5u << 20) | 0x345u;
    const uint64_t magic = 0xef53u;
    void *super_pointer = super_block;
    memcpy(super_block + 0x10, &device, sizeof(device));
    memcpy(super_block + 0x60, &magic, sizeof(magic));
    memcpy(dentry + 0x68, &super_pointer, sizeof(super_pointer));
    if (kb_fs_subsystem_simple_statfs(dentry, statfs) != 0) {
        return 99;
    }
    uint64_t value = 0;
    memcpy(&value, statfs, sizeof(value));
    if (value != magic) {
        return 100;
    }
    memcpy(&value, statfs + 8, sizeof(value));
    if (value != 4096u) {
        return 101;
    }
    uint32_t fsid = 0;
    memcpy(&fsid, statfs + 56, sizeof(fsid));
    if (fsid != ((0x345u & 0xffu) | (5u << 8) |
            ((0x345u & ~0xffu) << 12)))
    {
        return 102;
    }
    memcpy(&value, statfs + 64, sizeof(value));
    if (value != 255u) {
        return 103;
    }
    return 0;
}

static int run_filemap_fault_and_mkwrite_smoke(void)
{
    enum {
        INODE_MAPPING_OFFSET = 0x30,
        INODE_SIZE_OFFSET = 0x50,
        INODE_BLKBITS_OFFSET = 0x86,
        MAPPING_HOST_OFFSET = 0x0,
        MAPPING_AOPS_OFFSET = 0x68,
        AOPS_READ_FOLIO_OFFSET = 0x08,
        FILE_MAPPING_OFFSET = 0x18,
        FILE_INODE_OFFSET = 0x28,
        VMA_FILE_OFFSET = 0x80,
        VMF_PGOFF_OFFSET = 0x10,
        VMF_PAGE_OFFSET = 0x50,
    };
    unsigned char inode[0x270] = {0};
    unsigned char mapping[0x100] = {0};
    unsigned char address_space_operations[0x80] = {0};
    unsigned char file[0xc0] = {0};
    unsigned char vma[0xc0] = {0};
    unsigned char vm_fault[0x70] = {0};
    void *pointer = mapping;
    memcpy(inode + INODE_MAPPING_OFFSET, &pointer, sizeof(pointer));
    int64_t size = 8192;
    memcpy(inode + INODE_SIZE_OFFSET, &size, sizeof(size));
    inode[INODE_BLKBITS_OFFSET] = 12;
    pointer = inode;
    memcpy(mapping + MAPPING_HOST_OFFSET, &pointer, sizeof(pointer));
    pointer = address_space_operations;
    memcpy(mapping + MAPPING_AOPS_OFFSET, &pointer, sizeof(pointer));
    pointer = (void *)(uintptr_t)&mmap_smoke_read_folio;
    memcpy(address_space_operations + AOPS_READ_FOLIO_OFFSET,
        &pointer,
        sizeof(pointer));
    pointer = mapping;
    memcpy(file + FILE_MAPPING_OFFSET, &pointer, sizeof(pointer));
    pointer = inode;
    memcpy(file + FILE_INODE_OFFSET, &pointer, sizeof(pointer));
    pointer = file;
    memcpy(vma + VMA_FILE_OFFSET, &pointer, sizeof(pointer));
    pointer = vma;
    memcpy(vm_fault, &pointer, sizeof(pointer));

    unsigned int fault = kb_fs_subsystem_filemap_fault(vm_fault);
    if (fault != (0x4u | 0x200u)) {
        kb_fs_subsystem_truncate_inode_pages_final(mapping);
        return 104;
    }
    void *folio = NULL;
    memcpy(&folio, vm_fault + VMF_PAGE_OFFSET, sizeof(folio));
    unsigned char *payload = kb_linux_kvm_page_payload(folio, 0, 4096);
    if (folio == NULL || payload == NULL || payload[0] != 0x6d ||
        payload[4095] != 0x6d)
    {
        kb_fs_subsystem_truncate_inode_pages_final(mapping);
        return 105;
    }
    kb_fs_subsystem_folio_unlock(folio);
    kb_fs_subsystem_folio_put(folio);

    memset(vm_fault + VMF_PAGE_OFFSET, 0, sizeof(void *));
    fault = kb_fs_subsystem_filemap_fault(vm_fault);
    memcpy(&folio, vm_fault + VMF_PAGE_OFFSET, sizeof(folio));
    if (fault != 0x200u || folio == NULL) {
        kb_fs_subsystem_truncate_inode_pages_final(mapping);
        return 106;
    }
    kb_fs_subsystem_folio_unlock(folio);
    if (kb_fs_subsystem_block_page_mkwrite(
            vma,
            vm_fault,
            mmap_smoke_get_block) != 0)
    {
        kb_fs_subsystem_folio_put(folio);
        kb_fs_subsystem_truncate_inode_pages_final(mapping);
        return 107;
    }
    uint64_t folio_flags = 0;
    memcpy(&folio_flags, folio, sizeof(folio_flags));
    if ((folio_flags & (0x1u | 0x10u)) != (0x1u | 0x10u)) {
        kb_fs_subsystem_folio_unlock(folio);
        kb_fs_subsystem_folio_put(folio);
        kb_fs_subsystem_truncate_inode_pages_final(mapping);
        return 108;
    }
    kb_fs_subsystem_folio_unlock(folio);
    kb_fs_subsystem_folio_put(folio);

    uint64_t out_of_range = 2;
    memcpy(vm_fault + VMF_PGOFF_OFFSET,
        &out_of_range,
        sizeof(out_of_range));
    memset(vm_fault + VMF_PAGE_OFFSET, 0, sizeof(void *));
    if (kb_fs_subsystem_filemap_fault(vm_fault) != 0x2u ||
        kb_fs_subsystem_filemap_map_pages(vm_fault, 2, 1) != 0 ||
        kb_fs_subsystem_filemap_map_pages(vm_fault, 0, 1) != 0)
    {
        kb_fs_subsystem_truncate_inode_pages_final(mapping);
        return 109;
    }
    kb_fs_subsystem_truncate_inode_pages_final(mapping);
    return 0;
}

static void *exportfs_smoke_inode;
static void *exportfs_smoke_parent;

static void *exportfs_smoke_get_inode(
    void *super_block,
    uint64_t inode_number,
    uint32_t generation)
{
    enum {
        INODE_SB_OFFSET = 0x28,
        INODE_NUMBER_OFFSET = 0x40,
        INODE_GENERATION_OFFSET = 0x248,
    };
    void *candidates[] = {
        exportfs_smoke_inode,
        exportfs_smoke_parent,
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        void *inode = candidates[i];
        uint64_t number = 0;
        uint32_t current_generation = 0;
        void *current_super = NULL;
        if (inode == NULL) {
            continue;
        }
        memcpy(&current_super,
            (const unsigned char *)inode + INODE_SB_OFFSET,
            sizeof(current_super));
        memcpy(&number,
            (const unsigned char *)inode + INODE_NUMBER_OFFSET,
            sizeof(number));
        memcpy(&current_generation,
            (const unsigned char *)inode + INODE_GENERATION_OFFSET,
            sizeof(current_generation));
        if (current_super == super_block && number == inode_number &&
            (generation == 0 || generation == current_generation))
        {
            return kb_fs_subsystem_igrab(inode);
        }
    }
    return (void *)(intptr_t)-116;
}

static int run_exportfs_helpers_smoke(void)
{
    enum {
        DENTRY_INODE_OFFSET = 0x30,
        INODE_GENERATION_OFFSET = 0x248,
    };
    unsigned char super_block[2048] = {0};
    exportfs_smoke_inode = kb_fs_subsystem_iget_locked(
        super_block,
        0x10203040u);
    exportfs_smoke_parent = kb_fs_subsystem_iget_locked(
        super_block,
        0x50607080u);
    if (exportfs_smoke_inode == NULL || exportfs_smoke_parent == NULL) {
        return 110;
    }
    const uint32_t inode_generation = 0xa1b2c3d4u;
    const uint32_t parent_generation = 0x11223344u;
    memcpy((unsigned char *)exportfs_smoke_inode + INODE_GENERATION_OFFSET,
        &inode_generation,
        sizeof(inode_generation));
    memcpy((unsigned char *)exportfs_smoke_parent + INODE_GENERATION_OFFSET,
        &parent_generation,
        sizeof(parent_generation));
    if (kb_fs_subsystem_find_inode_by_ino_rcu(
            super_block,
            0x10203040u) != exportfs_smoke_inode ||
        kb_fs_subsystem_find_inode_by_ino_rcu(
            super_block,
            0xabcdefu) != NULL)
    {
        return 111;
    }

    uint32_t handle[4] = {0};
    int handle_length = 1;
    if (kb_fs_subsystem_generic_encode_ino32_fh(
            exportfs_smoke_inode,
            handle,
            &handle_length,
            NULL) != 0xff || handle_length != 2)
    {
        return 112;
    }
    handle_length = 3;
    if (kb_fs_subsystem_generic_encode_ino32_fh(
            exportfs_smoke_inode,
            handle,
            &handle_length,
            exportfs_smoke_parent) != 0xff || handle_length != 4)
    {
        return 113;
    }
    handle_length = 4;
    if (kb_fs_subsystem_generic_encode_ino32_fh(
            exportfs_smoke_inode,
            handle,
            &handle_length,
            exportfs_smoke_parent) != 2 || handle_length != 4 ||
        handle[0] != 0x10203040u || handle[1] != inode_generation ||
        handle[2] != 0x50607080u || handle[3] != parent_generation)
    {
        return 114;
    }

    void *dentry = kb_fs_subsystem_generic_fh_to_dentry(
        super_block,
        handle,
        handle_length,
        2,
        (void *)(uintptr_t)&exportfs_smoke_get_inode);
    void *decoded_inode = NULL;
    if ((intptr_t)dentry < 0 || dentry == NULL) {
        return 115;
    }
    memcpy(&decoded_inode,
        (const unsigned char *)dentry + DENTRY_INODE_OFFSET,
        sizeof(decoded_inode));
    if (decoded_inode != exportfs_smoke_inode) {
        return 115;
    }
    kb_fs_subsystem_dput(dentry);

    dentry = kb_fs_subsystem_generic_fh_to_parent(
        super_block,
        handle,
        handle_length,
        2,
        (void *)(uintptr_t)&exportfs_smoke_get_inode);
    decoded_inode = NULL;
    if ((intptr_t)dentry < 0 || dentry == NULL) {
        return 116;
    }
    memcpy(&decoded_inode,
        (const unsigned char *)dentry + DENTRY_INODE_OFFSET,
        sizeof(decoded_inode));
    if (decoded_inode != exportfs_smoke_parent) {
        return 116;
    }
    kb_fs_subsystem_dput(dentry);

    if (kb_fs_subsystem_generic_fh_to_dentry(
            super_block,
            handle,
            1,
            2,
            (void *)(uintptr_t)&exportfs_smoke_get_inode) != NULL ||
        (intptr_t)kb_fs_subsystem_generic_fh_to_dentry(
            super_block,
            handle,
            4,
            99,
            (void *)(uintptr_t)&exportfs_smoke_get_inode) != -116 ||
        kb_fs_subsystem_generic_read_dir(NULL, NULL, 0, NULL) != -21)
    {
        return 117;
    }
    kb_fs_subsystem_iput(exportfs_smoke_inode);
    kb_fs_subsystem_iput(exportfs_smoke_parent);
    exportfs_smoke_inode = NULL;
    exportfs_smoke_parent = NULL;
    return 0;
}

static unsigned int finish_open_smoke_calls;
static unsigned int finish_close_smoke_calls;

static int finish_open_smoke_callback(void *inode, void *file)
{
    if (inode == NULL || file == NULL) {
        return -22;
    }
    finish_open_smoke_calls++;
    return 0;
}

static int finish_open_smoke_failure(void *inode, void *file)
{
    (void)inode;
    (void)file;
    return -5;
}

static int finish_close_smoke_callback(void *inode, void *file)
{
    if (inode == NULL || file == NULL) {
        return -22;
    }
    finish_close_smoke_calls++;
    return 0;
}

static int run_finish_open_smoke(void)
{
    enum {
        INODE_MODE_OFFSET = 0x00,
        INODE_MAPPING_OFFSET = 0x30,
        INODE_FILE_OP_OFFSET = 0x160,
        INODE_WRITECOUNT_OFFSET = 0x158,
        DENTRY_INODE_OFFSET = 0x30,
        MAPPING_AOPS_OFFSET = 0x68,
        FILE_MODE_OFFSET = 0x0c,
        FILE_OP_OFFSET = 0x10,
        FILE_MAPPING_OFFSET = 0x18,
        FILE_INODE_OFFSET = 0x28,
        FILE_PATH_DENTRY_OFFSET = 0x48,
        FILE_OP_LLSEEK_OFFSET = 0x10,
        FILE_OP_READ_ITER_OFFSET = 0x28,
        FILE_OP_WRITE_ITER_OFFSET = 0x30,
        FILE_OP_OPEN_OFFSET = 0x68,
        FILE_OP_RELEASE_OFFSET = 0x78,
        FILE_RA_OFFSET = 0x98,
        FILE_RA_PAGES_OFFSET = 0x10,
        FILE_RA_PREV_POS_OFFSET = 0x18,
        FMODE_READ = 0x1,
        FMODE_WRITE = 0x2,
        FMODE_OPENED = 0x80000,
        FMODE_CAN_READ = 0x20000,
        FMODE_CAN_WRITE = 0x40000,
        FMODE_WRITER = 0x10000,
        FMODE_ATOMIC_POS = 0x8000,
    };
    unsigned char inode[0x270] = {0};
    unsigned char mapping[0x100] = {0};
    unsigned char address_space_operations[0x80] = {0};
    unsigned char file_operations[0x100] = {0};
    unsigned char dentry[0x100] = {0};
    unsigned char file[0x100] = {0};
    void *pointer = mapping;
    memcpy(inode + INODE_MAPPING_OFFSET, &pointer, sizeof(pointer));
    pointer = file_operations;
    memcpy(inode + INODE_FILE_OP_OFFSET, &pointer, sizeof(pointer));
    const uint16_t mode = 0100644;
    memcpy(inode + INODE_MODE_OFFSET, &mode, sizeof(mode));
    pointer = inode;
    memcpy(dentry + DENTRY_INODE_OFFSET, &pointer, sizeof(pointer));
    pointer = address_space_operations;
    memcpy(mapping + MAPPING_AOPS_OFFSET, &pointer, sizeof(pointer));
    pointer = (void *)(uintptr_t)&finish_open_smoke_callback;
    memcpy(file_operations + FILE_OP_OPEN_OFFSET, &pointer, sizeof(pointer));
    memcpy(file_operations + FILE_OP_READ_ITER_OFFSET, &pointer, sizeof(pointer));
    memcpy(file_operations + FILE_OP_WRITE_ITER_OFFSET, &pointer, sizeof(pointer));
    memcpy(file_operations + FILE_OP_LLSEEK_OFFSET, &pointer, sizeof(pointer));
    memcpy(address_space_operations + 0x58, &pointer, sizeof(pointer));
    uint32_t file_mode = FMODE_READ | FMODE_WRITE;
    memcpy(file + FILE_MODE_OFFSET, &file_mode, sizeof(file_mode));
    finish_open_smoke_calls = 0;
    if (kb_fs_subsystem_finish_open(file, dentry, NULL) != 0 ||
        finish_open_smoke_calls != 1)
    {
        return 118;
    }
    void *observed = NULL;
    memcpy(&observed, file + FILE_PATH_DENTRY_OFFSET, sizeof(observed));
    if (observed != dentry) {
        return 119;
    }
    memcpy(&observed, file + FILE_INODE_OFFSET, sizeof(observed));
    if (observed != inode) {
        return 119;
    }
    memcpy(&observed, file + FILE_MAPPING_OFFSET, sizeof(observed));
    if (observed != mapping) {
        return 119;
    }
    memcpy(&observed, file + FILE_OP_OFFSET, sizeof(observed));
    if (observed != file_operations) {
        return 119;
    }
    memcpy(&file_mode, file + FILE_MODE_OFFSET, sizeof(file_mode));
    if ((file_mode & (FMODE_OPENED | FMODE_CAN_READ | FMODE_CAN_WRITE |
            FMODE_WRITER | FMODE_ATOMIC_POS)) !=
        (FMODE_OPENED | FMODE_CAN_READ | FMODE_CAN_WRITE |
            FMODE_WRITER | FMODE_ATOMIC_POS))
    {
        return 120;
    }
    uint32_t writecount = 0;
    memcpy(&writecount, inode + INODE_WRITECOUNT_OFFSET, sizeof(writecount));
    if (writecount != 1 ||
        kb_fs_subsystem_finish_open(file, dentry, NULL) != -16)
    {
        return 121;
    }

    memset(file, 0, sizeof(file));
    writecount = 0;
    memcpy(inode + INODE_WRITECOUNT_OFFSET, &writecount, sizeof(writecount));
    file_mode = FMODE_WRITE;
    memcpy(file + FILE_MODE_OFFSET, &file_mode, sizeof(file_mode));
    if (kb_fs_subsystem_finish_open(
            file,
            dentry,
            (void *)(uintptr_t)&finish_open_smoke_failure) != -5)
    {
        return 122;
    }
    memcpy(&writecount, inode + INODE_WRITECOUNT_OFFSET, sizeof(writecount));
    memcpy(&observed, file + FILE_INODE_OFFSET, sizeof(observed));
    if (writecount != 0 || observed != NULL) {
        return 123;
    }

    pointer = (void *)(uintptr_t)&finish_close_smoke_callback;
    memcpy(file_operations + FILE_OP_RELEASE_OFFSET, &pointer, sizeof(pointer));
    unsigned char vfsmount[0x40] = {0};
    void *opened_file = NULL;
    finish_open_smoke_calls = 0;
    finish_close_smoke_calls = 0;
    if (kb_fs_subsystem_file_open(
            vfsmount,
            dentry,
            KB_FS_FILE_ACCESS_READ | KB_FS_FILE_ACCESS_WRITE,
            &opened_file) != 0 ||
        opened_file == NULL || finish_open_smoke_calls != 1)
    {
        return 124;
    }
    uint32_t ra_pages = 0;
    uint64_t ra_previous_position = 0;
    memcpy(&ra_pages,
        (const unsigned char *)opened_file + FILE_RA_OFFSET +
            FILE_RA_PAGES_OFFSET,
        sizeof(ra_pages));
    memcpy(&ra_previous_position,
        (const unsigned char *)opened_file + FILE_RA_OFFSET +
            FILE_RA_PREV_POS_OFFSET,
        sizeof(ra_previous_position));
    memcpy(&writecount, inode + INODE_WRITECOUNT_OFFSET, sizeof(writecount));
    if (ra_pages != 128u || ra_previous_position != UINT64_MAX ||
        writecount != 1u)
    {
        return 125;
    }
    if (kb_fs_subsystem_file_close(opened_file) != 0 ||
        finish_close_smoke_calls != 1)
    {
        return 126;
    }
    memcpy(&writecount, inode + INODE_WRITECOUNT_OFFSET, sizeof(writecount));
    if (writecount != 0) {
        return 127;
    }
    return 0;
}

int main(void)
{
    int execution_context_result = run_execution_context_kthread_smoke();
    if (execution_context_result != 0) {
        return execution_context_result;
    }
    int kthread_result = run_cooperative_kthread_smoke();
    if (kthread_result != 0) {
        return kthread_result;
    }
    int bit_wait_result = run_cooperative_bit_wait_smoke();
    if (bit_wait_result != 0) {
        return bit_wait_result;
    }
    int waitqueue_result = run_cooperative_waitqueue_smoke();
    if (waitqueue_result != 0) {
        return waitqueue_result;
    }
    int core_helpers_result = run_linux_core_helpers_smoke();
    if (core_helpers_result != 0) {
        return core_helpers_result;
    }
    int inode_result = run_inode_init_once_smoke();
    if (inode_result != 0) {
        return inode_result;
    }
    int inode_version_result = run_inode_version_and_flags_smoke();
    if (inode_version_result != 0) {
        return inode_version_result;
    }
    int atime_result = run_atime_and_file_time_smoke();
    if (atime_result != 0) {
        return atime_result;
    }
    int timestamp_result = run_inode_timestamp_and_sync_policy_smoke();
    if (timestamp_result != 0) {
        return timestamp_result;
    }
    int generic_helpers_result = run_generic_metadata_seek_and_error_smoke();
    if (generic_helpers_result != 0) {
        return generic_helpers_result;
    }
    int parser_result = run_fs_parser_and_statfs_smoke();
    if (parser_result != 0) {
        return parser_result;
    }
    int mmap_result = run_filemap_fault_and_mkwrite_smoke();
    if (mmap_result != 0) {
        return mmap_result;
    }
    int readahead_result = run_pagecache_readahead_smoke();
    if (readahead_result != 0) {
        return readahead_result;
    }
    int exportfs_result = run_exportfs_helpers_smoke();
    if (exportfs_result != 0) {
        return exportfs_result;
    }
    int finish_open_result = run_finish_open_smoke();
    if (finish_open_result != 0) {
        return finish_open_result;
    }
    int acl_result = run_posix_acl_smoke();
    if (acl_result != 0) {
        return acl_result;
    }
    int storage_limit_result = run_storage_limit_helpers_smoke();
    if (storage_limit_result != 0) {
        return storage_limit_result;
    }
    int dentry_result = run_dentry_lifecycle_smoke();
    if (dentry_result != 0) {
        return dentry_result;
    }
    int devpts_dentry_result = run_devpts_dentry_layout_smoke();
    if (devpts_dentry_result != 0) {
        return devpts_dentry_result;
    }

    kb_device_backend_t *backend = NULL;
    if (kb_linux_mock_device_create(&backend) != KB_OK || backend == NULL) {
        return 1;
    }

    kb_interface_t *fs_interface = NULL;
    fs_ipc_smoke_t smoke = {0};
    kb_linux_interface_desc_t fs_interface_desc = {
        .name = "linux-ipc-fs",
        .subsystem = "fs",
        .endpoint = "kobox.fs",
        .dispatch = fs_ipc_dispatch,
        .dispatch_ctx = &smoke,
    };
    if (kb_linux_ipc_interface_create(&fs_interface_desc, &fs_interface) != KB_OK || fs_interface == NULL) {
        kb_device_backend_destroy(backend);
        return 2;
    }

    kb_interface_t *interfaces[] = {
        fs_interface,
    };
    kb_platform_desc_t platform_desc = {
        "linux-fs-platform",
        backend,
        interfaces,
        1,
    };
    kb_platform_t *platform = NULL;
    if (kb_platform_create(&platform_desc, &platform) != KB_OK || platform == NULL) {
        kb_interface_destroy(fs_interface);
        kb_device_backend_destroy(backend);
        return 3;
    }

    kb_interface_t *bound = NULL;
    if (kb_fs_subsystem_bind_ipc_interface(platform, &bound) != KB_OK || bound != fs_interface) {
        kb_platform_destroy(platform);
        return 4;
    }

    uint64_t mount_handle = 0;
    kb_fs_mount_desc_t mount_desc = {
        .source = "/dev/kobox0",
        .target = "/mnt/kobox",
        .fs_type = "ext4",
    };
    if (kb_fs_subsystem_mount(bound, &mount_desc, &mount_handle) != KB_OK || mount_handle != 7) {
        kb_platform_destroy(platform);
        return 5;
    }

    char read_buffer[8] = {0};
    size_t read_bytes = 0;
    kb_fs_read_desc_t read_desc = {
        .handle = mount_handle,
        .path = "/mnt/kobox/file",
        .buffer = read_buffer,
        .byte_count = sizeof(read_buffer),
        .out_bytes = &read_bytes,
    };
    if (kb_fs_subsystem_read(bound, &read_desc) != KB_OK ||
        read_bytes != 5 ||
        memcmp(read_buffer, "hello", 5) != 0)
    {
        kb_platform_destroy(platform);
        return 6;
    }

    read_bytes = 99;
    read_desc.path = "/mnt/kobox/empty";
    if (kb_fs_subsystem_read(bound, &read_desc) != KB_OK || read_bytes != 0) {
        kb_platform_destroy(platform);
        return 16;
    }

    size_t written_bytes = 0;
    kb_fs_write_desc_t write_desc = {
        .handle = mount_handle,
        .path = "/mnt/kobox/file",
        .buffer = "world",
        .byte_count = 5,
        .out_bytes = &written_bytes,
    };
    if (kb_fs_subsystem_write(bound, &write_desc) != KB_OK || written_bytes != 5) {
        kb_platform_destroy(platform);
        return 7;
    }

    char dir_buffer[8] = {0};
    size_t dir_bytes = 0;
    kb_fs_readdir_desc_t readdir_desc = {
        .handle = mount_handle,
        .path = "/mnt/kobox",
        .buffer = dir_buffer,
        .byte_count = sizeof(dir_buffer),
        .out_bytes = &dir_bytes,
    };
    if (kb_fs_subsystem_readdir(bound, &readdir_desc) != KB_OK ||
        dir_bytes != 4 ||
        memcmp(dir_buffer, "a\nb\n", 4) != 0)
    {
        kb_platform_destroy(platform);
        return 8;
    }

    if (smoke.mount_seen != 1 || smoke.read_seen != 2 || smoke.write_seen != 1 || smoke.readdir_seen != 1) {
        kb_platform_destroy(platform);
        return 9;
    }

    kb_interface_unbind(bound);
    kb_platform_destroy(platform);
    int local_result = run_local_fs_model_smoke();
    if (local_result != 0) {
        return local_result;
    }
    int rwsem_result = run_percpu_rwsem_lifecycle_smoke();
    if (rwsem_result != 0) {
        return rwsem_result;
    }
    int page_model_result = run_page_model_smoke();
    if (page_model_result != 0) {
        return page_model_result;
    }
    int pagecache_result = run_pagecache_truncate_smoke();
    if (pagecache_result != 0) {
        return pagecache_result;
    }
    int invalidate_result = run_pagecache_invalidate_smoke();
    if (invalidate_result != 0) {
        return invalidate_result;
    }
    int buffer_folio_result = run_buffer_folio_helpers_smoke();
    if (buffer_folio_result != 0) {
        return buffer_folio_result;
    }
    int write_cache_result = run_write_cache_pages_smoke();
    if (write_cache_result != 0) {
        return write_cache_result;
    }
    int setattr_result = run_setattr_helpers_smoke();
    if (setattr_result != 0) {
        return setattr_result;
    }
    int link_result = run_simple_get_link_smoke();
    if (link_result != 0) {
        return link_result;
    }
    int sync_result = run_inode_sync_smoke();
    if (sync_result != 0) {
        return sync_result;
    }
    int errseq_result = run_errseq_smoke();
    if (errseq_result != 0) {
        return errseq_result;
    }
    int insert_result = run_insert_inode_smoke();
    if (insert_result != 0) {
        return insert_result;
    }
    return run_inode_reference_smoke();
}
