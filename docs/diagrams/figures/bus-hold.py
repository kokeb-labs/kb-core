"""Held bus transactions in both thread models.

Reads: On a guarded bus a task takes the handler mutex first and then the bus mutex, both bounded, and holds the bus
for several calls with nothing in between; another task's plain request waits (bounded) or times out. On an owner bus
the owner task takes the hold between passes, which never waits, and runs inline calls for several handlers; the
service pass reports Busy while the hold is held and the violation is counted.
"""

import kbfig
from kokeb_mkdocs_theme.figkit import Fig


def build(_d):
    f = Fig(
        "bus-hold",
        "Held transactions",
        alt=kbfig.title_case_alt(
            "Two side by side models. Guarded bus: task X takes the handler mutex, then the bus mutex, and makes",
            "several calls while task Y's plain request waits or times out. Owner bus: the owner task takes the hold",
            "between passes and runs inline calls; the service pass reports Busy while the hold is held."),
        kicker="Handlers",
        subtitle="The order of the locks, and what each hold excludes.",
        legend=[("brown", "task"), ("violet", "handler"), ("teal", "bus domain"), ("rose", "excluded")],
    )
    W = f.body_w
    gw = 24
    half = (W - gw) / 2
    iw = half - 32
    top = 0
    x0 = 16
    y = f.region_inner_top(top, sub_below=True)
    tx = f.card(x0, y, iw, "Task X", ["Guarded::Hold(timeout)"], tone="brown", accent=True)
    y = tx.b + 40
    hl = f.card(x0, y, iw, "1: handler mutex", ["bounded wait"], tone="violet", accent=True)
    f.edge([(tx.cx, tx.b + 3), (tx.cx, hl.y - 3)], tone="brown")
    y = hl.b + 40
    bl = f.card(x0, y, iw, "2: BusDomain::Hold", ["bus mutex, bounded wait"], tone="teal", accent=True)
    f.edge([(hl.cx, hl.b + 3), (hl.cx, bl.y - 3)], tone="violet")
    y = bl.b + 40
    gh = f.card(x0, y, iw, "GuardedHeld", ["Request, Sample ... many calls, nothing in between"], tone="violet", accent=True)
    f.edge([(bl.cx, bl.b + 3), (bl.cx, gh.y - 3)], tone="teal")
    y = gh.b + 40
    ty = f.card(x0, y, iw, "Task Y: plain Request", ["waits (bounded) or returns Timeout"], tone="rose", accent=True, dashed=True)
    gx = half + gw
    x1 = gx + 16
    y = f.region_inner_top(top, sub_below=True)
    ot = f.card(x1, y, iw, "Owner task, between passes", ["BusDomain::Hold(): never waits"], tone="brown", accent=True)
    y = ot.b + 40
    pf = f.card(x1, y, iw, "Pass flag", ["set while the hold is held"], tone="teal", accent=True)
    f.edge([(ot.cx, ot.b + 3), (ot.cx, pf.y - 3)], tone="brown")
    y = pf.b + 40
    oh = f.card(x1, y, iw, "OwnerAccess::Held(hold)", ["inline calls, several handlers"], tone="violet", accent=True)
    f.edge([(pf.cx, pf.b + 3), (pf.cx, oh.y - 3)], tone="teal")
    y = oh.b + 40
    sp = f.card(x1, y, iw, "Service(now, ...)", ["Busy while held; the violation is counted"], tone="rose", accent=True, dashed=True)
    end = max(ty.b, sp.b) + 18
    f.region(0, top, half, end - top, "Guarded bus", tone="violet", sub="handler lock, then bus lock", sub_below=True)
    f.region(gx, top, half, end - top, "Owner bus", tone="violet", sub="the pass flag", sub_below=True)
    return f
