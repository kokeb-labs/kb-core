"""The two handler thread models.

Reads: Two models, declared per bus, one above the other; callers on other tasks use one access API in both. Model O,
the owner task, is the default for real-time buses: one real-time task, a kb::rtos::Thread, calls Service(now) at a
fixed rate, steps the single-threaded cores of handlers A and B on BusDomain<SPI1, Owner> without a mutex (an owner
check instead), publishes each step to a lock-free SeqlockSnapshot and drains a bounded command queue with completion.
Other tasks read the latest snapshot with Latest() (a lock-free copy) or post a request with Request(cmd, timeout) and
wait. Model G, the guarded facade, is for slow, sporadic devices: tasks X and Y each call Call(timeout, op) on a
Guarded handler, C or D, which takes the handler lock and then, with a bounded wait (TryLockFor), the bus lock of
BusDomain<I2C0, Guarded>, which has a mutex, recovery and statistics. An interrupt service routine only sets event
flags or notifies the owner task; it never calls a handler.

This figure stays a module of plain figkit calls: it is two small graphs stacked in one figure, which the graph kind
lays out side by side (too wide for a phone).
"""

from kokeb_mkdocs_theme.figkit import Fig
from kokeb_mkdocs_theme.figkit.core import PROFILES, card_size

K = PROFILES["detail"]
GAP_X = 16  # between two cards in a row
GAP_Y = 52  # between rows, room for an arrow and its label
PAD = 16  # inside a region


def _row(f: Fig, x: float, y: float, w: float, cards: list[tuple[str, list[str], str]]):
    """Cards side by side in a band ``w`` wide, all as tall as the tallest; ``cards`` are ``(title, lines, tone)``."""
    n = len(cards)
    cw = (w - GAP_X * (n - 1)) / n
    h = max(card_size(K, cw, t, ls) for t, ls, _tone in cards)
    return [f.card(x + i * (cw + GAP_X), y, cw, t, ls, tone=tone, h=h, accent=True)
            for i, (t, ls, tone) in enumerate(cards)]


def _down(f: Fig, a, b, tone: str, label: str = "", x: float | None = None) -> None:
    """A vertical arrow from the bottom of ``a`` to the top of ``b``, at ``x`` (default: the centre of ``b``)."""
    x = b.cx if x is None else x
    f.edge([(x, a.b + 3), (x, b.y - 3)], tone=tone, label=label)


def _up(f: Fig, a, b, tone: str, label: str = "", x: float | None = None) -> None:
    """A vertical arrow from the top of ``a`` up to the bottom of ``b``."""
    x = b.cx if x is None else x
    f.edge([(x, a.y - 3), (x, b.b + 3)], tone=tone, label=label)


def build(_d):
    f = Fig(
        "handler-threading",
        "Handler thread models",
        alt=("Two thread models, one above the other: in model O other tasks read a lock-free snapshot or queue requests "
             "that the owner task publishes and drains, and in model G tasks call guarded handlers that take the "
             "handler lock and then a bounded bus lock."),
        width=640,
        kicker="Handlers",
        subtitle="Declared per bus. Callers on other tasks use one access API in both models.",
        legend=[("brown", "task"), ("blue", "shared state"), ("violet", "handler"), ("teal", "bus domain")],
        palette=2,
        print_mode="portrait",
        adaptive=True,
    )
    W = f.body_w
    iw = W - 2 * PAD
    x = PAD

    # Model O: the owner task.
    top = 0
    y = f.region_inner_top(top, sub_below=True)
    (others,) = _row(f, x, y, iw, [("Other tasks", ["Latest(): lock-free copy", "Request(cmd, timeout): post, wait"],
                                    "brown")])
    snap, queue = _row(f, x, others.b + GAP_Y, iw, [("SeqlockSnapshot", ["published each step"], "blue"),
                                                     ("Command queue", ["drained, bounded"], "blue")])
    _down(f, others, snap, "brown", "Latest()")
    _down(f, others, queue, "brown", "Request()")
    (owner,) = _row(f, x, snap.b + GAP_Y, iw, [("Owner task", ["fixed-rate Thread, calls Service(now)"], "brown")])
    _up(f, owner, snap, "brown", "publishes")
    _up(f, owner, queue, "brown", "drains")
    ha, hb = _row(f, x, owner.b + GAP_Y, iw, [("Handler A", ["single-threaded core"], "violet"),
                                              ("Handler B", ["single-threaded core"], "violet")])
    _down(f, owner, ha, "brown")
    _down(f, owner, hb, "brown")
    (bus_o,) = _row(f, x, ha.b + GAP_Y, iw, [("BusDomain<SPI1, Owner>", ["no mutex, owner check"], "teal")])
    _down(f, ha, bus_o, "violet", x=ha.cx)
    _down(f, hb, bus_o, "violet", x=hb.cx)
    end_o = bus_o.b + PAD
    f.region(0, top, W, end_o - top, "Model O: owner task", tone="violet", sub="default for real-time buses",
             sub_below=True)

    # Model G: the guarded facade.
    top = end_o + 36
    y = f.region_inner_top(top, sub_below=True)
    tx, ty = _row(f, x, y, iw, [("Task X", [], "brown"), ("Task Y", [], "brown")])
    gc, gd = _row(f, x, tx.b + GAP_Y, iw, [("Guarded<Handler C>", ["handler lock"], "violet"),
                                            ("Guarded<Handler D>", ["handler lock"], "violet")])
    _down(f, tx, gc, "brown", "Call(timeout, op)")
    _down(f, ty, gd, "brown", "Call(timeout, op)")
    (bus_g,) = _row(f, x, gc.b + GAP_Y, iw, [("BusDomain<I2C0, Guarded>", ["mutex, recovery, statistics"], "teal")])
    _down(f, gc, bus_g, "violet", "then bus lock", x=gc.cx)
    _down(f, gd, bus_g, "violet", x=gd.cx)
    end_g = bus_g.b + PAD
    f.region(0, top, W, end_g - top, "Model G: guarded facade", tone="violet", sub="slow, sporadic devices",
             sub_below=True)

    f.note(0, end_g + 28, W, "An ISR only sets event flags or notifies the owner task. It never calls a handler.",
           tone="slate", title="Interrupts")
    return f
