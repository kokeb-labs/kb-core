# examples/host/

Programs that run on a PC over the host ports, one per part for now (one per driver binding once the bindings have
natural, hardware-free programs). Each is built when `KB_BUILD_EXAMPLES` is ON and run as a ctest test, so an example
that stops working fails CI. The index is [`../examples.yml`](../examples.yml).

| Folder | Part | Shows |
| --- | --- | --- |
| [`utils_checks/`](utils_checks/README.md) | utils | CRC check values, byte packing, saturating arithmetic, wrap-safe time |
| [`rtos_thread/`](rtos_thread/README.md) | rtos | A CRTP thread, a queue, a mutex, event flags and a timer on the std::thread backend |
| [`interfaces_fakes/`](interfaces_fakes/README.md) | interfaces | The host port's fake I2C bus with a register-file device and fault injection |
