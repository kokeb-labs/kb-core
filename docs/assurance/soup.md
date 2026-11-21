# SOUP list

Software of unknown provenance that kb-core and its in-repository ports build on. Each entry is filled in when the
code that uses it lands; each release lock records the exact versions CI used.

| Item | Version | Used by | Purpose | Known anomalies reviewed |
| --- | --- | --- | --- | --- |
| FreeRTOS kernel (as shipped in ESP-IDF) | to be recorded | `components/lib/rtos/backends/freertos/` | Scheduler, synchronisation | not yet |
| Eclipse ThreadX (`eclipse-threadx/threadx`, MIT) | `v6.5.2.202603b_rel`, commit `b2868c3d2849aab95d04cedcf8885d3f81798c86` (submodule `third_party/threadx`) | `components/lib/rtos/backends/threadx/` (`KB_RTOS=THREADX`) | Scheduler, synchronisation, timers | 2026-11-14: the repository's security advisories of 2025-2026 (GitHub GHSA list, 21 entries) concern ThreadX Modules (the module manager and the Cortex-M and Cortex-A7 module ports), the TrustZone-M entry functions and the OSEK layer, none of which kb-core builds or calls (the common kernel and the plain `components/ports/cortex_m0`, `cortex_m4`, `cortex_m7` and `linux` only); the nine of 2026-10 are fixed since `v6.5.2.202603_rel`, the eight of 2026-11 in this release. Review the list at each bump. A certified product pins its certified release instead (ADR-0015) |
| ESP-IDF | v5.5 (CI) | `components/ports/esp32/` (moves with the port) | Peripheral drivers, timer | not yet |
