# Assurance records

kb-core is developed at the handbook's assurance level `high`
([ADR-0011](../adr/ADR-0011-assurance-level-high.md)); it makes no safety-class claim, and drivers and handlers are
not a product's safety layer. This folder holds the records that level asks for, plus those a product team would
need if it later qualifies a component, written as the parts land:

| Record | File |
| --- | --- |
| SOUP list (FreeRTOS, ESP-IDF, toolchains): version, purpose, known anomalies | [soup.md](soup.md) |
| Known vulnerabilities of the pinned components, checked by a scan (OSV) | [vulnerabilities.md](vulnerabilities.md) |
| MISRA C++:2023 compliance matrix | `misra-matrix.md` (with the first source) |
| Requirement to test traceability, per part | `traceability-<part>.md` (with the first source) |
| Independent review records | `reviews/` (one per review round) |
| Equivalent mutants with reasons | `equivalent-mutants.yml` (with the first mutation run) |
| Bench evidence | `bench/` (one folder per dated run, fixed template) |
| Problem reports and their resolution | GitHub issues labelled `problem-report`, summarised per release |

Design: [Assurance](../design/assurance.md).

## Per-driver evidence (planned)

The configurator's panel shows tests, coverage and MISRA chips per selected driver. Drivers publish no evidence of
their own yet, so every row shows kb-core's (`kb-evidence/1`) and is labelled *kb-core evidence*; nothing is derived
for a driver. When a driver's CI produces evidence, the follow-up is a per-driver block keyed by driver name
(`drivers.<name>`) beside the release's evidence, with the same fields the chips read from kb-core's document:

```json
{ "tests": { "total": 41, "failures": 0 },
  "coverage": { "line": { "percent": 98.0 }, "branch": { "percent": 91.0 }, "floors": { "branch": 75 } },
  "misra": { "passed": true, "edition": "MISRA C++:2023" },
  "source": { "run_url": "https://github.com/<owner>/<repo>/actions/runs/<id>" } }
```

`js/panel.js` already reads such a block when it is present (the row then says *driver evidence*). It needs a schema
change (a new `kb-evidence` version or an additive `drivers` property), which this step does not make.
