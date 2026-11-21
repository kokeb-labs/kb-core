# Assurance records

kb-core is developed at the handbook's assurance level `high`
([ADR-0011](../adr/ADR-0011-assurance-level-high.md)); it makes no safety-class claim, and drivers and handlers are
not a product's safety layer. This folder holds the records that level asks for, plus those a product team would
need if it later qualifies a component, written as the parts land:

| Record | File |
| --- | --- |
| SOUP list (FreeRTOS, ESP-IDF, toolchains): version, purpose, known anomalies | [soup.md](soup.md) |
| Known vulnerabilities of the pinned components, checked weekly (OSV) | [vulnerabilities.md](vulnerabilities.md) |
| MISRA C++:2023 compliance matrix | `misra-matrix.md` (with the first source) |
| Requirement to test traceability, per part | `traceability-<part>.md` (with the first source) |
| Independent review records | `reviews/` (one per review round) |
| Equivalent mutants with reasons | `equivalent-mutants.yml` (with the first mutation run) |
| Bench evidence | `bench/` (one folder per dated run, fixed template) |
| Problem reports and their resolution | GitHub issues labelled `problem-report`, summarised per release |

Design: [Assurance](../design/assurance.md).
