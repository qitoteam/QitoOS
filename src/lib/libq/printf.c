/*
 * QitoOS - printf family
 *
 * A compact but reasonably complete formatter supporting the conversions the
 * kernel and the shells actually use:
 *
 *   %c %s %d %i %u %x %X %o %b %p %%
 *   flags:  - + space 0 #
 *   width:  number or *
 *   precision: .number or .*
 *   length: hh h l ll z
 */

#include <kernel/printf.h>
#include <kernel/string.h>

struct sink {
    putc_fn emit;
    void   *ctx;
    int     count;
};

static void sink_putc(struct sink *s, char c)
{
    s->emit(s->ctx, c);
    s->count++;
}

static void sink_pad(struct sink *s, char pad, int count)
{
    while (count-- > 0) {
        sink_putc(s, pad);
    }
}

#define FLAG_LEFT   0x01
#define FLAG_PLUS   0x02
#define FLAG_SPACE  0x04
#define FLAG_ZERO   0x08
#define FLAG_ALT    0x10

static const char *DIGITS_LOWER = "0123456789abcdef";
static const char *DIGITS_UPPER = "0123456789ABCDEF";

/*
 * Render an unsigned value into `buf` (written backwards) and return the
 * number of digits produced.
 */
static int format_uint(char *buf, uint64_t value, unsigned base, const char *digits)
{
    int len = 0;

    if (value == 0) {
        buf[len++] = '0';
        return len;
    }
    while (value) {
        buf[len++] = digits[value % base];
        value /= base;
    }
    return len;
}

static void emit_number(struct sink *s, uint64_t value, unsigned base, int negative,
                        int flags, int width, int precision, const char *digits)
{
    char digit_buf[72];
    int  digit_len = format_uint(digit_buf, value, base, digits);

    /* A precision of 0 with a zero value produces no digits at all. */
    if (precision == 0 && value == 0) {
        digit_len = 0;
    }

    int zeros = (precision > digit_len) ? precision - digit_len : 0;

    char sign = 0;
    if (negative) {
        sign = '-';
    } else if (flags & FLAG_PLUS) {
        sign = '+';
    } else if (flags & FLAG_SPACE) {
        sign = ' ';
    }

    char prefix[2] = {0, 0};
    int  prefix_len = 0;
    if (flags & FLAG_ALT) {
        if (base == 16) {
            prefix[0]  = '0';
            prefix[1]  = (digits == DIGITS_UPPER) ? 'X' : 'x';
            prefix_len = 2;
        } else if (base == 8 && zeros == 0 && digit_len > 0 &&
                   digit_buf[digit_len - 1] != '0') {
            prefix[0]  = '0';
            prefix_len = 1;
        } else if (base == 2) {
            prefix[0]  = '0';
            prefix[1]  = 'b';
            prefix_len = 2;
        }
    }

    int body = digit_len + zeros + (sign ? 1 : 0) + prefix_len;
    int pad  = (width > body) ? width - body : 0;

    /* Zero padding is ignored when the value is left aligned or a precision
     * was given for an integer conversion. */
    if ((flags & FLAG_ZERO) && !(flags & FLAG_LEFT) && precision < 0) {
        zeros += pad;
        pad = 0;
    }

    if (!(flags & FLAG_LEFT)) {
        sink_pad(s, ' ', pad);
    }
    if (sign) {
        sink_putc(s, sign);
    }
    for (int i = 0; i < prefix_len; i++) {
        sink_putc(s, prefix[i]);
    }
    sink_pad(s, '0', zeros);
    while (digit_len-- > 0) {
        sink_putc(s, digit_buf[digit_len]);
    }
    if (flags & FLAG_LEFT) {
        sink_pad(s, ' ', pad);
    }
}

static void emit_string(struct sink *s, const char *str, int flags, int width,
                        int precision)
{
    if (str == NULL) {
        str = "(null)";
    }
    int len = (int)((precision >= 0) ? strnlen(str, (size_t)precision) : strlen(str));
    int pad = (width > len) ? width - len : 0;

    if (!(flags & FLAG_LEFT)) {
        sink_pad(s, ' ', pad);
    }
    for (int i = 0; i < len; i++) {
        sink_putc(s, str[i]);
    }
    if (flags & FLAG_LEFT) {
        sink_pad(s, ' ', pad);
    }
}

