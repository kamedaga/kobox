#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "kobox/backend.h"
#include "kobox/backend_linux_mock.h"
#include "kobox/backend_linux_vfio.h"
#include "kobox/backend_pachaos_capsule.h"
#include "kobox/module.h"
#include "kobox/shim.h"
#include "subsystem/input/input.h"
#include "subsystem/usb/storage.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32) && defined(__GLIBC__)
#include <execinfo.h>
#endif
#if !defined(_WIN32)
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
#endif

enum {
    KB_RUN_DEPS_MAX = 16,
};

typedef struct loaded_input_module {
    const char *path;
    void *data;
    size_t size;
    kb_module_t *module;
} loaded_input_module_t;

#if !defined(_WIN32)
static void crash_handler(int signo, siginfo_t *info, void *ucontext)
{
#if defined(__GLIBC__)
    void *frames[64];
    int count = backtrace(frames, 64);
#endif
    const char message[] = "kobox-run: crash while executing module\n";
    (void)write(STDERR_FILENO, message, sizeof(message) - 1);
#if defined(__x86_64__)
    ucontext_t *context = (ucontext_t *)ucontext;
    if (context != NULL) {
        uintptr_t *sp = (uintptr_t *)context->uc_mcontext.gregs[REG_RSP];
        dprintf(
            STDERR_FILENO,
            "kobox-run: signal=%d addr=%p rip=%p rsp=%p\n",
            signo,
            info != NULL ? info->si_addr : NULL,
            (void *)(uintptr_t)context->uc_mcontext.gregs[REG_RIP],
            (void *)sp);
        dprintf(
            STDERR_FILENO,
            "kobox-run: rax=%p rbx=%p rcx=%p rdx=%p rdi=%p rsi=%p r12=%p\n",
            (void *)(uintptr_t)context->uc_mcontext.gregs[REG_RAX],
            (void *)(uintptr_t)context->uc_mcontext.gregs[REG_RBX],
            (void *)(uintptr_t)context->uc_mcontext.gregs[REG_RCX],
            (void *)(uintptr_t)context->uc_mcontext.gregs[REG_RDX],
            (void *)(uintptr_t)context->uc_mcontext.gregs[REG_RDI],
            (void *)(uintptr_t)context->uc_mcontext.gregs[REG_RSI],
            (void *)(uintptr_t)context->uc_mcontext.gregs[REG_R12]);
        if (sp != NULL && getenv("KOBOX_CRASH_STACK") != NULL) {
            for (int i = 0; i < 8; i++) {
                dprintf(STDERR_FILENO, "kobox-run: stack[%d]=%p\n", i, (void *)sp[i]);
            }
        }
    }
#else
    if (info != NULL) {
        dprintf(STDERR_FILENO, "kobox-run: signal=%d addr=%p\n", signo, info->si_addr);
    }
#endif
#if defined(__GLIBC__)
    backtrace_symbols_fd(frames, count, STDERR_FILENO);
#endif
    signal(signo, SIG_DFL);
    raise(signo);
}

static void install_crash_handler(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = crash_handler;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    sigaction(SIGSEGV, &action, NULL);
    sigaction(SIGBUS, &action, NULL);
    sigaction(SIGILL, &action, NULL);
    sigaction(SIGABRT, &action, NULL);
}
#else
static void install_crash_handler(void)
{
}
#endif

static const char *status_name(kb_status_t status)
{
    switch (status) {
    case KB_OK:
        return "KB_OK";
    case KB_ERR_INVALID:
        return "KB_ERR_INVALID";
    case KB_ERR_NOT_FOUND:
        return "KB_ERR_NOT_FOUND";
    case KB_ERR_DENIED:
        return "KB_ERR_DENIED";
    case KB_ERR_NOMEM:
        return "KB_ERR_NOMEM";
    case KB_ERR_IO:
        return "KB_ERR_IO";
    case KB_ERR_UNSUPPORTED:
        return "KB_ERR_UNSUPPORTED";
    default:
        return "KB_ERR_UNKNOWN";
    }
}

static kb_status_t read_file(const char *path, void **out_data, size_t *out_size)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
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

    void *data = malloc((size_t)size);
    if (data == NULL) {
        fclose(file);
        return KB_ERR_NOMEM;
    }
    if (fread(data, 1, (size_t)size, file) != (size_t)size) {
        free(data);
        fclose(file);
        return KB_ERR_IO;
    }

    fclose(file);
    *out_data = data;
    *out_size = (size_t)size;
    return KB_OK;
}

