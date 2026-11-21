"""Ownership of a DMA buffer.

Reads: Three states. A buffer is constructed in Cpu: static, line-aligned and padded. Lend moves it from Cpu to Device
after the owned cache lines are cleaned or invalidated and a barrier ran. Complete moves it from Device back to Cpu after a
receive is invalidated and a barrier ran. Abort with proof that the bus master stopped also returns it to Cpu. Abort without
proof, or a lease destroyed while the device owns the buffer, moves it from Device to Quarantined. Recover with proof,
after the peripheral was re-initialised, moves it from Quarantined to Cpu. In Quarantined a Lend is refused with a Fault and
the CPU view is empty. Only the port holds the key that moves a buffer between states (ADR-0030).
"""

import kbfig
from kokeb_mkdocs_theme.figkit import Fig


def build(_d):
    f = Fig(
        "dma-lease",
        "Ownership of a DMA buffer",
        alt=kbfig.title_case_alt(
            "State machine with the states Cpu, Device and Quarantined: Lend moves a buffer from Cpu to Device, Complete or a",
            "proven Abort returns it to Cpu, an unproven Abort moves it to Quarantined, and Recover with proof returns it to",
            "Cpu."),
        kicker="DMA",
        subtitle="Only the port holds the key that moves a buffer between states. The CPU's view exists in Cpu only.",
        legend=[("green", "the CPU owns it"), ("amber", "the device owns it"), ("rose", "quarantined")],
    )
    cw, ch = 190, 104
    y = 70
    C = f.card(0, y, cw, "Cpu", ["constructed: static, line-aligned, padded"], tone="green", accent=True, h=ch)
    D = f.card(f.body_w / 2 - cw / 2, y, cw, "Device", ["the peripheral owns the buffer"], tone="amber", accent=True, h=ch)
    Q = f.card(f.body_w - cw, y, cw, "Quarantined", ["CpuView empty, Lend refused with a Fault"], tone="rose", accent=True, h=ch)
    f.circle(-22, C.cy, 7, fill="ink", layer="node")
    f.edge([(-15, C.cy), (C.x - 3, C.cy)], tone="ink")
    f.edge([(C.r + 3, C.cy - 22), (D.x - 3, D.cy - 22)], tone="amber", label="Lend")
    f.edge([(D.x - 3, D.cy + 22), (C.r + 3, C.cy + 22)], tone="green", label="Complete", arrow="end")
    f.edge([(D.r + 3, D.cy), (Q.x - 3, Q.cy)], tone="rose", label="Abort without proof", dashed=True)
    f.edge([(Q.cx, Q.b + 3), (Q.cx, y + ch + 60), (C.cx, y + ch + 60), (C.cx, C.b + 3)], tone="green",
           label="Recover with proof", label_at=0.5)
    f.edge([(D.cx, D.b + 3), (D.cx, y + ch + 34), (C.cx + 50, y + ch + 34), (C.cx + 50, C.b + 3)], tone="green",
           label="Abort with proof", label_at=0.5, label_dx=30)
    f.note(0, y + ch + 100, f.body_w,
           "Lend cleans or invalidates the owned cache lines and runs a barrier. Complete invalidates a receive and runs a "
           "barrier. A lease destroyed while the device owns the buffer also quarantines it. Recover needs the peripheral "
           "to be re-initialised first.", tone="slate", title="Details")
    return f
