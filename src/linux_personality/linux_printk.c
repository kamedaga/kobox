#include "kobox/shim.h"

#include <ctype.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
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

static int printk_loglevel(const char *fmt)
{
    if (fmt != NULL && (unsigned char)fmt[0] == 1 && fmt[1] >= '0' && fmt[1] <= '7') {
        return fmt[1] - '0';
    }
    return -1;
}

static int printk_trace_enabled(void)
{
    const char *value = getenv("KOBOX_TRACE_PRINTK");
    return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
}

static int routine_printk_message(const char *text)
{
    if (text == NULL) {
        return 0;
    }
    if ((unsigned char)text[0] == 1 && text[1] >= '5' && text[1] <= '7') {
        return 1;
    }
    return strncmp(text, "pci function ", 13) == 0 ||
        strstr(text, " default/read/poll queues") != NULL ||
        strncmp(text, "Ignoring bogus Namespace Identifiers", 36) == 0;
}

static size_t safe_bounded_strlen(const char *text, size_t limit)
{
    size_t length = 0;
    if (text == NULL) {
        return 0;
    }
    while (length < limit && text[length] != '\0') {
        length++;
    }
    return length;
}

static const char *safe_string_arg(const char *value);

typedef struct safe_snprintf_buffer {
    char *buf;
    size_t size;
    size_t used;
    int total;
} safe_snprintf_buffer_t;

static void safe_buffer_write(safe_snprintf_buffer_t *out, const char *text, size_t length)
{
    if (out == NULL || text == NULL) {
        return;
    }
    if (out->buf != NULL && out->size != 0 && out->used + 1u < out->size) {
        size_t room = out->size - out->used - 1u;
        size_t copied = length < room ? length : room;
        memcpy(out->buf + out->used, text, copied);
        out->used += copied;
        out->buf[out->used] = '\0';
    }
    if (length > (size_t)(INT32_MAX - out->total)) {
        out->total = INT32_MAX;
    } else {
        out->total += (int)length;
    }
}

static void safe_buffer_puts(safe_snprintf_buffer_t *out, const char *text)
{
    if (text == NULL) {
        text = "";
    }
    safe_buffer_write(out, text, safe_bounded_strlen(text, 512));
}

