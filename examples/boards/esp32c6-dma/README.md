# Example board: ESP32-C6 with DMA

A `board.yml` for an ESP32-C6: SPI2 with DMA through a 1028-byte staging buffer, and a continuous ADC with 256-byte
frames, four frames and callbacks that run from IRAM. They take two of the three GDMA channel pairs of the part.

```bash
python3 tools/kb/kb.py image check examples/boards/esp32c6-dma/board.yml --sdkconfig sdkconfig.defaults
python3 tools/kb/kb.py board gen examples/boards/esp32c6-dma/board.yml -o out/
```

The `iram_safe` ADC needs `CONFIG_ADC_CONTINUOUS_ISR_IRAM_SAFE=y`. Without `--sdkconfig` the check passes with a
warning that it could not look (`dma.sdkconfig_not_checked`); `--strict` makes that warning fail. The generated
`board_dma.hpp` and `board_dma.cpp` carry the compile-time checks and the bus limits; the host test compiles them.
There is no linker file: the buffers of an ESP32 board live in the SDK's own memory map
([Board DMA](../../../docs/design/board-dma.md)).
