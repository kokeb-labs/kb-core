"""Handler lifecycle and health.

Reads: Seven states. A handler starts Unknown (constructed, no I/O). EnsureInitialized or StartStep moves it to
Starting, which walks the declared stages, each bounded by its deadline; the last stage makes it Ready. A failing
call makes a Ready handler Degraded until the next call succeeds; fault_after failures in a row, or one fatal error
(the chip reset or lost its state), make it Faulted. Recover() moves Faulted to Recovering with a backoff that doubles
on each failure and is refused after max_attempts. Deinitialize() returns Ready or Faulted to Unknown. Only
EnsureInitialized, StartStep, Deinitialize and Recover change the lifecycle: calls never start or restart the chip.
"""

import kbfig
from kokeb_mkdocs_theme.figkit import Fig


def build(_d):
    f = Fig(
        "handler-health",
        "Handler lifecycle and health",
        alt=kbfig.title_case_alt(
            "State machine: Unknown to Starting to Ready; Ready and Degraded swap on a failing and a passing call;",
            "Starting, Degraded and Ready can go to Faulted; Faulted goes to Recovering and back to Ready or Faulted;",
            "Deinitialize returns Ready or Faulted to Unknown."),
        kicker="Handlers",
        subtitle="Only EnsureInitialized, StartStep, Deinitialize and Recover change the lifecycle. A call never "
                 "starts or restarts the chip.",
        legend=[("slate", "not started"), ("amber", "in progress or degraded"), ("green", "healthy"),
                ("rose", "failed"), ("violet", "recovering")],
    )
    cw, ch = 190, 104
    xs = [0, 316, 632, 946]
    y0, y1 = 70, 330
    st = {}
    spec = [("Unknown", 0, y0, "slate", "constructed, no I/O"),
            ("Starting", 1, y0, "amber", "walks the declared stages"),
            ("Ready", 2, y0, "green", "all calls allowed"),
            ("Degraded", 3, y0, "amber", "a call failed, below fault_after"),
            ("Faulted", 1, y1, "rose", "chip lost or fault_after reached"),
            ("Recovering", 2, y1, "violet", "Recover() running, with backoff")]
    for name, c, y, t, sub in spec:
        st[name] = f.card(xs[c], y, cw, name, [sub], tone=t, accent=True, h=ch)
    U, S, R, D, F, V = (st[k] for k in ("Unknown", "Starting", "Ready", "Degraded", "Faulted", "Recovering"))
    # entry
    f.circle(-22 + 0, U.cy, 7, fill="ink", layer="node")
    f.edge([(-15, U.cy), (U.x - 3, U.cy)], tone="ink")
    f.edge([(U.r + 3, U.cy), (S.x - 3, S.cy)], label="start")
    kbfig.loop(f, S, "top", "next stage")
    f.edge([(S.r + 3, S.cy), (R.x - 3, R.cy)], tone="green", label="last stage ok")
    f.edge([(R.r + 3, R.cy - 22), (D.x - 3, D.cy - 22)], tone="amber", label="call fails")
    f.edge([(D.x - 3, D.cy + 22), (R.r + 3, R.cy + 22)], tone="green", label="call ok", arrow="end")
    f.edge([(S.cx - 40, S.b + 3), (F.cx - 40, F.y - 3)], tone="rose", label="stage fails", label_dx=-62)
    f.edge([(R.cx - 60, R.b + 3), (R.cx - 60, y1 - 40), (F.cx + 40, y1 - 40), (F.cx + 40, F.y - 3)], tone="rose",
           label="fatal error", label_at=0.35, label_dx=-10)
    f.edge([(D.cx, D.b + 3), (D.cx, F.b + 44), (F.cx, F.b + 44), (F.cx, F.b + 3)], tone="rose",
           label="fault_after in a row", label_at=0.55)
    f.edge([(F.r + 3, F.cy - 20), (V.x - 3, V.cy - 20)], tone="violet", label="Recover()")
    f.edge([(V.x - 3, V.cy + 20), (F.r + 3, F.cy + 20)], tone="rose", label="fails")
    f.edge([(V.cx + 20, V.y - 3), (R.cx + 20, R.b + 3)], tone="green", label="ok", label_dx=22)
    # Deinitialize: Ready and Faulted back to Unknown
    f.edge([(R.cx, R.y - 3), (R.cx, y0 - 34), (U.cx, y0 - 34), (U.cx, U.y - 3)], label="Deinitialize()", label_at=0.5)
    f.edge([(F.x - 3, F.cy), (U.cx, F.cy), (U.cx, U.b + 3)], label="Deinitialize()", label_at=0.45)
    f.note(0, y1 + ch + 70, f.body_w,
           "Recovery backoff doubles on each failure and the handler refuses Recover() after max_attempts. A failed "
           "start leaves the driver destroyed and the next start begins again from the first stage.",
           tone="slate", title="Details")
    return f
