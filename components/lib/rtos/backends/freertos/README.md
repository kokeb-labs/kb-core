# components/lib/rtos/backends/freertos/

The FreeRTOS backend: the only files in kb-core that include FreeRTOS headers. Planned boundary files:
`freertos_boundary.cpp` (tasks, mutexes, queues, event groups through the static allocation API) and
`freertos_isr.cpp` (the `...FromISR` calls). Logic stays out of these files: they convert types and map
errors; the behaviour is tested on the POSIX FreeRTOS port on the host and in the ESP-IDF examples.
