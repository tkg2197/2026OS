/* 标准输出和报错机制 */

#include <stdarg.h>
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

// 从低位开始取数字，再倒序输出。
static void printint(struct printbuf *out, uint32 value, int base)
{
    char buf[16];
    int n = 0;

    do {
        buf[n++] = digits[value % base];
        value /= base;
    } while (value);
    while (n > 0)
        put_char(out, buf[--n]);
}

/* %x */
static void printptr(struct printbuf *out, uint64 x)
{
    put_char(out, '0');
    put_char(out, 'x');
    for (int i = 0; i < (sizeof(uint64) * 2); i++, x <<= 4)
        put_char(out, digits[x >> (sizeof(uint64) * 8 - 4)]);
}

// %p 读取指针，%x 读取 64 位整数，两者都输出完整的十六进制地址格式。
static void format(struct printbuf *out, const char *fmt, va_list ap)
{
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            put_char(out, *fmt);
            continue;
        }
        fmt++;
        if (*fmt == '\0') {
            put_char(out, '%');
            break;
        }
        switch (*fmt) {
        case 'd': {
            int n = va_arg(ap, int);
            uint32 magnitude = (uint32)n;
            if (n < 0) {
                put_char(out, '-');
                magnitude = 0u - magnitude;
            }
            printint(out, magnitude, 10);
            break;
        }
        case 'p':
            printptr(out, (uint64)va_arg(ap, void *));
            break;
        case 'x':
            printptr(out, va_arg(ap, uint64));
            break;
        case 'c':
            put_char(out, va_arg(ap, int));
            break;
        case 's': {
            char *s = va_arg(ap, char *);
            if (s == NULL)
                s = "(null)";
            while (*s)
                put_char(out, *s++);
            break;
        }
        case '%':
            put_char(out, '%');
            break;
        default:
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
        const char *prefix = "panic! ";
        while (*prefix)
            uart_putc_emergency(*prefix++);
        if (!s)
            s = "(null)";
        while (*s)
            uart_putc_emergency(*s++);
        uart_putc_emergency('\n');
    }
    for (;;)
        asm volatile("wfi");
}

/* 如果不满足条件, 则调用panic */
void assert(bool condition, const char *warning)
{
    if (!condition) {
        panic(warning);
    }
}
