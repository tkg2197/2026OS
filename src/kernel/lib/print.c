/* 标准输出和报错机制 */

#include "mod.h"

static const char digits[] = "0123456789abcdef";

/* printf的自旋锁 */
static spinlock_t print_lk;

/* 初始化uart + 初始化printf锁 */
void print_init(void)
{
    uart_init();
    spinlock_init(&print_lk, "printf");
}

struct printbuf {
    char data[512];
    unsigned int len;
    bool locked;
};

// 短消息格式化完成后才加锁；长消息从第一次刷出到最后一个字节都持锁。
static void flush(struct printbuf *out)
{
    if (!out->locked) {
        spinlock_acquire(&print_lk);
        out->locked = true;
    }
    for (unsigned int i = 0; i < out->len; i++)
        uart_putc_raw(out->data[i]);
    out->len = 0;
}

static void put_char(struct printbuf *out, char c)
{
    if (out->len == sizeof(out->data))
        flush(out);
    out->data[out->len++] = c;
}

static void print_unsigned(struct printbuf *out, uint64 value, unsigned int base)
{
    char buf[32];
    int i = 0;
    do
    {
        buf[i++] = digits[value % base];
    } while ((value /= base) != 0);

    while (--i >= 0)
        put_char(out, buf[i]);
}

static void printint(struct printbuf *out, int value)
{
    if (value < 0)
        put_char(out, '-');
    print_unsigned(out, value < 0 ? 0u - (uint32)value : (uint32)value, 10);
}

/* %x */
static void printptr(struct printbuf *out, uint64 x)
{
    put_char(out, '0');
    put_char(out, 'x');
    for (int i = 0; i < (sizeof(uint64) * 2); i++, x <<= 4)
        put_char(out, digits[x >> (sizeof(uint64) * 8 - 4)]);
}

// Decode the binary64 argument using integer operations. Trap paths do
// not enable the floating-point unit or save floating-point registers.
static void printfloat(struct printbuf *out, double value)
{
    union { double value; uint64 bits; } number = { .value = value };
    uint64 significand = number.bits & ((1ull << 52) - 1);
    int exponent = (number.bits >> 52) & 0x7ff;
    if (number.bits >> 63)
        put_char(out, '-');
    if (exponent)
        significand |= 1ull << 52;
    int shift = 1075 - (exponent ? exponent : 1);
    uint64 whole = 0;
    __uint128_t fraction = 0;

    if (shift <= 0)
        whole = significand << -shift;
    else if (shift < 128)
    {
        whole = (__uint128_t)significand >> shift;
        fraction = significand - ((__uint128_t)whole << shift);
    }

    print_unsigned(out, whole, 10);
    put_char(out, '.');
    for (int i = 0; i < 6; i++)
    {
        unsigned int digit = 0;
        if (fraction)
        {
            fraction *= 10;
            digit = fraction >> shift;
            fraction -= (__uint128_t)digit << shift;
        }
        put_char(out, '0' + digit);
    }
}

static void format(struct printbuf *out, const char *fmt, va_list ap)
{
    for (; *fmt; fmt++)
    {
        if (*fmt != '%')
        {
            put_char(out, *fmt);
            continue;
        }
        if (!*++fmt)
            break;
        switch (*fmt)
        {
        case 'd':
            printint(out, va_arg(ap, int));
            break;
        case 'x':
            printptr(out, va_arg(ap, uint64));
            break;
        case 'p':
            print_unsigned(out, va_arg(ap, uint32), 16);
            break;
        case 'f':
            printfloat(out, va_arg(ap, double));
            break;
        case 'c':
            put_char(out, va_arg(ap, int));
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            for (s = s ? s : "(null)"; *s; s++)
                put_char(out, *s);
            break;
        }
        default:
            if (*fmt != '%')
                put_char(out, '%');
            put_char(out, *fmt);
            break;
        }
    }
}

void printf(const char *fmt, ...)
{
    struct printbuf out;
    out.len = 0;
    out.locked = false;

    va_list ap;
    va_start(ap, fmt);
    format(&out, fmt, ap);
    va_end(ap);

    flush(&out);
    spinlock_release(&print_lk);
}

int panicked;

void panic(const char *s)
{
    intr_off();
    if (__atomic_exchange_n(&panicked, 1, __ATOMIC_RELAXED) == 0) {
        const char *prefix = "panic: ";
        while (*prefix)
            uart_putc_emergency(*prefix++);
        while (s && *s)
            uart_putc_emergency(*s++);
        uart_putc_emergency('\n');
    }
    for (;;)
        asm volatile("wfi");
}

/* 如果不满足条件, 则调用panic */
void assert(bool condition, const char *warning)
{
    if (!condition)
        panic(warning);
}
