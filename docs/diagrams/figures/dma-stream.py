"""The DMA copy-out stream.

Reads: Two rows of three steps. The DMA ring in the buffer holds two halves or N frames. A half or complete interrupt makes
PublishFromDevice invalidate the lines, copy the finished frame out and stamp a sequence number, a time and a torn flag. It
copies into a slot of a ring of at least two slots; when every slot is held the frame is dropped, counted as Full, and the next
frame carries Gap(). The task takes a slot with TryTake and reads it through a move-only FrameView that releases on its end.
Judge checks the content, and Supervise answers None, Restart, BackOff or UseFallback. EnterFallback switches to the declared
fallback producer, whose frames carry Fallback(). The ring is never lent to the task (ADR-0031).
"""

import kbfig
from kokeb_mkdocs_theme.figkit import Fig


def build(_d):
    f = Fig(
        "dma-stream",
        "The DMA copy-out stream",
        alt=kbfig.title_case_alt(
            "Flow: the DMA ring raises an interrupt that copies each finished frame into a slot; the task takes a slot as a",
            "FrameView, a content check feeds the supervisor, which can switch to a declared fallback producer; a full ring",
            "drops the frame and flags the next one with Gap."),
        kicker="DMA",
        subtitle="The interrupt copies each finished frame into a slot. The task never holds the ring itself.",
        legend=[("teal", "hardware"), ("violet", "interrupt"), ("blue", "task"), ("rose", "gap"), ("green", "fallback")],
    )
    W = f.body_w
    top = kbfig.row(f, 0, 0, W, [
        dict(title="DMA ring", lines=["in the buffer: 2 halves or N frames"], role="port", accent=True),
        dict(title="Interrupt", lines=["PublishFromDevice: invalidate, copy out, seq, time, torn?"], role="handler", accent=True),
        dict(title="Ring of slots", lines=["at least 2 slots"], role="contract", accent=True)], gap=96)
    f.edge([(top[0].r + 4, top[0].cy), (top[1].x - 4, top[1].cy)], tone="rail", label="interrupt")
    f.edge([(top[1].r + 4, top[1].cy), (top[2].x - 4, top[2].cy)], tone="rail", label="slot free")
    y2 = top[0].b + 210
    bot = kbfig.row(f, 0, y2, W, [
        dict(title="Fallback producer", lines=["frames flagged Fallback()"], role="ok", accent=True),
        dict(title="Supervise", lines=["None, Restart, BackOff, UseFallback"], role="handler", accent=True),
        dict(title="FrameView", lines=["move-only, releases on end"], role="contract", accent=True)], gap=96)
    f.edge([(top[2].cx, top[2].b + 4), (bot[2].cx, bot[2].y - 4)], tone="rail", label="TryTake")
    f.edge([(bot[2].x - 4, bot[2].cy), (bot[1].r + 4, bot[1].cy)], tone="rail", label="Judge")
    f.edge([(bot[1].x - 4, bot[1].cy), (bot[0].r + 4, bot[0].cy)], tone="green", label="fallback")
    gap = f.card(top[1].x, top[1].b + 30, top[1].w, "Next frame: Gap()", ["every slot was held: the frame is dropped and counted as Full"], tone="rose", dashed=True)
    f.edge([(top[1].cx, top[1].b + 4), (gap.cx, gap.y - 4)], tone="rose", dashed=True, label="all held")
    return f
