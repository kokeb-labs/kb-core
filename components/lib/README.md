# components/lib/

The four parts of kb-core. Each folder is one CMake target, one published package and one Doxygen section, and
each builds alone with only the parts it declares (CI: `tests/standalone/`).

| Folder | Target | Depends on | Who may use it |
| --- | --- | --- | --- |
| [`utils/`](utils/README.md) | `kb::utils` | nothing | everyone, applications included |
| [`rtos/`](rtos/README.md) | `kb::rtos` | `kb::utils` | everyone, applications included |
| [`interfaces/`](interfaces/README.md) | `kb::interfaces` | `kb::rtos`, `kb::utils` | the HAL, `kb::handlers` and MCU ports only |
| [`handlers/`](handlers/README.md) | `kb::handlers` | drivers, `kb::interfaces`, `kb::rtos`, `kb::utils` | the HAL only |

Rules for every part:

- Public headers in `inc/kb/<part>/<name>.hpp`, implementation in `src/<name>.ipp`, included at the end of its
  header. `.cpp` files exist only in the RTOS boundary folders (`rtos/backends/<rtos>/`); MCU code is in
  `components/ports/<mcu>/`, next to this folder.
- C++20 only, under the KB-CPP20 profile: no exceptions, no RTTI, no heap, no virtual functions, no SDK header in
  a public header. Concepts state contracts; a base class only where the register of ADR-0006 lists it.
- Tests of a part live in its own `tests/` and `fuzz/`; tests that need several parts or drivers live in the
  root `tests/`.
- Dependencies point down only (ADR-0001); `tools/include_checker` enforces it.
