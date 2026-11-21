# deviations/

MISRA C++:2023 deviation records of kb-core, and the list of boundary files that may need them.

- The handbook's project-wide records (MD-CPP-001 onwards, handbook `compliance/misra-cpp-2023-deviations.yml`)
  apply to kb-core as to every repository. This folder holds only kb-core's own records.
- `deviations.yml` is read by the MISRA workflow (ci-tools `ru-misra`, input `repo_deviations`). Only records
  with `check: misra/<guideline>` become CodeQL deviations; a record for another tool uses that tool's check id.
- `boundary-files.yml` lists the only files of kb-core allowed to include an RTOS header; each MCU port has its own
  list and records in `components/ports/<mcu>/deviations/` (ADR-0007). A record's `where` must
  name files from its list; there is no deviation anywhere else (docs/design/assurance.md).
- In code, a deviation is a marker citing a record id: `// codeql::misra_deviation(DEV-KBC-001)` on the line, or
  `codeql::misra_deviation_next_line(...)` before it. A marker that deviates nothing, an unknown id or a lapsed
  record (`review_by` passed) fails the scan.

## Record format

```yaml
deviations:
  - id: DEV-KBC-001                 # never reused
    check: misra/Rule 8.2.5         # misra/<Rule|Dir> <number>; a mandatory guideline is never deviated
    where: components/lib/rtos/backends/freertos/freertos_boundary.cpp   # exact files, from boundary-files.yml
    reason: >-
      Why the guideline cannot be met here (the external C API that forces it).
    mitigation: >-
      How the risk the guideline addresses is still controlled (typed wrapper, host test, second-person review).
    approved_by: maintainer
    date: YYYY-MM-DD
    review_by: YYYY-MM-DD           # the record lapses unless renewed
```