int vcbprintf(putc_fn emit, void *ctx, const char *fmt, va_list ap)
{
    struct sink s = {emit, ctx, 0};

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            sink_putc(&s, *fmt);
            continue;
        }
        fmt++;
        if (*fmt == '\0') {
            break;
        }
        if (*fmt == '%') {
            sink_putc(&s, '%');
            continue;
        }

        /* Flags */
        int flags = 0;
        for (;; fmt++) {
            if (*fmt == '-') {
                flags |= FLAG_LEFT;
            } else if (*fmt == '+') {
                flags |= FLAG_PLUS;
            } else if (*fmt == ' ') {
                flags |= FLAG_SPACE;
            } else if (*fmt == '0') {
                flags |= FLAG_ZERO;
            } else if (*fmt == '#') {
                flags |= FLAG_ALT;
            } else {
                break;
            }
        }

        /* Width */
        int width = 0;
        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0) {
                flags |= FLAG_LEFT;
                width = -width;
            }
            fmt++;
        } else {
            while (isdigit((uint8_t)*fmt)) {
                width = width * 10 + (*fmt++ - '0');
            }
        }

        /* Precision */
        int precision = -1;
        if (*fmt == '.') {
            fmt++;
            precision = 0;
            if (*fmt == '*') {
                precision = va_arg(ap, int);
                fmt++;
            } else {
                while (isdigit((uint8_t)*fmt)) {
                    precision = precision * 10 + (*fmt++ - '0');
                }
            }
        }

        /* Length modifiers */
        enum { LEN_INT, LEN_CHAR, LEN_SHORT, LEN_LONG, LEN_LLONG, LEN_SIZE } len =
            LEN_INT;
        if (fmt[0] == 'h' && fmt[1] == 'h') {
            len = LEN_CHAR;
            fmt += 2;
        } else if (fmt[0] == 'h') {
            len = LEN_SHORT;
            fmt += 1;
        } else if (fmt[0] == 'l' && fmt[1] == 'l') {
            len = LEN_LLONG;
            fmt += 2;
        } else if (fmt[0] == 'l') {
            len = LEN_LONG;
            fmt += 1;
        } else if (fmt[0] == 'z') {
            len = LEN_SIZE;
            fmt += 1;
        }

        switch (*fmt) {
        case 'c': {
            char c   = (char)va_arg(ap, int);
            int  pad = (width > 1) ? width - 1 : 0;
            if (!(flags & FLAG_LEFT)) {
                sink_pad(&s, ' ', pad);
            }
            sink_putc(&s, c);
            if (flags & FLAG_LEFT) {
                sink_pad(&s, ' ', pad);
            }
            break;
        }
        case 's':
            emit_string(&s, va_arg(ap, const char *), flags, width, precision);
            break;
        case 'd':
        case 'i': {
            int64_t v;
            switch (len) {
            case LEN_CHAR:  v = (signed char)va_arg(ap, int);  break;
            case LEN_SHORT: v = (short)va_arg(ap, int);        break;
            case LEN_LONG:  v = va_arg(ap, long);              break;
            case LEN_LLONG: v = va_arg(ap, long long);         break;
            case LEN_SIZE:  v = (int64_t)va_arg(ap, size_t);   break;
            default:        v = va_arg(ap, int);               break;
            }
            uint64_t mag = (v < 0) ? (uint64_t)(-(v + 1)) + 1u : (uint64_t)v;
            emit_number(&s, mag, 10, v < 0, flags, width, precision, DIGITS_LOWER);
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        case 'o':
        case 'b': {
            uint64_t v;
            switch (len) {
            case LEN_CHAR:  v = (unsigned char)va_arg(ap, unsigned int);  break;
            case LEN_SHORT: v = (unsigned short)va_arg(ap, unsigned int); break;
            case LEN_LONG:  v = va_arg(ap, unsigned long);                break;
            case LEN_LLONG: v = va_arg(ap, unsigned long long);           break;
            case LEN_SIZE:  v = va_arg(ap, size_t);                       break;
            default:        v = va_arg(ap, unsigned int);                 break;
            }
            unsigned    base   = 10;
            const char *digits = DIGITS_LOWER;
            if (*fmt == 'x') {
                base = 16;
            } else if (*fmt == 'X') {
                base   = 16;
                digits = DIGITS_UPPER;
            } else if (*fmt == 'o') {
                base = 8;
            } else if (*fmt == 'b') {
                base = 2;
            }
            emit_number(&s, v, base, 0, flags, width, precision, digits);
            break;
        }
        case 'p': {
            void *ptr = va_arg(ap, void *);
            sink_putc(&s, '0');
            sink_putc(&s, 'x');
            emit_number(&s, (uint64_t)(uintptr_t)ptr, 16, 0, FLAG_ZERO, 16, -1,
                        DIGITS_LOWER);
            break;
        }
        default:
            sink_putc(&s, '%');
            sink_putc(&s, *fmt);
            break;
        }
    }

    return s.count;
}

struct buf_sink {
    char  *buf;
    size_t size;
    size_t pos;
};

static void buf_emit(void *ctx, char c)
{
    struct buf_sink *b = (struct buf_sink *)ctx;
    if (b->pos + 1 < b->size) {
        b->buf[b->pos] = c;
    }
    b->pos++;
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    struct buf_sink sink = {buf, size, 0};
    int             n    = vcbprintf(buf_emit, &sink, fmt, ap);

    if (size) {
        size_t end = (sink.pos < size - 1) ? sink.pos : size - 1;
        buf[end]   = '\0';
    }
    return n;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}
