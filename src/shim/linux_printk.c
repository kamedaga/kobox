#include "kobox/shim.h"

#include <ctype.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

static int stderr_write_all(const char *text, size_t length)
{
    if (text == NULL || length == 0) {
        return 0;
    }
    size_t written = 0;
    while (written < length) {
#if defined(_WIN32)
        int n = _write(2, text + written, (unsigned int)(length - written));
#else
        ssize_t n = write(STDERR_FILENO, text + written, length - written);
#endif
        if (n <= 0) {
            break;
        }
        written += (size_t)n;
    }
    return (int)written;
}

static const char *printk_format(const char *fmt)
{
    if (fmt == NULL) {
        return "";
    }
    if ((unsigned char)fmt[0] == 1 && fmt[1] >= '0' && fmt[1] <= '7') {
        return fmt + 2;
    }
    return fmt;
}

static int safe_puts_counted(const char *text)
{
    if (text == NULL) {
        return 0;
    }
    return stderr_write_all(text, strlen(text));
}

static const char *safe_string_arg(const char *value)
{
    const uintptr_t ptr = (uintptr_t)value;
    if (value == NULL) {
        return "(null)";
    }
    if (ptr < 4096u) {
        return "(efault)";
    }
    if (ptr >= UINTPTR_MAX - 4095u) {
        return "(errptr)";
    }
    return value;
}

static void append_char(char *buffer, size_t *index, size_t capacity, char value)
{
    if (*index + 1u < capacity) {
        buffer[*index] = value;
        *index += 1u;
        buffer[*index] = '\0';
    }
}

static void append_decimal(char *buffer, size_t *index, size_t capacity, int value)
{
    char temp[32];
    (void)snprintf(temp, sizeof(temp), "%d", value);
    for (size_t i = 0; temp[i] != '\0'; i++) {
        append_char(buffer, index, capacity, temp[i]);
    }
}

typedef enum printk_length {
    PRINTK_LEN_NONE,
    PRINTK_LEN_HH,
    PRINTK_LEN_H,
    PRINTK_LEN_L,
    PRINTK_LEN_LL,
    PRINTK_LEN_Z,
    PRINTK_LEN_T,
    PRINTK_LEN_J,
} printk_length_t;

static int print_signed_value(const char *spec, printk_length_t length, va_list args)
{
    char buffer[128];
    switch (length) {
    case PRINTK_LEN_L:
        (void)snprintf(buffer, sizeof(buffer), spec, va_arg(args, long));
        break;
    case PRINTK_LEN_LL:
        (void)snprintf(buffer, sizeof(buffer), spec, va_arg(args, long long));
        break;
    case PRINTK_LEN_Z:
        (void)snprintf(buffer, sizeof(buffer), spec, va_arg(args, ptrdiff_t));
        break;
    case PRINTK_LEN_T:
        (void)snprintf(buffer, sizeof(buffer), spec, va_arg(args, ptrdiff_t));
        break;
    case PRINTK_LEN_J:
        (void)snprintf(buffer, sizeof(buffer), spec, va_arg(args, intmax_t));
        break;
    case PRINTK_LEN_HH:
    case PRINTK_LEN_H:
    case PRINTK_LEN_NONE:
    default:
        (void)snprintf(buffer, sizeof(buffer), spec, va_arg(args, int));
        break;
    }
    return safe_puts_counted(buffer);
}

static int print_unsigned_value(const char *spec, printk_length_t length, va_list args)
{
    char buffer[128];
    switch (length) {
    case PRINTK_LEN_L:
        (void)snprintf(buffer, sizeof(buffer), spec, va_arg(args, unsigned long));
        break;
    case PRINTK_LEN_LL:
        (void)snprintf(buffer, sizeof(buffer), spec, va_arg(args, unsigned long long));
        break;
    case PRINTK_LEN_Z:
        (void)snprintf(buffer, sizeof(buffer), spec, va_arg(args, size_t));
        break;
    case PRINTK_LEN_T:
        (void)snprintf(buffer, sizeof(buffer), spec, (uintmax_t)va_arg(args, ptrdiff_t));
        break;
    case PRINTK_LEN_J:
        (void)snprintf(buffer, sizeof(buffer), spec, va_arg(args, uintmax_t));
        break;
    case PRINTK_LEN_HH:
    case PRINTK_LEN_H:
    case PRINTK_LEN_NONE:
    default:
        (void)snprintf(buffer, sizeof(buffer), spec, va_arg(args, unsigned int));
        break;
    }
    return safe_puts_counted(buffer);
}

