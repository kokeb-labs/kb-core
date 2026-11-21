"""The two handler thread models.

Reads: Two models, declared per bus; callers on other tasks use one access API in both. In the owner-task model, one
real-time task calls Service(now) at a fixed rate, steps the single-threaded handlers on a bus domain without a mutex,
publishes each step to a lock-free seqlock snapshot and drains a bounded command queue. Other tasks read the latest
snapshot (a lock-free copy) or post a request and wait with a timeout. In the guarded model, each calling task takes the
handler lock and then, within a bounded wait, the bus lock of a mutex-protected bus domain. An interrupt service routine
only sets event flags or notifies the owner task.
"""

import kbfig
from kokeb_mkdocs_theme.figkit import Fig


def build(_d):
    f = Fig(
        "handler-threading",
        "Handler thread models",
        alt=kbfig.title_case_alt(
            "Two side by side models. Owner task: other tasks read a lock-free snapshot or post requests to a queue that",
            "the owner task publishes to and drains, and the owner task steps the handlers on an unlocked bus domain.",
            "Guarded facade: tasks call a guarded handler that takes the handler lock then a bounded bus lock."),
        kicker="Handlers",
        subtitle="Declared per bus. Callers on other tasks use one access API in both models.",
        legend=[("violet", "handler"), ("brown", "task"), ("blue", "shared state"), ("teal", "bus domain")],
    )
    W = f.body_w
    gw = 24
    half = (W - gw) / 2
    # ---------------- model O
    top = 0
    x0, iw = 16, half - 32
    y = f.region_inner_top(top, sub_below=True)
    others = f.card(x0, y, iw, "Other tasks", ["Latest(): lock-free copy", "Request(cmd, timeout): post and wait"],
                    tone="brown", accent=True)
    y = others.b + 54
    cw = (iw - 16) / 2
    snap, q = kbfig.row(f, x0, y, iw, [
        dict(title="SeqlockSnapshot", lines=["published each step"], role="contract", accent=True),
        dict(title="Command queue", lines=["with completion, drained bounded per step"], role="contract", accent=True)], gap=16)
    f.edge([(snap.cx, others.b + 3), (snap.cx, snap.y - 3)], tone="brown", label="Latest()")
    f.edge([(q.cx, others.b + 3), (q.cx, q.y - 3)], tone="brown", label="Request()")
    y = snap.b + 54
    owner = f.card(x0, y, iw, "Owner task", ["kb::rtos::Thread at a fixed rate, calls Service(now)"], tone="brown", accent=True)
    f.edge([(snap.cx, owner.y - 3), (snap.cx, snap.b + 3)], tone="brown", label="publishes")
    f.edge([(q.cx, owner.y - 3), (q.cx, q.b + 3)], tone="brown", label="drains")
    y = owner.b + 48
    ha, hb = kbfig.row(f, x0, y, iw, [dict(title="Handler A", lines=["single-threaded core"], role="handler", accent=True),
                                      dict(title="Handler B", lines=["single-threaded core"], role="handler", accent=True)], gap=16)
    f.edge([(ha.cx, owner.b + 3), (ha.cx, ha.y - 3)], tone="brown")
    f.edge([(hb.cx, owner.b + 3), (hb.cx, hb.y - 3)], tone="brown")
    y = ha.b + 48
    bus = f.card(x0, y, iw, "BusDomain<SPI1, Owner>", ["no mutex, owner check"], tone="teal", accent=True)
    f.edge([(ha.cx, ha.b + 3), (ha.cx, bus.y - 3)], tone="violet")
    f.edge([(hb.cx, hb.b + 3), (hb.cx, bus.y - 3)], tone="violet")
    # ---------------- model G
    gx = half + gw
    x1 = gx + 16
    y = f.region_inner_top(top, sub_below=True)
    tx, ty = kbfig.row(f, x1, y, iw, [dict(title="Task X", tone="brown", accent=True),
                                      dict(title="Task Y", tone="brown", accent=True)], gap=16)
    y = tx.b + 70
    gc, gd = kbfig.row(f, x1, y, iw, [dict(title="Guarded<Handler C>", lines=["handler lock"], role="handler", accent=True),
                                      dict(title="Guarded<Handler D>", lines=["handler lock"], role="handler", accent=True)], gap=16)
    f.edge([(tx.cx, tx.b + 3), (gc.cx, gc.y - 3)], tone="brown", label="Call(timeout, op)")
    f.edge([(ty.cx, ty.b + 3), (gd.cx, gd.y - 3)], tone="brown", label="Call(timeout, op)")
    y = gc.b + 80
    bus2 = f.card(x1, y, iw, "BusDomain<I2C0, Guarded>", ["mutex, recovery, statistics"], tone="teal", accent=True)
    f.edge([(gc.cx, gc.b + 3), (gc.cx, bus2.y - 3)], tone="violet", label="then bus lock (TryLockFor)")
    f.edge([(gd.cx, gd.b + 3), (gd.cx, bus2.y - 3)], tone="violet")
    end = max(bus.b, bus2.b) + 18
    f.region(0, top, half, end - top, "Model O: owner task", tone="violet", sub="default for real-time buses", sub_below=True)
    f.region(gx, top, half, end - top, "Model G: guarded facade", tone="violet", sub="slow, sporadic devices", sub_below=True)
    f.note(0, end + 28, W, "An ISR only sets event flags or notifies the owner task. It never calls a handler.", tone="slate", title="Interrupts")
    return f
