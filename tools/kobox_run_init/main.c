#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "kobox/device.h"
#include "kobox/device_linux_mock.h"
#include "kobox/device_linux_vfio.h"
#include "kobox/device_pachaos_capsule.h"
#include "kobox/module.h"
#include "kobox/shim.h"
#include "linux_subsystem/ata/ata.h"
#include "linux_subsystem/fs/fs.h"
#include "linux_subsystem/input/input.h"
#include "linux_subsystem/usb/storage.h"
#include "linux_subsystem/usb/usb.h"
#include "loader/module_context.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <io.h>
#else
#include <signal.h>
#include <dlfcn.h>
#include <unistd.h>
#if defined(__x86_64__)
#include <ucontext.h>
#endif
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

#if !defined(_WIN32) && defined(__x86_64__)
static void signal_diagnostics_handler(int signal_number, siginfo_t *info, void *uctx)
{
    ucontext_t *context = (ucontext_t *)uctx;
    void *rip = (void *)context->uc_mcontext.gregs[REG_RIP];
    void *rsp = (void *)context->uc_mcontext.gregs[REG_RSP];
    void *rdi = (void *)context->uc_mcontext.gregs[REG_RDI];
    void *rsi = (void *)context->uc_mcontext.gregs[REG_RSI];
    void *rdx = (void *)context->uc_mcontext.gregs[REG_RDX];
    void *rcx = (void *)context->uc_mcontext.gregs[REG_RCX];
    const uint8_t *insn = (const uint8_t *)rip;
    fprintf(stderr,
        "kobox-run: signal=%d rip=%p rsp=%p fault=%p external_target=%p caller_gs=0x%lx callee_gs=0x%lx rdi=%p rsi=%p rdx=%p rcx=%p\n",
        signal_number,
        rip,
        rsp,
        info == NULL ? NULL : info->si_addr,
        kb_module_current_external_call_target(),
        kb_module_current_external_call_caller_gs(),
        kb_module_current_external_call_callee_gs(),
        rdi,
        rsi,
        rdx,
        rcx);
    if (insn != NULL && insn[0] == 0xff && insn[1] == 0x15) {
        int32_t displacement = 0;
        uintptr_t slot = 0;
        uintptr_t target = 0;
        memcpy(&displacement, insn + 2, sizeof(displacement));
        slot = (uintptr_t)(insn + 6) + (intptr_t)displacement;
        memcpy(&target, (const void *)slot, sizeof(target));
        fprintf(stderr, "kobox-run: indirect_call slot=%p target=%p displacement=%d\n", (void *)slot, (void *)target, displacement);
    }
    {
        Dl_info rip_info;
        Dl_info target_info;
        void *external_target = kb_module_current_external_call_target();
        if (dladdr(rip, &rip_info) != 0) {
            fprintf(stderr, "kobox-run: rip_symbol=%s object=%s base=%p\n", rip_info.dli_sname == NULL ? "(unknown)" : rip_info.dli_sname, rip_info.dli_fname == NULL ? "(unknown)" : rip_info.dli_fname, rip_info.dli_fbase);
        }
        if (external_target != NULL && dladdr(external_target, &target_info) != 0) {
            fprintf(stderr, "kobox-run: external_symbol=%s object=%s base=%p offset=0x%lx\n", target_info.dli_sname == NULL ? "(unknown)" : target_info.dli_sname, target_info.dli_fname == NULL ? "(unknown)" : target_info.dli_fname, target_info.dli_fbase, (unsigned long)((uintptr_t)external_target - (uintptr_t)target_info.dli_fbase));
        }
    }
    _Exit(128 + signal_number);
}

static void install_signal_diagnostics(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = signal_diagnostics_handler;
    action.sa_flags = SA_SIGINFO;
    (void)sigaction(SIGSEGV, &action, NULL);
    (void)sigaction(SIGILL, &action, NULL);
    (void)sigaction(SIGBUS, &action, NULL);
    (void)sigaction(SIGABRT, &action, NULL);
}
#else
static void install_signal_diagnostics(void)
{
}
#endif

static void stdout_write_all(const char *text, size_t length)
{
    if (text == NULL || length == 0) {
        return;
    }
    size_t written = 0;
    while (written < length) {
#if defined(_WIN32)
        int n = _write(1, text + written, (unsigned int)(length - written));
#else
        ssize_t n = write(STDOUT_FILENO, text + written, length - written);
#endif
        if (n <= 0) {
            break;
        }
        written += (size_t)n;
    }
}