int kb_vprintk_safe(const char *fmt, va_list args)
{
    const char *p = printk_format(fmt);
    int written = 0;

    while (*p != '\0') {
        if (*p != '%') {
            written += stderr_write_all(p, 1);
            p++;
            continue;
        }

        p++;
        if (*p == '%') {
            written += stderr_write_all("%", 1);
            p++;
            continue;
        }

        char spec[64] = "%";
        size_t spec_len = 1;
        while (*p != '\0' && strchr("#0- +'", *p) != NULL) {
            append_char(spec, &spec_len, sizeof(spec), *p++);
        }

        if (*p == '*') {
            append_decimal(spec, &spec_len, sizeof(spec), va_arg(args, int));
            p++;
        } else {
            while (isdigit((unsigned char)*p)) {
                append_char(spec, &spec_len, sizeof(spec), *p++);
            }
        }

        if (*p == '.') {
            append_char(spec, &spec_len, sizeof(spec), *p++);
            if (*p == '*') {
                append_decimal(spec, &spec_len, sizeof(spec), va_arg(args, int));
                p++;
            } else {
                while (isdigit((unsigned char)*p)) {
                    append_char(spec, &spec_len, sizeof(spec), *p++);
                }
            }
        }

        printk_length_t length = PRINTK_LEN_NONE;
        if (*p == 'h' && p[1] == 'h') {
            append_char(spec, &spec_len, sizeof(spec), *p++);
            append_char(spec, &spec_len, sizeof(spec), *p++);
            length = PRINTK_LEN_HH;
        } else if (*p == 'h') {
            append_char(spec, &spec_len, sizeof(spec), *p++);
            length = PRINTK_LEN_H;
        } else if (*p == 'l' && p[1] == 'l') {
            append_char(spec, &spec_len, sizeof(spec), *p++);
            append_char(spec, &spec_len, sizeof(spec), *p++);
            length = PRINTK_LEN_LL;
        } else if (*p == 'l') {
            append_char(spec, &spec_len, sizeof(spec), *p++);
            length = PRINTK_LEN_L;
        } else if (*p == 'z') {
            append_char(spec, &spec_len, sizeof(spec), *p++);
            length = PRINTK_LEN_Z;
        } else if (*p == 't') {
            append_char(spec, &spec_len, sizeof(spec), *p++);
            length = PRINTK_LEN_T;
        } else if (*p == 'j') {
            append_char(spec, &spec_len, sizeof(spec), *p++);
            length = PRINTK_LEN_J;
        }

        const char conversion = *p == '\0' ? '\0' : *p++;
        if (conversion == '\0') {
            break;
        }

        if (conversion == 's') {
            append_char(spec, &spec_len, sizeof(spec), 's');
            written += safe_puts_counted(safe_string_arg(va_arg(args, const char *)));
            continue;
        }
        if (conversion == 'p') {
            void *ptr = va_arg(args, void *);
            while (*p != '\0' && isalnum((unsigned char)*p)) {
                p++;
            }
            char buffer[32];
            (void)snprintf(buffer, sizeof(buffer), "%p", ptr);
            written += safe_puts_counted(buffer);
            continue;
        }
        if (conversion == 'c') {
            append_char(spec, &spec_len, sizeof(spec), 'c');
            char buffer[32];
            (void)snprintf(buffer, sizeof(buffer), spec, va_arg(args, int));
            written += safe_puts_counted(buffer);
            continue;
        }
        if (conversion == 'd' || conversion == 'i') {
            append_char(spec, &spec_len, sizeof(spec), conversion);
            written += print_signed_value(spec, length, args);
            continue;
        }
        if (conversion == 'u' || conversion == 'x' || conversion == 'X' || conversion == 'o') {
            append_char(spec, &spec_len, sizeof(spec), conversion);
            written += print_unsigned_value(spec, length, args);
            continue;
        }
        if (conversion == 'n') {
            (void)va_arg(args, void *);
            continue;
        }

        written += stderr_write_all("%", 1);
        written += stderr_write_all(&conversion, 1);
    }

    return written;
}

int kb_printk(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int written = kb_vprintk_safe(fmt, args);
    va_end(args);
    return written;
}

int printk(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int written = kb_vprintk_safe(fmt, args);
    va_end(args);
    return written;
}
