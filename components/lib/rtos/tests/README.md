# components/lib/rtos/tests/

Host tests on the `none` backend (fake clock: at-least sleeps, drift-free periodic waits, bounded try-lock) and,
when available, on the POSIX FreeRTOS port under ThreadSanitizer.