static void stdout_linef(const char *fmt, ...)
{
    char line[256];
    va_list args;
    va_start(args, fmt);
    int length = vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    if (length <= 0) {
        return;
    }
    size_t write_len = (size_t)length;
    if (write_len >= sizeof(line)) {
        write_len = sizeof(line) - 1u;
        line[write_len] = '\0';
    }
    if (write_len != 0 && line[write_len - 1u] == '\n' && write_len + 1u < sizeof(line)) {
        line[write_len - 1u] = '\r';
        line[write_len] = '\n';
        write_len++;
        line[write_len] = '\0';
    }
    stdout_write_all(line, write_len);
}

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
    case KB_ERR_PCI_CONFIG:
        return "KB_ERR_PCI_CONFIG";
    default:
        return "KB_ERR_UNKNOWN";
    }
}

static int cleanup_failure_code(kb_status_t status)
{
    return status == KB_ERR_NOT_FOUND ? 0 : 1;
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

static void drain_after_init(kb_device_backend_t *backend, unsigned long drain_ms)
{
    if (drain_ms == 0) {
        return;
    }

    kb_shim_set_device_backend(backend);
    const kb_device_backend_ops_t *ops = kb_device_backend_get_ops(backend);
    uint64_t start_ns = 0;
    if (ops != NULL && ops->monotonic_ns != NULL) {
        start_ns = ops->monotonic_ns(backend);
    }

    for (unsigned long i = 0; i < drain_ms; i++) {
        kb_run_deferred_work();
        if (kb_usb_root_hub_poll_needed()) {
            (void)kb_usb_poll_root_hubs();
        }
        (void)kb_handle_any_irq(1000000ull);
        if (start_ns != 0 && ops != NULL && ops->monotonic_ns != NULL) {
            uint64_t now_ns = ops->monotonic_ns(backend);
            if (now_ns >= start_ns && now_ns - start_ns >= ((uint64_t)drain_ms * 1000000ull)) {
                break;
            }
        }
    }
    kb_run_deferred_work();
    if (kb_usb_root_hub_poll_needed()) {
        (void)kb_usb_poll_root_hubs();
    }
    (void)kb_handle_any_irq(0);
    kb_shim_set_device_backend(NULL);
}

static int mouse_live_enabled(void)
{
    const char *value = getenv("KOBOX_USB_HID_MOUSE_LIVE");
    return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
}

static int mouse_live_synthetic_enabled(void)
{
    const char *device = getenv("KOBOX_USB_SYNTHETIC_DEVICE");
    return device != NULL && strstr(device, "hid") != NULL && strstr(device, "mouse") != NULL;
}

static int mouse_live_xhci_only_enabled(void)
{
    const char *value = getenv("KOBOX_USB_HID_MOUSE_XHCI_ONLY");
    if (value != NULL && value[0] != '\0') {
        return strcmp(value, "0") != 0;
    }
    const char *real = getenv("KOBOX_USB_REAL_DEVICE");
    const char *backend = getenv("KOBOX_DEVICE_BACKEND");
    return real != NULL && real[0] != '\0' && strcmp(real, "0") != 0 &&
        backend != NULL && (strcmp(backend, "pachaos") == 0 || strcmp(backend, "pachaos_capsule") == 0);
}

static unsigned long mouse_live_duration_ms(unsigned long drain_ms)
{
    const char *value = getenv("KOBOX_USB_HID_MOUSE_LIVE_MS");
    if (value != NULL && value[0] != '\0') {
        unsigned long parsed = strtoul(value, NULL, 10);
        if (parsed != 0) {
            return parsed;
        }
    }
    return drain_ms != 0 ? drain_ms : 10000;
}

static int mouse_live_trace_enabled(void)
{
    const char *value = getenv("KOBOX_TRACE_MOUSE_LIVE");
    return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
}

static void mouse_live_trace(unsigned long loop, const char *phase)
{
    fprintf(stderr, "kobox-usb-hid-mouse-live-trace: loop=%lu phase=%s\n", loop, phase);
    fflush(stderr);
}

static void mouse_live_pause(int xhci_only)
{
    (void)xhci_only;
    kb_msleep(1);
}

static unsigned int mouse_live_print_limit(void)
{
    const char *value = getenv("KOBOX_USB_HID_MOUSE_LIVE_PRINT_LIMIT");
    if (value != NULL && value[0] != '\0') {
        unsigned long parsed = strtoul(value, NULL, 10);
        if (parsed <= 1024ul) {
            return (unsigned int)parsed;
        }
    }
    return 32;
}

static unsigned int drain_mouse_live_events(int *x, int *y, int *left, unsigned int *printed_details)
{
    enum {
        EV_SYN = 0x00,
        EV_KEY = 0x01,
        EV_REL = 0x02,
        EV_ABS = 0x03,
        REL_X = 0x00,
        REL_Y = 0x01,
        ABS_X = 0x00,
        ABS_Y = 0x01,
        BTN_LEFT = 0x110,
    };

    kb_input_event_t events[64];
    size_t count = kb_input_subsystem_pop_events(events, sizeof(events) / sizeof(events[0]));
    unsigned int changed_count = 0;
    const unsigned int print_limit = mouse_live_print_limit();
    const unsigned int non_coordinate_print_limit = print_limit < 8u ? print_limit : 8u;
    for (size_t i = 0; i < count; i++) {
        int changed = 0;
        int coordinate_event = 0;
        if (events[i].type == EV_REL && events[i].code == REL_X) {
            *x += events[i].value;
            changed = 1;
            coordinate_event = 1;
        } else if (events[i].type == EV_REL && events[i].code == REL_Y) {
            *y += events[i].value;
            changed = 1;
            coordinate_event = 1;
        } else if (events[i].type == EV_ABS && events[i].code == ABS_X) {
            *x = events[i].value;
            changed = 1;
            coordinate_event = 1;
        } else if (events[i].type == EV_ABS && events[i].code == ABS_Y) {
            *y = events[i].value;
            changed = 1;
            coordinate_event = 1;
        } else if (events[i].type == EV_KEY && events[i].code == BTN_LEFT) {
            *left = events[i].value != 0;
            changed = 1;
        } else if (events[i].type == EV_SYN) {
            changed = 1;
        }
        if (changed) {
            changed_count++;
            const int should_print_detail =
                coordinate_event ||
                (printed_details != NULL && *printed_details < non_coordinate_print_limit);
            if (printed_details != NULL && *printed_details < print_limit && should_print_detail) {
                stdout_linef(
                    "kobox-usb-hid-mouse-live: seq=%llu device_id=%u type=%u code=%u value=%d x=%d y=%d left=%d\n",
                    (unsigned long long)events[i].sequence,
                    events[i].device_id,
                    events[i].type,
                    events[i].code,
                    events[i].value,
                    *x,
                    *y,
                    *left);
                (*printed_details)++;
                if (*printed_details == print_limit) {
                    stdout_linef("kobox-usb-hid-mouse-live: detail output capped at %u events\n", print_limit);
                }
            }
        }
    }
    return changed_count;
}

static void mouse_live_run_deferred_step(int input_active)
{
    if (input_active) {
        kb_run_deferred_bottom_halves();
    } else {
        kb_run_deferred_work();
    }
}

static void mouse_live_drive_source(unsigned long loop, int synthetic, int xhci_only, int trace_live)
{
    if (synthetic) {
        if (trace_live) {
            mouse_live_trace(loop, "synthetic-before");
        }
        (void)kb_usb_synthesize_connected_hid_mouse();
        if (trace_live) {
            mouse_live_trace(loop, "synthetic-after");
        }
        return;
    }

    if (xhci_only) {
        return;
    }

    if (trace_live) {
        mouse_live_trace(loop, "hub-before");
    }
    if (kb_usb_root_hub_poll_needed()) {
        (void)kb_usb_poll_root_hubs();
    }
    if (trace_live) {
        mouse_live_trace(loop, "hub-after");
    }
}

static void mouse_live_open_inputs(int *input_active)
{
    int opened_this_loop = kb_input_subsystem_open_registered_devices();
    if (opened_this_loop != 0 || kb_input_subsystem_device_count() != 0) {
        *input_active = 1;
        kb_usb_pause_root_hub_poll_for_live();
    }
}

static int mouse_live_deadline_reached(
    const kb_device_backend_ops_t *ops,
    kb_device_backend_t *backend,
    uint64_t live_start_ns,
    unsigned long live_ms,
    unsigned long loops,
    unsigned long max_loops)
{
    if (ops != NULL && ops->monotonic_ns != NULL) {
        uint64_t now_ns = ops->monotonic_ns(backend);
        if (live_start_ns != 0 &&
            now_ns >= live_start_ns &&
            now_ns - live_start_ns >= ((uint64_t)live_ms * 1000000ull))
        {
            return 1;
        }
        return live_start_ns == 0 && loops >= max_loops;
    }
    return loops >= max_loops;
}

static void run_mouse_live(kb_device_backend_t *backend, unsigned long live_ms)
{
    if (live_ms == 0) {
        return;
    }

    kb_shim_set_device_backend(backend);
    kb_usb_reset_root_hub_poll_live_state();
    const kb_device_backend_ops_t *ops = kb_device_backend_get_ops(backend);
    int x = 0;
    int y = 0;
    int left = 0;
    unsigned long loops = 0;
    unsigned int printed = 0;
    unsigned int printed_details = 0;
    int input_active = 0;
    const int synthetic = mouse_live_synthetic_enabled();
    const int xhci_only = mouse_live_xhci_only_enabled();
    const int trace_live = mouse_live_trace_enabled();
    uint64_t live_start_ns = ops != NULL && ops->monotonic_ns != NULL ? ops->monotonic_ns(backend) : 0;
    const unsigned long max_loops = live_ms + 10000;
    stdout_linef(
        "kobox-usb-hid-mouse-live: begin ms=%lu mode=%s loop=deadline-start/bh-after-input\n",
        live_ms,
        synthetic ? "synthetic" : (xhci_only ? "real-xhci" : "hybrid"));
    if (!synthetic && xhci_only) {
        if (kb_usb_root_hub_poll_needed()) {
            (void)kb_usb_poll_root_hubs();
            /*
             * In xHCI-only live mode this first poll is only the connect kick.
             * After that, real xHCI events must drive enumeration; repeated
             * root-hub polls from completion waits can race the device setup
             * path and corrupt the ep0 descriptor handshake.
             */
            kb_usb_pause_root_hub_poll_for_live();
        }
    }
    for (;;) {
        if (trace_live) {
            mouse_live_trace(loops, "work-before");
        }
        mouse_live_run_deferred_step(input_active);
        if (trace_live) {
            mouse_live_trace(loops, "work-after");
        }
        mouse_live_drive_source(loops, synthetic, xhci_only, trace_live);
        if (trace_live) {
            mouse_live_trace(loops, "irq-wait-before");
        }
        (void)kb_handle_any_irq_no_work(1000000ull);
        if (trace_live) {
            mouse_live_trace(loops, "irq-wait-after");
        }
        if (trace_live) {
            mouse_live_trace(loops, "input-open-before");
        }
        mouse_live_open_inputs(&input_active);
        if (trace_live) {
            mouse_live_trace(loops, "input-open-after");
        }
        if (trace_live) {
            mouse_live_trace(loops, "irq-drain-before");
        }
        (void)kb_handle_any_irq_no_work(0);
        if (trace_live) {
            mouse_live_trace(loops, "irq-drain-after");
        }
        if (trace_live) {
            mouse_live_trace(loops, "event-drain-before");
        }
        unsigned int drained = drain_mouse_live_events(&x, &y, &left, &printed_details);
        printed += drained;
        if (drained != 0) {
            input_active = 1;
            kb_usb_pause_root_hub_poll_for_live();
        }
        if (trace_live) {
            mouse_live_trace(loops, "event-drain-after");
        }
        mouse_live_pause(xhci_only);
        loops++;

        if (mouse_live_deadline_reached(ops, backend, live_start_ns, live_ms, loops, max_loops)) {
            break;
        }
    }
    if (synthetic) {
        (void)kb_usb_synthesize_connected_hid_mouse();
    } else if (!xhci_only) {
        if (kb_usb_root_hub_poll_needed()) {
            (void)kb_usb_poll_root_hubs();
        }
    }
    (void)kb_handle_any_irq_no_work(0);
    (void)kb_input_subsystem_open_registered_devices();
    (void)kb_handle_any_irq_no_work(0);
    printed += drain_mouse_live_events(&x, &y, &left, &printed_details);
    stdout_linef(
        "kobox-usb-hid-mouse-live-summary: events=%u x=%d y=%d left=%d result=%s\n",
        printed,
        x,
        y,
        left,
        printed != 0 ? "ok" : "no-events");
    kb_shim_set_device_backend(NULL);
}

static void cleanup_all_irqs_for_backend(kb_device_backend_t *backend)
{
    if (backend == NULL) {
        return;
    }
    kb_shim_set_device_backend(backend);
    kb_usb_cleanup_tracked_urb_dma();
    kb_free_all_irqs();
    kb_pci_release_all_mmio_mappings();
    kb_shim_set_device_backend(NULL);
}

static int destroy_backend_after_cleanup(kb_device_backend_t *backend)
{
    cleanup_all_irqs_for_backend(backend);
    size_t residuals = 0;
    if (backend != NULL) {
        residuals = kb_pachaos_capsule_report_residuals(backend, stderr, "pre-destroy");
    }
    kb_device_backend_destroy(backend);
    return residuals == 0 ? 0 : 11;
}

static void configure_pachaos_driver_preference(const char *path)
{
#if !defined(_WIN32)
    if (path == NULL || getenv("KOBOX_PACHAOS_PREFERRED_CLASS") != NULL) {
        return;
    }
    if (strstr(path, "nvme") != NULL || strstr(path, "NVME") != NULL) {
        (void)setenv("KOBOX_PACHAOS_PREFERRED_CLASS", "0x010802", 0);
        return;
    }
    if (strstr(path, "xhci") != NULL || strstr(path, "XHCI") != NULL) {
        (void)setenv("KOBOX_PACHAOS_PREFERRED_CLASS", "0x0c0330", 0);
        return;
    }
    if (strstr(path, "ehci") != NULL || strstr(path, "EHCI") != NULL) {
        (void)setenv("KOBOX_PACHAOS_PREFERRED_CLASS", "0x0c0320", 0);
        return;
    }
    if (strstr(path, "ohci") != NULL || strstr(path, "OHCI") != NULL) {
        (void)setenv("KOBOX_PACHAOS_PREFERRED_CLASS", "0x0c0310", 0);
        return;
    }
    if (strstr(path, "uhci") != NULL || strstr(path, "UHCI") != NULL) {
        (void)setenv("KOBOX_PACHAOS_PREFERRED_CLASS", "0x0c0300", 0);
    }
#else
    (void)path;
#endif
}

int main(int argc, char **argv)
{
    install_signal_diagnostics();
    kb_usb_set_event_injection_runtime_allowed(0);

    const char *path = NULL;
    const char *device_backend_name = "mock";
    const char *pci_bdf = NULL;
    const char *capsule_text = NULL;
    const char *dep_paths[KB_RUN_DEPS_MAX];
    size_t dep_count = 0;
    unsigned long drain_ms = 0;
    int argi = 1;
    while (argi < argc && argv[argi][0] == '-') {
        if (strncmp(argv[argi], "--device=", 10) == 0) {
            device_backend_name = argv[argi] + 10;
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
        fprintf(stderr, "usage: kobox-run [--device=mock|vfio|pachaos --pci=<BDF> --capsule=<DeviceCapsule> --dep=<module.ko> --drain-ms=<ms>] run <module.ko>\n");
        return 1;
    }

    if (argi + 1 == argc) {
        path = argv[argi];
    } else if (argi + 2 == argc && strcmp(argv[argi], "run") == 0) {
        path = argv[argi + 1];
    } else {
        fprintf(stderr, "usage: kobox-run [--device=mock|vfio|pachaos --pci=<BDF> --capsule=<DeviceCapsule> --dep=<module.ko> --drain-ms=<ms>] run <module.ko>\n");
        return 1;
    }

    void *data = NULL;
    size_t size = 0;
    kb_status_t status = read_file(path, &data, &size);
    if (status != KB_OK) {
        fprintf(stderr, "read failed: %s (%d)\n", status_name(status), status);
        return 2;
    }

    kb_device_backend_t *backend = NULL;
    if (strcmp(device_backend_name, "mock") == 0) {
        status = kb_linux_mock_device_create(&backend);
    } else if (strcmp(device_backend_name, "vfio") == 0) {
        if (pci_bdf == NULL) {
            free(data);
            fprintf(stderr, "vfio device backend requires --pci=<BDF>\n");
            return 3;
        }
        status = kb_linux_vfio_device_create(pci_bdf, &backend);
    } else if (strcmp(device_backend_name, "pachaos") == 0 || strcmp(device_backend_name, "pachaos_capsule") == 0) {
        (void)setenv("KOBOX_DEVICE_BACKEND", "pachaos", 1);
        configure_pachaos_driver_preference(path);
        if (capsule_text != NULL) {
            uint64_t capsule = 0;
            status = kb_pachaos_capsule_parse_token(capsule_text, &capsule);
            if (status == KB_OK) {
                status = kb_pachaos_capsule_device_create(capsule, &backend);
            }
        } else {
            status = kb_pachaos_capsule_device_create_from_env(&backend);
        }
    } else {
        free(data);
        fprintf(stderr, "unknown device backend: %s\n", device_backend_name);
        return 3;
    }
    if (status != KB_OK) {
        free(data);
        fprintf(stderr, "%s device backend failed: %s (%d)\n", device_backend_name, status_name(status), status);
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
    kb_usb_set_event_injection_runtime_allowed(1);
    if (getenv("KOBOX_NVME_IO_SMOKE") != NULL) {
        kb_shim_set_device_backend(backend);
        int io_result = kb_nvme_io_smoke();
        kb_shim_set_device_backend(NULL);
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
    const char *ext4_image_smoke = getenv("KOBOX_EXT4_IMAGE_SMOKE");
    if (ext4_image_smoke != NULL && ext4_image_smoke[0] != '\0') {
        const char *inode_text = getenv("KOBOX_EXT4_IMAGE_SMOKE_INODE");
        char *inode_end = NULL;
        unsigned long inode_number = inode_text == NULL ? 0 : strtoul(inode_text, &inode_end, 0);
        const char *large_inode_text = getenv("KOBOX_EXT4_IMAGE_SMOKE_LARGE_INODE");
        char *large_inode_end = NULL;
        unsigned long large_inode_number = (large_inode_text == NULL || large_inode_text[0] == '\0') ?
            0 :
            strtoul(large_inode_text, &large_inode_end, 0);
        const char *ldlike_inode_text = getenv("KOBOX_EXT4_IMAGE_SMOKE_LDLIKE_INODE");
        char *ldlike_inode_end = NULL;
        unsigned long ldlike_inode_number = (ldlike_inode_text == NULL || ldlike_inode_text[0] == '\0') ?
            0 :
            strtoul(ldlike_inode_text, &ldlike_inode_end, 0);
        const char *zero_inode_text = getenv("KOBOX_EXT4_IMAGE_SMOKE_ZERO_INODE");
        char *zero_inode_end = NULL;
        unsigned long zero_inode_number = (zero_inode_text == NULL || zero_inode_text[0] == '\0') ?
            0 :
            strtoul(zero_inode_text, &zero_inode_end, 0);
        if (inode_text == NULL || inode_text[0] == '\0' || inode_end == inode_text || *inode_end != '\0' || inode_number == 0) {
            (void)kb_module_call_cleanup(module);
            kb_module_close(module);
            for (size_t i = dep_count; i > 0; i--) {
                (void)kb_module_call_cleanup(deps[i - 1].module);
                kb_module_close(deps[i - 1].module);
                free(deps[i - 1].data);
            }
            destroy_backend_after_cleanup(backend);
            free(data);
            fprintf(stderr, "ext4 image smoke requires KOBOX_EXT4_IMAGE_SMOKE_INODE\n");
            return 10;
        }
        if ((large_inode_text != NULL && large_inode_text[0] != '\0' &&
                (large_inode_end == large_inode_text || *large_inode_end != '\0' || large_inode_number == 0)) ||
            (ldlike_inode_text != NULL && ldlike_inode_text[0] != '\0' &&
                (ldlike_inode_end == ldlike_inode_text || *ldlike_inode_end != '\0' || ldlike_inode_number == 0)) ||
            (zero_inode_text != NULL && zero_inode_text[0] != '\0' &&
                (zero_inode_end == zero_inode_text || *zero_inode_end != '\0' || zero_inode_number == 0)))
        {
            (void)kb_module_call_cleanup(module);
            kb_module_close(module);
            for (size_t i = dep_count; i > 0; i--) {
                (void)kb_module_call_cleanup(deps[i - 1].module);
                kb_module_close(deps[i - 1].module);
                free(deps[i - 1].data);
            }
            destroy_backend_after_cleanup(backend);
            free(data);
            fprintf(stderr, "ext4 image smoke optional inode env is invalid\n");
            return 10;
        }
        kb_loader_set_active_module(module);
        int ext4_smoke_result = kb_fs_subsystem_run_ext4_image_smoke(
            ext4_image_smoke,
            inode_number,
            large_inode_number,
            ldlike_inode_number,
            zero_inode_number);
        kb_loader_set_active_module(NULL);
        if (ext4_smoke_result != 0) {
            (void)kb_module_call_cleanup(module);
            kb_module_close(module);
            for (size_t i = dep_count; i > 0; i--) {
                (void)kb_module_call_cleanup(deps[i - 1].module);
                kb_module_close(deps[i - 1].module);
                free(deps[i - 1].data);
            }
            destroy_backend_after_cleanup(backend);
            free(data);
            fprintf(stderr, "ext4 image smoke failed: %d\n", ext4_smoke_result);
            return 11;
        }
    }
    if (mouse_live_enabled()) {
        run_mouse_live(backend, mouse_live_duration_ms(drain_ms));
    } else {
        drain_after_init(backend, drain_ms);
    }
    const char *usb_hid_mouse_smoke = getenv("KOBOX_USB_HID_MOUSE_SMOKE");
    if (usb_hid_mouse_smoke != NULL && usb_hid_mouse_smoke[0] != '\0' && strcmp(usb_hid_mouse_smoke, "0") != 0) {
        int hid_result = kb_input_subsystem_run_mouse_smoke(stdout);
        if (hid_result != 0) {
            (void)kb_module_call_cleanup(module);
            kb_module_close(module);
            for (size_t i = dep_count; i > 0; i--) {
                (void)kb_module_call_cleanup(deps[i - 1].module);
                kb_module_close(deps[i - 1].module);
                free(deps[i - 1].data);
            }
            destroy_backend_after_cleanup(backend);
            free(data);
            fprintf(stderr, "usb hid mouse smoke failed: %d\n", hid_result);
            return 12;
        }
    }
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
    const char *ata_io_smoke = getenv("KOBOX_ATA_IO_SMOKE");
    if (ata_io_smoke != NULL && ata_io_smoke[0] != '\0' && strcmp(ata_io_smoke, "0") != 0) {
        int ata_io_result = kb_ata_subsystem_run_io_smoke(stdout);
        if (ata_io_result != 0) {
            (void)kb_module_call_cleanup(module);
            kb_module_close(module);
            for (size_t i = dep_count; i > 0; i--) {
                (void)kb_module_call_cleanup(deps[i - 1].module);
                kb_module_close(deps[i - 1].module);
                free(deps[i - 1].data);
            }
            destroy_backend_after_cleanup(backend);
            free(data);
            fprintf(stderr, "ata io smoke failed: %d\n", ata_io_result);
            return 13;
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

    kb_status_t first_cleanup_error = KB_OK;
    int first_cleanup_return = 0;

    status = kb_module_call_cleanup(module);
    if (status == KB_OK) {
        printf("cleanup_module returned\n");
    } else if (status != KB_ERR_NOT_FOUND) {
        fprintf(stderr, "cleanup failed: %s (%d)\n", status_name(status), status);
        first_cleanup_error = status;
        first_cleanup_return = 6;
    }

    kb_module_close(module);
    for (size_t i = dep_count; i > 0; i--) {
        status = kb_module_call_cleanup(deps[i - 1].module);
        if (status == KB_OK) {
            printf("dependency %s cleanup_module returned\n", deps[i - 1].path);
        } else if (status != KB_ERR_NOT_FOUND) {
            fprintf(stderr, "dependency cleanup failed: %s: %s (%d)\n", deps[i - 1].path, status_name(status), status);
            if (first_cleanup_error == KB_OK) {
                first_cleanup_error = status;
                first_cleanup_return = 7;
            }
        }
        kb_module_close(deps[i - 1].module);
        free(deps[i - 1].data);
    }
    int cleanup_result = destroy_backend_after_cleanup(backend);
    free(data);
    if (cleanup_result != 0) {
        fprintf(stderr, "kobox cleanup residuals failed: %d\n", cleanup_result);
        return cleanup_result;
    }
    if (cleanup_failure_code(first_cleanup_error) != 0) {
        return first_cleanup_return;
    }
    return 0;
}