static void safe_buffer_putc(safe_snprintf_buffer_t *out, char value)
{
    safe_buffer_write(out, &value, 1);
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

typedef struct printk_format_options {
    int left;
    int plus;
    int space;
    int alternate;
    int zero;
    int width;
    int precision;
    int precision_set;
} printk_format_options_t;

static int parse_decimal_field(const char **cursor)
{
    int value = 0;
    while (isdigit((unsigned char)**cursor)) {
        int digit = **cursor - '0';
        if (value <= (INT32_MAX - digit) / 10) {
            value = value * 10 + digit;
        }
        *cursor += 1;
    }
    return value;
}

static void safe_buffer_repeat(safe_snprintf_buffer_t *out, char value, int count)
{
    for (int i = 0; i < count; i++) {
        safe_buffer_putc(out, value);
    }
}

static size_t format_unsigned_digits(char *digits, size_t capacity, uintmax_t value, unsigned int base, int uppercase)
{
    static const char lower[] = "0123456789abcdef";
    static const char upper[] = "0123456789ABCDEF";
    const char *alphabet = uppercase ? upper : lower;
    char reverse[sizeof(uintmax_t) * 8u + 1u];
    size_t length = 0;

    if (base < 2u || base > 16u || capacity == 0) {
        return 0;
    }
    if (value == 0) {
        reverse[length++] = '0';
    } else {
        while (value != 0 && length < sizeof(reverse)) {
            reverse[length++] = alphabet[value % base];
            value /= base;
        }
    }

    size_t written = 0;
    while (written < length && written + 1u < capacity) {
        digits[written] = reverse[length - written - 1u];
        written++;
    }
    if (written < capacity) {
        digits[written] = '\0';
    }
    return written;
}

static uintmax_t signed_magnitude(intmax_t value, int *negative)
{
    if (value < 0) {
        *negative = 1;
        return (uintmax_t)(-(value + 1)) + 1u;
    }
    *negative = 0;
    return (uintmax_t)value;
}

static void safe_buffer_put_number(
    safe_snprintf_buffer_t *out,
    uintmax_t magnitude,
    int negative,
    unsigned int base,
    int uppercase,
    int is_signed,
    const printk_format_options_t *options)
{
    char digits[sizeof(uintmax_t) * 8u + 1u];
    size_t digits_len = format_unsigned_digits(digits, sizeof(digits), magnitude, base, uppercase);
    if (options->precision_set && options->precision == 0 && magnitude == 0) {
        digits[0] = '\0';
        digits_len = 0;
    }

    char prefix[3];
    size_t prefix_len = 0;
    if (is_signed && negative) {
        prefix[prefix_len++] = '-';
    } else if (is_signed && options->plus) {
        prefix[prefix_len++] = '+';
    } else if (is_signed && options->space) {
        prefix[prefix_len++] = ' ';
    } else if (options->alternate && base == 8u && (digits_len == 0 || digits[0] != '0')) {
        prefix[prefix_len++] = '0';
    } else if (options->alternate && base == 16u && magnitude != 0) {
        prefix[prefix_len++] = '0';
        prefix[prefix_len++] = uppercase ? 'X' : 'x';
    }

    int precision_zeroes = 0;
    if (options->precision_set && options->precision > (int)digits_len) {
        precision_zeroes = options->precision - (int)digits_len;
    }

    int width_padding = 0;
    int content_width = (int)prefix_len + precision_zeroes + (int)digits_len;
    if (options->width > content_width) {
        width_padding = options->width - content_width;
    }

    char pad_char = (options->zero && !options->left && !options->precision_set) ? '0' : ' ';
    if (!options->left && pad_char == ' ') {
        safe_buffer_repeat(out, ' ', width_padding);
    }
    safe_buffer_write(out, prefix, prefix_len);
    if (!options->left && pad_char == '0') {
        safe_buffer_repeat(out, '0', width_padding);
    }
    safe_buffer_repeat(out, '0', precision_zeroes);
    safe_buffer_write(out, digits, digits_len);
    if (options->left) {
        safe_buffer_repeat(out, ' ', width_padding);
    }
}

static void safe_buffer_put_pointer(safe_snprintf_buffer_t *out, const void *ptr, const printk_format_options_t *options)
{
    printk_format_options_t pointer_options = *options;
    pointer_options.alternate = 1;
    safe_buffer_put_number(out, (uintptr_t)ptr, 0, 16u, 0, 0, &pointer_options);
}

int KB_STACK_REALIGN kb_vsnprintf_safe(char *buf, size_t size, const char *fmt, va_list args)
{
    safe_snprintf_buffer_t out = {
        .buf = buf,
        .size = size,
        .used = 0,
        .total = 0,
    };
    if (size != 0 && buf != NULL) {
        buf[0] = '\0';
    }

    const char *p = printk_format(fmt);
    while (*p != '\0') {
        if (*p != '%') {
            safe_buffer_putc(&out, *p++);
            continue;
        }

        p++;
        if (*p == '%') {
            safe_buffer_putc(&out, '%');
            p++;
            continue;
        }

        printk_format_options_t options = {0};
        while (*p != '\0' && strchr("#0- +'", *p) != NULL) {
            switch (*p++) {
            case '#':
                options.alternate = 1;
                break;
            case '0':
                options.zero = 1;
                break;
            case '-':
                options.left = 1;
                break;
            case '+':
                options.plus = 1;
                break;
            case ' ':
                options.space = 1;
                break;
            default:
                break;
            }
        }
        if (*p == '*') {
            options.width = va_arg(args, int);
            if (options.width < 0) {
                options.left = 1;
                options.width = -options.width;
            }
            p++;
        } else {
            options.width = parse_decimal_field(&p);
        }
        if (*p == '.') {
            p++;
            options.precision_set = 1;
            if (*p == '*') {
                options.precision = va_arg(args, int);
                if (options.precision < 0) {
                    options.precision = 0;
                    options.precision_set = 0;
                }
                p++;
            } else {
                options.precision = parse_decimal_field(&p);
            }
        }

        printk_length_t length = PRINTK_LEN_NONE;
        if (*p == 'h' && p[1] == 'h') {
            p += 2;
            length = PRINTK_LEN_HH;
        } else if (*p == 'h') {
            p++;
            length = PRINTK_LEN_H;
        } else if (*p == 'l' && p[1] == 'l') {
            p += 2;
            length = PRINTK_LEN_LL;
        } else if (*p == 'l') {
            p++;
            length = PRINTK_LEN_L;
        } else if (*p == 'z') {
            p++;
            length = PRINTK_LEN_Z;
        } else if (*p == 't') {
            p++;
            length = PRINTK_LEN_T;
        } else if (*p == 'j') {
            p++;
            length = PRINTK_LEN_J;
        }

        char conversion = *p == '\0' ? '\0' : *p++;
        if (conversion == '\0') {
            break;
        }
        if (conversion == 's') {
            if (length == PRINTK_LEN_L) {
                (void)va_arg(args, const void *);
                safe_buffer_puts(&out, "(wstr)");
            } else {
                safe_buffer_puts(&out, safe_string_arg(va_arg(args, const char *)));
            }
            continue;
        }
        if (conversion == 'S') {
            (void)va_arg(args, const void *);
            safe_buffer_puts(&out, "(wstr)");
            continue;
        }
        if (conversion == 'p') {
            void *ptr = va_arg(args, void *);
            while (*p != '\0' && isalnum((unsigned char)*p)) {
                p++;
            }
            safe_buffer_put_pointer(&out, ptr, &options);
            continue;
        }
        if (conversion == 'c' || conversion == 'C') {
            int value = va_arg(args, int);
            char ch = (value >= 32 && value < 127) ? (char)value : '?';
            safe_buffer_putc(&out, ch);
            continue;
        }
        if (conversion == 'd' || conversion == 'i') {
            intmax_t value;
            if (length == PRINTK_LEN_L) {
                value = va_arg(args, long);
            } else if (length == PRINTK_LEN_LL) {
                value = va_arg(args, long long);
            } else if (length == PRINTK_LEN_Z || length == PRINTK_LEN_T) {
                value = va_arg(args, ptrdiff_t);
            } else if (length == PRINTK_LEN_J) {
                value = va_arg(args, intmax_t);
            } else {
                value = va_arg(args, int);
            }
            int negative = 0;
            uintmax_t magnitude = signed_magnitude(value, &negative);
            safe_buffer_put_number(&out, magnitude, negative, 10u, 0, 1, &options);
            continue;
        }
        if (conversion == 'u' || conversion == 'x' || conversion == 'X' || conversion == 'o') {
            uintmax_t value;
            if (length == PRINTK_LEN_L) {
                value = va_arg(args, unsigned long);
            } else if (length == PRINTK_LEN_LL) {
                value = va_arg(args, unsigned long long);
            } else if (length == PRINTK_LEN_Z) {
                value = va_arg(args, size_t);
            } else if (length == PRINTK_LEN_T) {
                value = (uintmax_t)va_arg(args, ptrdiff_t);
            } else if (length == PRINTK_LEN_J) {
                value = va_arg(args, uintmax_t);
            } else {
                value = va_arg(args, unsigned int);
            }
            unsigned int base = conversion == 'o' ? 8u : (conversion == 'u' ? 10u : 16u);
            safe_buffer_put_number(&out, value, 0, base, conversion == 'X', 0, &options);
            continue;
        }
        if (conversion == 'n') {
            (void)va_arg(args, void *);
            continue;
        }
        safe_buffer_putc(&out, '%');
        safe_buffer_putc(&out, conversion);
    }
    if (size != 0 && buf != NULL) {
        size_t index = out.used < size ? out.used : size - 1u;
        buf[index] = '\0';
    }
    return out.total;
}

int KB_STACK_REALIGN kb_vprintk_safe(const char *fmt, va_list args)
{
    char buffer[4096];
    va_list copy;
    va_copy(copy, args);
    int needed = kb_vsnprintf_safe(buffer, sizeof(buffer), fmt, copy);
    va_end(copy);

    int level = printk_loglevel(fmt);
    if (!printk_trace_enabled() && (level >= 5 || routine_printk_message(buffer))) {
        return needed;
    }

    size_t length = safe_bounded_strlen(buffer, sizeof(buffer));
    (void)stderr_write_all(buffer, length);
    return needed;
}

static int KB_STACK_REALIGN kb_vprintk_filtered(const char *fmt, va_list args)
{
    char buffer[4096];
    va_list copy;
    va_copy(copy, args);
    int needed = kb_vsnprintf_safe(buffer, sizeof(buffer), fmt, copy);
    va_end(copy);

    int level = printk_loglevel(fmt);
    if (!printk_trace_enabled() && (level >= 5 || routine_printk_message(buffer))) {
        return needed;
    }

    size_t length = safe_bounded_strlen(buffer, sizeof(buffer));
    (void)stderr_write_all(buffer, length);
    return needed;
}

int KB_STACK_REALIGN kb_snprintf_safe(char *buf, size_t size, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int result = kb_vsnprintf_safe(buf, size, fmt, args);
    va_end(args);
    return result;
}

int KB_STACK_REALIGN kb_sprintf_safe(char *buf, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int result = kb_vsnprintf_safe(buf, 4096, fmt, args);
    va_end(args);
    return result;
}

int KB_STACK_REALIGN kb_printk(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int written = kb_vprintk_filtered(fmt, args);
    va_end(args);
    return written;
}

int KB_STACK_REALIGN kb_tracef(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int written = kb_vprintk_safe(fmt, args);
    va_end(args);
    return written;
}

int KB_STACK_REALIGN printk(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int written = kb_vprintk_filtered(fmt, args);
    va_end(args);
    return written;
}