static void drain_after_init(kb_backend_t *backend, unsigned long drain_ms)
{
    if (drain_ms == 0) {
        return;
    }

    kb_shim_set_backend(backend);
    const kb_backend_ops_t *ops = kb_backend_get_ops(backend);
    uint64_t start_ns = 0;
    if (ops != NULL && ops->monotonic_ns != NULL) {
        start_ns = ops->monotonic_ns(backend);
    }

    for (unsigned long i = 0; i < drain_ms; i++) {
        kb_run_deferred_work();
        (void)kb_usb_poll_root_hubs();
        (void)kb_handle_any_irq(1000000ull);
        if (start_ns != 0 && ops != NULL && ops->monotonic_ns != NULL) {
            uint64_t now_ns = ops->monotonic_ns(backend);
            if (now_ns >= start_ns && now_ns - start_ns >= ((uint64_t)drain_ms * 1000000ull)) {
                break;
            }
        }
    }
    kb_run_deferred_work();
    (void)kb_usb_poll_root_hubs();
    (void)kb_handle_any_irq(0);
    kb_shim_set_backend(NULL);
}

static void cleanup_all_irqs_for_backend(kb_backend_t *backend)
{
    if (backend == NULL) {
        return;
    }
    kb_shim_set_backend(backend);
    kb_free_all_irqs();
    kb_shim_set_backend(NULL);
}

static void destroy_backend_after_cleanup(kb_backend_t *backend)
{
    cleanup_all_irqs_for_backend(backend);
    kb_backend_destroy(backend);
}

static void configure_pachaos_driver_preference(const char *path)
{
#if !defined(_WIN32)
    if (path == NULL || getenv("KOBOX_PACHAOS_PREFERRED_CLASS") != NULL) {
        return;
    }
    if (strstr(path, "nvme") != NULL || strstr(path, "NVME") != NULL) {
        (void)setenv("KOBOX_PACHAOS_PREFERRED_CLASS", "0x0108", 0);
        return;
    }
    if (strstr(path, "xhci") != NULL || strstr(path, "XHCI") != NULL ||
        strstr(path, "usb") != NULL || strstr(path, "USB") != NULL) {
        (void)setenv("KOBOX_PACHAOS_PREFERRED_CLASS", "0x0c03", 0);
    }
#else
    (void)path;
#endif
}

