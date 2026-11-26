/* 标准输出和报错机制 */

#include <stdarg.h>
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

// 从低位开始取数字，再倒序输出。
static void printint(uint32 value, int base)
{
    char buf[16];
    int n = 0;

    do {
        buf[n++] = digits[value % base];
        value /= base;
    } while (value);
    while (n > 0)
        uart_putc_sync(buf[--n]);
}

/* %x */
static void printptr(uint64 x)
{
    uart_putc_sync('0');
    uart_putc_sync('x');
    for (int i = 0; i < (sizeof(uint64) * 2); i++, x <<= 4)
        uart_putc_sync(digits[x >> (sizeof(uint64) * 8 - 4)]);
}

// %p 读取指针，%x 读取 64 位整数，两者都输出完整的十六进制地址格式。
void printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    spinlock_acquire(&print_lk);
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            uart_putc_sync(*fmt);
            continue;
        }
        fmt++;
        if (*fmt == '\0') {
            uart_putc_sync('%');
            break;
        }
        switch (*fmt) {
        case 'd': {
            int n = va_arg(ap, int);
            uint32 magnitude = (uint32)n;
            if (n < 0) {
                uart_putc_sync('-');
                magnitude = 0u - magnitude;
            }
            printint(magnitude, 10);
            break;
        }
        case 'p':
            printptr((uint64)va_arg(ap, void *));
            break;
        case 'x':
            printptr(va_arg(ap, uint64));
            break;
        case 'c':
            uart_putc_sync(va_arg(ap, int));
            break;
        case 's': {
            char *s = va_arg(ap, char *);
            if (s == NULL)
                s = "(null)";
            while (*s)
                uart_putc_sync(*s++);
            break;
        }
        case '%':
            uart_putc_sync('%');
            break;
        default:
            uart_putc_sync('%');
            uart_putc_sync(*fmt);
            break;
        }
    }
    spinlock_release(&print_lk);
    va_end(ap);
}

/* 如果发生panic, UART的停止标志 */
volatile int panicked = 0;

/* 报错并终止输出 */
void panic(const char *s)
{
    printf("panic! %s\n", s);
    panicked = 1;
    while (1)
        ;
}

/* 如果不满足条件, 则调用panic */
void assert(bool condition, const char *warning)
{
    if (!condition) {
        panic(warning);
    }
}
