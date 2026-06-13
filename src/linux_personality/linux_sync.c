#include "kobox/shim.h"
#include "linux_subsystem/usb/usb.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define fprintf(stream, ...) kb_tracef(__VA_ARGS__)

static int trace_work_enabled(void)
{
    static int cached = -1;
    if (cached >= 0) {
        return cached;
    }
    const char *value = getenv("KOBOX_TRACE_WORK");
    cached = value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
    return cached;
}

void kb_mutex_init(void *lock)
{
    (void)lock;
}

void kb_mutex_lock(void *lock)
{
    (void)lock;
}

void kb_mutex_unlock(void *lock)
{
    (void)lock;
}

int kb_mutex_trylock(void *lock)
{
    (void)lock;
    return 1;
}

void kb_complete(void *completion)
{
    if (completion != NULL) {
        uint32_t done = 1;
        memcpy(completion, &done, sizeof(done));
        if (trace_work_enabled()) {
            fprintf(stderr, "kobox work: complete completion=%p done=%u\n", completion, done);
        }
    }
}

void kb_complete_all(void *completion)
{
    if (completion != NULL) {
        uint32_t done = UINT32_MAX / 2u;
        memcpy(completion, &done, sizeof(done));
        if (trace_work_enabled()) {
            fprintf(stderr, "kobox work: complete_all completion=%p done=%u\n", completion, done);
        }
    }
}

void kb_init_completion(void *completion)
{
    if (completion != NULL) {
        uint32_t done = 0;
        memcpy(completion, &done, sizeof(done));
    }
}

void kb_init_waitqueue_head(void *wq_head)
{
    (void)wq_head;
}

void kb_init_swait_queue_head(void *wq_head)
{
    (void)wq_head;
}

static void run_wait_progress(void)
{
    if (kb_deferred_work_is_draining()) {
        kb_run_deferred_bottom_halves();
    } else {
        kb_run_deferred_work();
    }
    if (kb_usb_root_hub_poll_needed()) {
        (void)kb_usb_poll_root_hubs();
    }
}

unsigned long kb_wait_for_completion(void *completion)
{
    if (completion == NULL) {
        return 0;
    }
    if (trace_work_enabled()) {
        fprintf(stderr,
            "kobox work: wait_for_completion completion=%p caller=%p\n",
            completion,
            __builtin_return_address(0));
    }
    for (unsigned i = 0; i < 10000; i++) {
        run_wait_progress();
        (void)kb_handle_any_irq_no_work(0);
        uint32_t done = 0;
        memcpy(&done, completion, sizeof(done));
        if (done != 0) {
            done--;
            memcpy(completion, &done, sizeof(done));
            if (trace_work_enabled()) {
                fprintf(stderr,
                    "kobox work: wait_for_completion done completion=%p remaining=%u caller=%p\n",
                    completion,
                    done,
                    __builtin_return_address(0));
            }
            return 1;
        }
        (void)kb_handle_any_irq_no_work(1000000ull);
    }
    if (trace_work_enabled()) {
        fprintf(stderr,
            "kobox work: wait_for_completion timeout completion=%p caller=%p\n",
            completion,
            __builtin_return_address(0));
    }
    return 0;
}

unsigned long kb_wait_for_completion_io_timeout(void *completion, unsigned long timeout)
{
    if (completion == NULL) {
        return 0;
    }
    if (trace_work_enabled()) {
        fprintf(stderr,
            "kobox work: wait_for_completion_timeout completion=%p timeout=%lu caller=%p\n",
            completion,
            timeout,
            __builtin_return_address(0));
    }
    const unsigned long loops = timeout == 0 ? 20 : (timeout > 10000 ? 10000 : timeout);
    for (unsigned long i = 0; i < loops; i++) {
        run_wait_progress();
        (void)kb_handle_any_irq_no_work(0);
        uint32_t done = 0;
        memcpy(&done, completion, sizeof(done));
        if (done != 0) {
            done--;
            memcpy(completion, &done, sizeof(done));
            if (trace_work_enabled()) {
                fprintf(
                    stderr,
                    "kobox work: wait_for_completion_timeout done completion=%p remaining=%u left=%lu caller=%p\n",
                    completion,
                    done,
                    timeout == 0 ? 1 : timeout - i,
                    __builtin_return_address(0));
            }
            return timeout == 0 ? 1 : timeout - i;
        }
        (void)kb_handle_any_irq_no_work(1000000ull);
    }
    if (trace_work_enabled()) {
        fprintf(stderr,
            "kobox work: wait_for_completion_timeout expired completion=%p timeout=%lu caller=%p\n",
            completion,
            timeout,
            __builtin_return_address(0));
    }
    return 0;
}
