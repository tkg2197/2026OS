OUTPUT_ARCH( "riscv" )
ENTRY( _entry )

PHDRS
{
  text PT_LOAD FLAGS(5); /* read + execute */
  data PT_LOAD FLAGS(6); /* read + write */
}

SECTIONS
{
  /* 入口须与当前平台 KERNEL_BASE 一致, 开发板数字请查阅手册或DTB。 */
#ifdef VISIONFIVE2
  . = 0x40200000;
#else
  . = 0x80200000;
#endif

  .text : {
    KEEP(*(.text.entry))
    *(.text .text.*)
    . = ALIGN(0x1000);
    _trampoline = .;
    *(trampsec)
    . = ALIGN(0x1000);
    ASSERT(. - _trampoline == 0x1000, "error: trampoline larger than one page");
    PROVIDE(KERNEL_DATA = .);
  } :text

  .rodata : {
    . = ALIGN(16);
    *(.srodata .srodata.*) /* do not need to distinguish this from .rodata */
    . = ALIGN(16);
    *(.rodata .rodata.*)
  } :text

  .data : {
    . = ALIGN(16);
    *(.sdata .sdata.*) /* do not need to distinguish this from .data */
    . = ALIGN(16);
    *(.data .data.*)
  } :data

  .bss : {
    . = ALIGN(16);
    BSS_BEGIN = .;
    *(.sbss .sbss.*) /* do not need to distinguish this from .bss */
    . = ALIGN(16);
    *(.bss .bss.*)
    . = ALIGN(8);
    BSS_END = .;
  } :data

  . = ALIGN(4096);
  ALLOC_BEGIN = .;
  KERNEL_IMAGE_SIZE = ALLOC_BEGIN - ADDR(.text);
  ALLOC_END = ADDR(.text) + 128M;
  ASSERT(ALLOC_BEGIN < ALLOC_END, "error: kernel exceeds managed memory")
}
