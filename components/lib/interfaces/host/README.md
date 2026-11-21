# components/lib/interfaces/host/

The host port: platform-free implementations of every contract, for tests and examples on a PC. Loopback buses
scripted by the test, fake GPIO and ADC, a fake CAN bus, a fake monotonic clock, and the asynchronous I2C and SPI
fakes (`fake_async.hpp`: latency in polls, a hang, a late completion, the cancel race). Each can inject faults (NACK on
a given byte, stuck line, short read, timeout, overrun, clock jumps near 2^31 and 2^32). No OS or SDK header. It
stays in kb-core because every test needs it; MCU ports live in `components/ports/<mcu>/` (ADR-0007). Target (with the
first
source): `kb::interfaces_host`, each type `static_assert`ed against its concept.
