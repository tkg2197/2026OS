/* low-level driver routines for 16550a UART. */

#include "mod.h"

// from printf.c 终止输出的标志
extern volatile int panicked;

// uart 初始化
void uart_init(void)
{
  	// 关闭中断
	WriteReg(IER, 0x00);
	// 清零和使能FIFO模式
	WriteReg(FCR, FCR_FIFO_ENABLE | FCR_FIFO_CLEAR);

#ifndef VISIONFIVE2
	// 进入设置比特率的模式
	WriteReg(LCR, LCR_BAUD_LATCH);

	// 设置比特率的低位和高位，最终设置为38.4K
	WriteReg(0, 0x03);
  	WriteReg(1, 0x00);

	// 设置传输字节长度为8bit,不校验
	WriteReg(LCR, LCR_EIGHT_BITS);
#endif
	WriteReg(IER, 0x00);
}

// 打开UART接收中断
void uart_enable_rx_interrupt(void)
{
	WriteReg(IER, IER_RX_ENABLE);
}

// Panic output can run even when the current hart has no valid CPU slot.
void uart_putc_raw(int c)
{
    while ((ReadReg(LSR) & LSR_TX_IDLE) == 0)
        ;
    WriteReg(THR, (uint8)c);
}

void uart_putc_sync(int c)
{
    push_off();
    while (panicked)
        ;
    uart_putc_raw(c);
    pop_off();
}

// 单个字符输入
// 失败返回-1
int uart_getc_sync(void)
{
	if (ReadReg(LSR) & LSR_RX_READY)
		return (uint8)ReadReg(RHR);
	else
		return -1;
}

// 中断处理(键盘输入->屏幕输出)
void uart_intr(void)
{
	while (1)
	{
		int c = uart_getc_sync();
        if (c == -1)
            break;
        if (c == '\b' || c == 0x7f)
            printf("\b \b");
        else if (c == '\r' || c == '\n')
            printf("\r\n");
        else
            printf("%c", c);
	}
}
