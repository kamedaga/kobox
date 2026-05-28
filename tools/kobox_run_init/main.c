#include "kobox/backend.h"
#include "kobox/backend_linux_mock.h"
#include "kobox/module.h"

#include <stdio.h>
#include <stdlib.h>

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
    if (argc == 2) {
        path = argv[1];
    } else if (argc == 3 && argv[1][0] == 'r' && argv[1][1] == 'u' && argv[1][2] == 'n' && argv[1][3] == '\0') {
        path = argv[2];
    } else {
        fprintf(stderr, "usage: kobox-run run <module.ko>\n");
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
    status = kb_linux_mock_create(&backend);
    if (status != KB_OK) {
        free(data);
        fprintf(stderr, "backend failed: %s (%d)\n", status_name(status), status);
        return 3;
    }

    kb_module_image_t image = {
        .data = data,
        .size = size,
        .name = path,
    };
    kb_module_t *module = NULL;
    status = kb_module_open_image(&image, backend, &module);
    if (status != KB_OK) {
        kb_backend_destroy(backend);
        free(data);
        fprintf(stderr, "open failed: %s (%d)\n", status_name(status), status);
        return 4;
    }

    int result = 0;
    status = kb_module_call_init(module, &result);
    if (status != KB_OK) {
        kb_module_close(module);
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
    kb_backend_destroy(backend);
    free(data);
    return 0;
}
