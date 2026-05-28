#include "kobox/shim.h"

#include <stdarg.h>
#include <stdio.h>

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

static int kb_vprintk(const char *fmt, va_list args)
{
    return vfprintf(stderr, printk_format(fmt), args);
}

int kb_printk(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int written = kb_vprintk(fmt, args);
    va_end(args);
    return written;
}

int printk(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int written = kb_vprintk(fmt, args);
    va_end(args);
    return written;
}
