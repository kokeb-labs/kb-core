/* The smallest linker script that uses a generated kb_dma.ld (CI job board-dma, tools/preflight/board_dma_cross.sh).
   Memory is the STM32H743's: flash, AXI SRAM, SRAM1 (D2). The generated fragment places the DMA sections itself. */
MEMORY
{
  FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 2M
  AXI   (rwx) : ORIGIN = 0x24000000, LENGTH = 512K
  SRAM1 (rwx) : ORIGIN = 0x30000000, LENGTH = 128K
}

ENTRY(main)

SECTIONS
{
  .text : { *(.text .text.*) *(.rodata .rodata.*) } > FLASH
  .data : { *(.data .data.*) } > AXI AT > FLASH
  .bss  : { *(.bss .bss.*) *(COMMON) } > AXI
  /DISCARD/ : { *(.ARM.exidx*) *(.comment) *(.note*) }
}

INCLUDE kb_dma.ld
