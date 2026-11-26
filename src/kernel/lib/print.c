/* 标准输出和报错机制 */

#include "mod.h"

static char digits[] = "0123456789abcdef";

/* printf的自旋锁 */
static spinlock_t print_lk;

/* 初始化uart + 初始化printf锁 */
void print_init(void)
{
    uart_init();
    spinlock_init(&print_lk, "printf");
}

static void print_unsigned(uint64 value, unsigned int base)
{
    char buf[32];
    int i = 0;
    do
    {
        buf[i++] = digits[value % base];
    } while ((value /= base) != 0);

    while (--i >= 0)
        uart_putc_sync(buf[i]);
}

static void printint(int value)
{
    if (value < 0)
        uart_putc_sync('-');
    print_unsigned(value < 0 ? 0u - (uint32)value : (uint32)value, 10);
}

/* %x */
static void printptr(uint64 x)
{
    uart_putc_sync('0');
    uart_putc_sync('x');
    for (int i = 0; i < (sizeof(uint64) * 2); i++, x <<= 4)
        uart_putc_sync(digits[x >> (sizeof(uint64) * 8 - 4)]);
}

// Decode the binary64 argument using integer operations. Trap paths do
// not enable the floating-point unit or save floating-point registers.
static void printfloat(double value)
{
    union { double value; uint64 bits; } number = { .value = value };
    uint64 significand = number.bits & ((1ull << 52) - 1);
    int exponent = (number.bits >> 52) & 0x7ff;
    if (number.bits >> 63)
        uart_putc_sync('-');
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

    print_unsigned(whole, 10);
    uart_putc_sync('.');
    for (int i = 0; i < 6; i++)
    {
        unsigned int digit = 0;
        if (fraction)
        {
            fraction *= 10;
            digit = fraction >> shift;
            fraction -= (__uint128_t)digit << shift;
        }
        uart_putc_sync('0' + digit);
    }
}

static void vprint(const char *fmt, va_list ap)
{
    for (; *fmt; fmt++)
    {
        if (*fmt != '%')
        {
            uart_putc_sync(*fmt);
            continue;
        }
        if (!*++fmt)
            break;
        switch (*fmt)
        {
        case 'd':
            printint(va_arg(ap, int));
            break;
        case 'x':
            printptr(va_arg(ap, uint64));
            break;
        case 'p':
            print_unsigned(va_arg(ap, uint32), 16);
            break;
        case 'f':
            printfloat(va_arg(ap, double));
            break;
        case 'c':
            uart_putc_sync(va_arg(ap, int));
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            for (s = s ? s : "(null)"; *s; s++)
                uart_putc_sync(*s);
            break;
        }
        default:
            if (*fmt != '%')
                uart_putc_sync('%');
            uart_putc_sync(*fmt);
            break;
        }
    }
}

void printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    spinlock_acquire(&print_lk);
    vprint(fmt, ap);
    spinlock_release(&print_lk);
    va_end(ap);
}

/* 如果发生panic, UART的停止标志 */
volatile int panicked = 0;

/* 报错并终止输出 */
void panic(const char *s)
{
    intr_off();
    // Report invariants even when the current hart already holds print_lk.
    const char *prefix = "panic: ";
    while (*prefix)
        uart_putc_raw(*prefix++);
    while (s && *s)
        uart_putc_raw(*s++);
    uart_putc_raw('\n');
    panicked = 1;
    while (1)
        ;
}

/* 如果不满足条件, 则调用panic */
void assert(bool condition, const char *warning)
{
    if (!condition)
        panic(warning);
}
