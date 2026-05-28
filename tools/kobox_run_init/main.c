#include "kobox/backend.h"
#include "kobox/backend_linux_mock.h"
#include "kobox/backend_linux_vfio.h"
#include "kobox/module.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    KB_RUN_DEPS_MAX = 16,
};

typedef struct loaded_input_module {
    const char *path;
    void *data;
    size_t size;
    kb_module_t *module;
} loaded_input_module_t;

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

int main(int argc, char **argv)
{
    const char *path = NULL;
    const char *backend_name = "mock";
    const char *pci_bdf = NULL;
    const char *dep_paths[KB_RUN_DEPS_MAX];
    size_t dep_count = 0;
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
        if (strncmp(argv[argi], "--dep=", 6) == 0) {
            if (dep_count >= KB_RUN_DEPS_MAX) {
                fprintf(stderr, "too many dependencies\n");
                return 1;
            }
            dep_paths[dep_count++] = argv[argi] + 6;
            argi++;
            continue;
        }
        fprintf(stderr, "usage: kobox-run [--backend=mock|vfio --pci=<BDF> --dep=<module.ko>] run <module.ko>\n");
        return 1;
    }

    if (argi + 1 == argc) {
        path = argv[argi];
    } else if (argi + 2 == argc && strcmp(argv[argi], "run") == 0) {
        path = argv[argi + 1];
    } else {
        fprintf(stderr, "usage: kobox-run [--backend=mock|vfio --pci=<BDF> --dep=<module.ko>] run <module.ko>\n");
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
            kb_backend_destroy(backend);
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
            kb_backend_destroy(backend);
            free(data);
            return 4;
        }
        int dep_result = 0;
        status = kb_module_call_init(deps[i].module, &dep_result);
        if (status != KB_OK) {
            fprintf(stderr, "dependency init failed: %s: %s (%d)\n", deps[i].path, status_name(status), status);
            for (size_t j = 0; j <= i; j++) {
                if (deps[j].module != NULL) {
                    kb_module_close(deps[j].module);
                }
                free(deps[j].data);
            }
            kb_backend_destroy(backend);
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
        kb_backend_destroy(backend);
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
        kb_backend_destroy(backend);
        free(data);
        fprintf(stderr, "init failed: %s (%d)\n", status_name(status), status);
        return 5;
    }

    printf("init_module returned %d\n", result);
    status = kb_module_call_cleanup(module);
    if (status == KB_OK) {
        printf("cleanup_module returned\n");
    } else if (status != KB_ERR_NOT_FOUND) {
        kb_module_close(module);
        kb_backend_destroy(backend);
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
            kb_backend_destroy(backend);
            free(data);
            return 7;
        }
        kb_module_close(deps[i - 1].module);
        free(deps[i - 1].data);
    }
    kb_backend_destroy(backend);
    free(data);
    return 0;
}
