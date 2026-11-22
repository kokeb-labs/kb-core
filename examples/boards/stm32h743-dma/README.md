# Example board: STM32H743 with DMA

A `board.yml` for an STM32H743 on the Cortex-M7 with FreeRTOS (syscall limit 5, four priority bits):

- SPI1 receive and transmit on DMA1 streams 0 and 1, staged through 1028-byte buffers in a non-cacheable D2 SRAM
  window (one MPU region, power-of-two size, aligned to it). The transmit stream raises only error interrupts.
- A timer-triggered ADC1 ring (two frames of six 2-byte conversions) on DMA1 stream 2, with its interrupt above the
  RTOS limit (priority 4) and a one-shot fallback at 2 percent CPU.
- One `Maintained` buffer in cached AXI SRAM, for frames the port cleans and invalidates by 32-byte line.

```bash
python3 tools/kb/kb.py image check examples/boards/stm32h743-dma/board.yml
python3 tools/kb/kb.py board gen examples/boards/stm32h743-dma/board.yml -o out/
```

`out/` then holds `board_dma.hpp`, `board_dma.cpp`, `kb_dma.ld`, `board_mpu.hpp` and the report
([Board DMA](../../../docs/design/board-dma.md)). Include `kb_dma.ld` in the linker script outside `SECTIONS`.
The CI job `board-dma` compiles the generated source for a Cortex-M7 and links it with that fragment.
