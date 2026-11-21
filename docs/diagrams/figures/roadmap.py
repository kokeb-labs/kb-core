"""The phase roadmap.

Reads: Eight phases, M0 to M7, in order; there are no calendar dates, each phase ends on its exit criteria. M0 fixes
decisions and guardrails. M1 to M3 deliver kb::utils, kb::rtos and kb::interfaces, each with MISRA zero findings, the
coverage floor, the mutation gate and fuzzing. M4 adds the ESP32 port and the first bindings. M5 rolls out handlers for
all drivers, simplest to hardest. M6 adds breadth: a second port, the ThreadX backend, tested version sets and release
evidence in the configurator. M7 adds the generators: pin tables from board.yml, a project archive and HIL in CI.
"""

import kbfig
from kokeb_mkdocs_theme.figkit import Fig

PHASES = [
    ("M0", "Decisions and guardrails", "ADR-0006 to ADR-0013 recorded; include checker, poison build, symbol lint, preflight and CI skeleton green on empty parts.", "core"),
    ("M1", "kb::utils v0.1", "MISRA (high) with 0 findings and 0 deviations; coverage above the high floor; mutation gate; fuzz clean.", "core"),
    ("M2", "kb::rtos v0.2", "none and FreeRTOS backends; at-least delays, bounded locks, CRTP threads with stats; boundary deviations approved; TSan clean.", "rtos"),
    ("M3", "kb::interfaces v0.3", "Concepts and a host port with fault injection; BusDomain; conformance and compile-fail tests; 0 deviations.", "contract"),
    ("M4", "ESP32 port and first bindings v0.4", "Boundary files reviewed; 3 simple drivers bound and handled; integration, compat.yml, kb image check; configurator P1; bench run.", "port"),
    ("M5", "Handler rollout v0.9 to v1.0", "All drivers bound, simplest to hardest (bno08x, se05x, tmc9660 last); start-up plans, both thread models, health and recovery tests.", "handler"),
    ("M6", "Breadth", "port-stm32, ThreadX backend, tested version sets, SBOM, CVE and licence evidence in the configurator (P2).", "tool"),
    ("M7", "Generators+", "board.yml pin tables, project archive, HIL in scheduled CI.", "tool"),
]


def build(_d):
    f = Fig(
        "roadmap",
        "Phase roadmap",
        alt=kbfig.title_case_alt(
            "Two rows of four phases, M0 to M7, in order: decisions, utils, rtos, interfaces, ESP32 port and bindings,",
            "handler rollout, breadth and generators, each with its exit criteria."),
        kicker="Plan",
        subtitle="No calendar dates: a phase ends when its exit criteria are met.",
    )
    W = f.body_w
    boxes = []
    y = 0
    for r in range(2):
        cards = [dict(title=f"{m} {t}", lines=[e], role=ro, accent=True) for m, t, e, ro in PHASES[4 * r:4 * r + 4]]
        bs = kbfig.row(f, 0, y, W, cards, gap=34)
        for a, b in zip(bs, bs[1:]):
            f.edge([(a.r + 3, a.cy), (b.x - 3, b.cy)], tone="rail")
        boxes += bs
        y = bs[0].b + 64
    last_top, first_bot = boxes[3], boxes[4]
    my = last_top.b + 32
    f.edge([(last_top.cx, last_top.b + 3), (last_top.cx, my), (first_bot.cx, my), (first_bot.cx, first_bot.y - 3)], tone="rail")
    return f
