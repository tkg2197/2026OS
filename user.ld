OUTPUT_ARCH( "riscv" )
ENTRY( main )

PHDRS
{
  text PT_LOAD FLAGS(5);
  data PT_LOAD FLAGS(6);
}

SECTIONS
{
  /* 用户程序从USER_BASE = 0x1000开始, 它下面的一页不建立任何映射 */
  . = 0x1000;

  .text : {
    *(.text.entry)
    *(.text.main)
    *(.text .text.*)
  } :text

  .rodata : {
    . = ALIGN(16);
    *(.srodata .srodata.*)
    . = ALIGN(16);
    *(.rodata .rodata.*)
  } :text

  .data : {
    . = ALIGN(16);
    *(.sdata .sdata.*)
    . = ALIGN(16);
    *(.data .data.*)
  } :data

  .bss : {
    . = ALIGN(16);
    *(.sbss .sbss.*)
    . = ALIGN(16);
    *(.bss .bss.*)
  } :data
}

/* 内核直接把initcode搬运到USER_BASE并从那里开始执行 */
ASSERT(main == 0x1000, "error: main is not located at 0x1000")
/* 内核只为initcode映射一个物理页 */
ASSERT(. <= 0x1000 + 0x1000, "error: initcode is larger than one page")
