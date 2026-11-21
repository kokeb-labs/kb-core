"""The fault evidence path.

Reads: Handler health is read lock-free and only its edges go to the HealthJournal. The LivenessMonitor's verdict goes
there too, and the watchdog owner is kicked only when nothing is stale. The journal appends with one bounded store write
into a FaultJournal ring of 32-byte CRC-32 records with a sequence number; at boot the port's reset reason is recorded.
The store (RTC or no-init RAM, NVS, FRAM, flash) belongs to the port; after a reset Open() finds the newest record and
skips a torn slot.
"""

import kbfig
from kokeb_mkdocs_theme.figkit import Fig


def build(_d):
    f = Fig(
        "fault-journal",
        "Fault evidence path",
        alt=kbfig.title_case_alt(
            "Flow: handler health edges and the liveness verdict go to the health journal, which appends one record to",
            "a CRC-checked ring in a port-provided store that survives a reset; the boot reset reason is recorded too;",
            "the watchdog is kicked only when nothing is stale; after a reset Open finds the newest record."),
        kicker="Handlers",
        subtitle="Health edges and stalls into a CRC-checked ring that survives a reset.",
        legend=[("violet", "handler and journal"), ("amber", "liveness"), ("teal", "port"), ("brand", "stored record")],
    )
    W = f.body_w
    x0, iw = 0, W
    y = 0
    a, b = kbfig.row(f, x0, y, iw, [
        dict(title="Handlers", lines=["Health(): lock-free"], role="handler", accent=True),
        dict(title="LivenessMonitor", lines=["Verdict(now)"], role="rtos", accent=True)], gap=40)
    y = a.b + 60
    hj = f.card(x0 + iw * 0.25, y, iw * 0.5, "HealthJournal", ["edges only"], tone="violet", accent=True)
    f.edge([(a.cx, a.b + 3), (hj.x + hj.w * 0.25, hj.y - 3)], tone="violet")
    f.edge([(b.cx, b.b + 3), (hj.x + hj.w * 0.75, hj.y - 3)], tone="amber")
    y = hj.b + 60
    fj = f.card(x0 + iw * 0.2, y, iw * 0.6, "FaultJournal ring", ["32-byte records, CRC-32, sequence"], tone="brand", accent=True)
    f.edge([(hj.cx, hj.b + 3), (hj.cx, fj.y - 3)], tone="violet", label="Append: one bounded store write")
    y = fj.b + 60
    st, boot = kbfig.row(f, x0, y, iw, [
        dict(title="Store", lines=["RTC / no-init RAM, NVS, FRAM, flash"], role="port", accent=True),
        dict(title="Boot", lines=["port reset reason"], role="port", accent=True)], gap=40, weights=[1.4, 1])
    f.edge([(fj.cx - 40, fj.b + 3), (st.cx, st.y - 3)], tone="brand", label="records")
    f.edge([(boot.cx, boot.y - 3), (fj.cx + 40, fj.b + 3)], tone="teal", label="RecordReset")
    y = st.b + 48
    f.note(0, y, W, "After a reset Open() finds the newest record and skips a torn slot. The watchdog owner is kicked "
                    "only when nothing is stale.", tone="slate", title="Reading it back")
    return f
