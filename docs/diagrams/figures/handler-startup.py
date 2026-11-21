"""The handler start-up pipeline.

Reads: One runner and a declared plan of stages per chip, in a fixed order: Power, HostReset, Boot, Link, Identify,
Secure, Configure, Verify, then Ready. A plan lists only the stages its chip needs; an optional stage is skipped when
the HAL supplies no pin, rail or key for it. Every stage is synchronous or polled, and bounded by a deadline. Most
drivers declare one step that calls the driver's EnsureInitialized. The bno08x resets and boots with polled steps; the
se05x sets platform keys, opens the secure link and optionally checks the lifecycle state.
"""

import kbfig
from kokeb_mkdocs_theme.figkit import Fig

STAGES = [
    ("Power", "rail or enable pin"), ("HostReset", "reset pulse, hold, release"),
    ("Boot", "boot time, ready pin, advertisement"), ("Link", "transport session (T=1, SHTP)"),
    ("Identify", "ID, select, probe, capabilities"), ("Secure", "secure channel (platform SCP03)"),
    ("Configure", "apply the configuration"), ("Verify", "read-back, lifecycle state"),
]


def build(_d):
    f = Fig(
        "handler-startup",
        "The handler start-up pipeline",
        alt=kbfig.title_case_alt(
            "Pipeline of eight start-up stages in order, Power, HostReset, Boot, Link, Identify, Secure, Configure and",
            "Verify, ending in Ready; beside it the plans of most drivers, the bno08x and the se05x, each using only",
            "some of the stages."),
        kicker="Handlers",
        subtitle="One runner, a plan of declared stages per chip. Each stage is synchronous or polled, and bounded.",
        legend=[("violet", "stage"), ("green", "ready"), ("blue", "plan of one chip")],
    )
    W = f.body_w
    lw = 470
    y = 0
    prev = None
    for i, (n, d) in enumerate(STAGES):
        b = f.card(0, y, lw, n, [d], tone="violet", h=kbfig.need_h(f, lw, n, [d]) + 6, accent=True)
        f.step(lw - 28, b.cy, i + 1, tone="violet")
        if prev:
            f.edge([(prev.cx, prev.b + 3), (b.cx, b.y - 3)], tone="violet")
        prev = b
        y = b.b + 22
    rd = f.card(0, y, lw, "Ready", ["the handler serves calls"], tone="green", accent=True, h=74)
    f.edge([(prev.cx, prev.b + 3), (rd.cx, rd.y - 3)], tone="green")
    px, pw = lw + 60, W - lw - 60
    plans = [
        ("Most drivers", ["One step: Start = driver.EnsureInitialized().", "Identify and Configure happen inside the driver."]),
        ("bno08x", ["HostReset = Begin(cfg), synchronous.", "Boot and Link = Poll() until Ready(), polled.",
                    "Configure = SetFeature operations until OpDone, polled."]),
        ("se05x", ["Secure keys = SetPlatformKeys(), no I/O.", "Link, Identify and Secure = Open(), synchronous and bounded by the link.",
                   "Verify = lifecycle state check, optional."]),
    ]
    py = 0
    f.text(px, 16, "PLANS OF DIFFERENT CHIPS", size=f.k["region"], weight=700, color="blue.text")
    py = 34
    for t, ls in plans:
        c = f.card(px, py, pw, t, ls, tone="blue", accent=True)
        py = c.b + 20
    f.note(px, py + 6, pw, "A stage the HAL gives nothing for (no pin, rail or key) is skipped, not failed.",
           tone="slate", title="Optional stages")
    return f