int main(int argc, char **argv)
{
    install_crash_handler();

    const char *path = NULL;
    const char *backend_name = "mock";
    const char *pci_bdf = NULL;
    const char *capsule_text = NULL;
    const char *dep_paths[KB_RUN_DEPS_MAX];
    size_t dep_count = 0;
    unsigned long drain_ms = 0;
    int argi = 1;
    while (argi < argc && argv[argi][0] == '-') {
        if (strncmp(argv[argi], "--backend=", 10) == 0) {
            backend_name = argv[argi] + 10;
            argi++;
            continue;
        }
        if (strncmp(argv[argi], "--pci=", 6) == 0) {
            pci_bdf = argv[argi] + 6;
            argi++;
            continue;
        }
        if (strncmp(argv[argi], "--capsule=", 10) == 0) {
            capsule_text = argv[argi] + 10;
            argi++;
            continue;
        }
        if (strncmp(argv[argi], "--dep=", 6) == 0) {
            if (dep_count >= KB_RUN_DEPS_MAX) {
                fprintf(stderr, "too many dependencies\n");
                return 1;
            }
            dep_paths[dep_count++] = argv[argi] + 6;
            argi++;
            continue;
        }
        if (strncmp(argv[argi], "--drain-ms=", 11) == 0) {
            drain_ms = strtoul(argv[argi] + 11, NULL, 10);
            argi++;
            continue;
        }
        fprintf(stderr, "usage: kobox-run [--backend=mock|vfio|pachaos --pci=<BDF> --capsule=<DeviceCapsule> --dep=<module.ko> --drain-ms=<ms>] run <module.ko>\n");
        return 1;
    }

    if (argi + 1 == argc) {
        path = argv[argi];
    } else if (argi + 2 == argc && strcmp(argv[argi], "run") == 0) {
        path = argv[argi + 1];
    } else {
        fprintf(stderr, "usage: kobox-run [--backend=mock|vfio|pachaos --pci=<BDF> --capsule=<DeviceCapsule> --dep=<module.ko> --drain-ms=<ms>] run <module.ko>\n");
        return 1;
    }

    void *data = NULL;
    size_t size = 0;
    kb_status_t status = read_file(path, &data, &size);
    if (status != KB_OK) {
        fprintf(stderr, "read failed: %s (%d)\n", status_name(status), status);
        return 2;
    }

    kb_backend_t *backend = NULL;
    if (strcmp(backend_name, "mock") == 0) {
        status = kb_linux_mock_create(&backend);
    } else if (strcmp(backend_name, "vfio") == 0) {
        if (pci_bdf == NULL) {
            free(data);
            fprintf(stderr, "vfio backend requires --pci=<BDF>\n");
            return 3;
        }
        status = kb_linux_vfio_create(pci_bdf, &backend);
    } else if (strcmp(backend_name, "pachaos") == 0 || strcmp(backend_name, "pachaos_capsule") == 0) {
        configure_pachaos_driver_preference(path);
        if (capsule_text != NULL) {
            uint64_t capsule = 0;
            status = kb_pachaos_capsule_parse_token(capsule_text, &capsule);
            if (status == KB_OK) {
                status = kb_pachaos_capsule_create(capsule, &backend);
            }
        } else {
            status = kb_pachaos_capsule_create_from_env(&backend);
        }
    } else {
        free(data);
        fprintf(stderr, "unknown backend: %s\n", backend_name);
        return 3;
    }
    if (status != KB_OK) {
        free(data);
        fprintf(stderr, "%s backend failed: %s (%d)\n", backend_name, status_name(status), status);
        return 3;
    }

    loaded_input_module_t deps[KB_RUN_DEPS_MAX];
    memset(deps, 0, sizeof(deps));
    for (size_t i = 0; i < dep_count; i++) {
        deps[i].path = dep_paths[i];
        status = read_file(deps[i].path, &deps[i].data, &deps[i].size);
        if (status != KB_OK) {
            fprintf(stderr, "dependency read failed: %s: %s (%d)\n", deps[i].path, status_name(status), status);
            destroy_backend_after_cleanup(backend);
            free(data);
            return 4;
        }
        kb_module_image_t dep_image = {
            .data = deps[i].data,
            .size = deps[i].size,
            .name = deps[i].path,
        };
        status = kb_module_open_image(&dep_image, backend, &deps[i].module);
        if (status != KB_OK) {
            fprintf(stderr, "dependency open failed: %s: %s (%d)\n", deps[i].path, status_name(status), status);
            for (size_t j = 0; j <= i; j++) {
                free(deps[j].data);
            }
            destroy_backend_after_cleanup(backend);
            free(data);
            return 4;
        }
        int dep_result = 0;
        status = kb_module_call_init(deps[i].module, &dep_result);
        if (status == KB_ERR_NOT_FOUND) {
            printf("dependency %s has no init_module\n", deps[i].path);
            continue;
        }
        if (status != KB_OK) {
            fprintf(stderr, "dependency init failed: %s: %s (%d)\n", deps[i].path, status_name(status), status);
            for (size_t j = 0; j <= i; j++) {
                if (deps[j].module != NULL) {
                    kb_module_close(deps[j].module);
                }
                free(deps[j].data);
            }
            destroy_backend_after_cleanup(backend);
            free(data);
            return 4;
        }
        printf("dependency %s init_module returned %d\n", deps[i].path, dep_result);
    }

    kb_module_image_t image = {
        .data = data,
        .size = size,
        .name = path,
    };
    kb_module_t *module = NULL;
    status = kb_module_open_image(&image, backend, &module);
    if (status != KB_OK) {
        for (size_t i = dep_count; i > 0; i--) {
            (void)kb_module_call_cleanup(deps[i - 1].module);
            kb_module_close(deps[i - 1].module);
            free(deps[i - 1].data);
        }
        destroy_backend_after_cleanup(backend);
        free(data);
        fprintf(stderr, "open failed: %s (%d)\n", status_name(status), status);
        return 4;
    }

    int result = 0;
    status = kb_module_call_init(module, &result);
    if (status != KB_OK) {
        kb_module_close(module);
        for (size_t i = dep_count; i > 0; i--) {
            (void)kb_module_call_cleanup(deps[i - 1].module);
            kb_module_close(deps[i - 1].module);
            free(deps[i - 1].data);
        }
        destroy_backend_after_cleanup(backend);
        free(data);
        fprintf(stderr, "init failed: %s (%d)\n", status_name(status), status);
        return 5;
    }

    printf("init_module returned %d\n", result);
    if (result != 0) {
        printf("cleanup_module skipped after failed init_module\n");
        kb_module_close(module);
        for (size_t i = dep_count; i > 0; i--) {
            (void)kb_module_call_cleanup(deps[i - 1].module);
            kb_module_close(deps[i - 1].module);
            free(deps[i - 1].data);
        }
        destroy_backend_after_cleanup(backend);
        free(data);
        return 0;
    }
    if (getenv("KOBOX_NVME_IO_SMOKE") != NULL) {
        kb_shim_set_backend(backend);
        int io_result = kb_nvme_io_smoke();
        kb_shim_set_backend(NULL);
        if (io_result != 0) {
            (void)kb_module_call_cleanup(module);
            kb_module_close(module);
            for (size_t i = dep_count; i > 0; i--) {
                (void)kb_module_call_cleanup(deps[i - 1].module);
                kb_module_close(deps[i - 1].module);
                free(deps[i - 1].data);
            }
            destroy_backend_after_cleanup(backend);
            free(data);
            fprintf(stderr, "nvme io smoke failed: %d\n", io_result);
            return 8;
        }
    }
    if (getenv("KOBOX_FOPS_SMOKE") != NULL) {
        int fops_result = kb_module_run_registered_ops_smoke();
        if (fops_result != 0) {
            (void)kb_module_call_cleanup(module);
            kb_module_close(module);
            for (size_t i = dep_count; i > 0; i--) {
                (void)kb_module_call_cleanup(deps[i - 1].module);
                kb_module_close(deps[i - 1].module);
                free(deps[i - 1].data);
            }
            destroy_backend_after_cleanup(backend);
            free(data);
            fprintf(stderr, "fops smoke failed: %d\n", fops_result);
            return 9;
        }
    }
    drain_after_init(backend, drain_ms);
    const char *usb_storage_io_smoke = getenv("KOBOX_USB_STORAGE_IO_SMOKE");
    if (usb_storage_io_smoke != NULL && usb_storage_io_smoke[0] != '\0' && strcmp(usb_storage_io_smoke, "0") != 0) {
        int storage_io_result = kb_usb_storage_subsystem_run_io_smoke(stdout);
        if (storage_io_result != 0) {
            (void)kb_module_call_cleanup(module);
            kb_module_close(module);
            for (size_t i = dep_count; i > 0; i--) {
                (void)kb_module_call_cleanup(deps[i - 1].module);
                kb_module_close(deps[i - 1].module);
                free(deps[i - 1].data);
            }
            destroy_backend_after_cleanup(backend);
            free(data);
            fprintf(stderr, "usb storage io smoke failed: %d\n", storage_io_result);
            return 10;
        }
    }
    const char *input_summary = getenv("KOBOX_INPUT_SUMMARY");
    if (input_summary != NULL && input_summary[0] != '\0' && strcmp(input_summary, "0") != 0) {
        kb_input_subsystem_print_summary(stdout);
    }
    const char *usb_storage_summary = getenv("KOBOX_USB_STORAGE_SUMMARY");
    if (usb_storage_summary != NULL && usb_storage_summary[0] != '\0' && strcmp(usb_storage_summary, "0") != 0) {
        kb_usb_storage_subsystem_print_summary(stdout);
    }

    status = kb_module_call_cleanup(module);
    if (status == KB_OK) {
        printf("cleanup_module returned\n");
    } else if (status != KB_ERR_NOT_FOUND) {
        kb_module_close(module);
        destroy_backend_after_cleanup(backend);
        free(data);
        fprintf(stderr, "cleanup failed: %s (%d)\n", status_name(status), status);
        return 6;
    }

    kb_module_close(module);
    for (size_t i = dep_count; i > 0; i--) {
        status = kb_module_call_cleanup(deps[i - 1].module);
        if (status == KB_OK) {
            printf("dependency %s cleanup_module returned\n", deps[i - 1].path);
        } else if (status != KB_ERR_NOT_FOUND) {
            fprintf(stderr, "dependency cleanup failed: %s: %s (%d)\n", deps[i - 1].path, status_name(status), status);
            kb_module_close(deps[i - 1].module);
            free(deps[i - 1].data);
            destroy_backend_after_cleanup(backend);
            free(data);
            return 7;
        }
        kb_module_close(deps[i - 1].module);
        free(deps[i - 1].data);
    }
    destroy_backend_after_cleanup(backend);
    free(data);
    return 0;
}
