"""The pieces of a handler.

Reads: Top to bottom. A HAL manager or application task calls an access facade with EnsureStarted, Request, Sample,
Latest or Health; there is one facade per handler, chosen by the bus's thread model: OwnerAccess (request queue and a
lock-free snapshot) or Guarded (handler lock, then bus lock). Only a facade can make the ServiceKey that the handler core
needs. The core, HandlerCore of the handler, the driver, the snapshot, the command and the reply, holds the start-up
plan runner, the health record, the snapshot and a liveness beat. It builds the driver in a std::optional; the driver
reaches the bus through the binding's Port, hooks over the kb::iface contract that the HAL supplies. The facade also owns
the BusDomain of its bus.
"""

import kbfig
from kokeb_mkdocs_theme.figkit import Fig


def build(_d):
    f = Fig(
        "handler-pieces",
        "The pieces of a handler",
        alt=kbfig.title_case_alt(
            "Stack: a HAL manager calls an access facade, OwnerAccess or Guarded; the facade calls the handler core with a",
            "service key and owns the bus domain; the core holds the start-up runner, health, snapshot and liveness beat",
            "and builds the driver, which reaches the HAL's bus through the binding's port and the kb::iface contract."),
        kicker="Handlers",
        subtitle="One facade per handler, chosen by the bus's thread model. Only a facade can run the core.",
        legend=kbfig.legend("product", "handler", "driver", "contract", "port"),
    )
    W = f.body_w
    mw = W * 0.64
    hal = f.card(0, 0, mw, "HAL manager or application task", ["EnsureStarted, Request, Sample, Latest, Health"], tone="magenta", accent=True)
    fy = hal.b + 56
    inner = f.region_inner_top(fy)
    oa, gd = kbfig.row(f, 16, inner, mw - 32, [
        dict(title="OwnerAccess<H>", lines=["request queue, lock-free snapshot"], role="handler", accent=True),
        dict(title="Guarded<H>", lines=["handler lock, then bus lock"], role="handler", accent=True)], gap=16)
    f.region(0, fy, mw, oa.b + 16 - fy, "Access facade", tone="violet", sub="one per handler")
    f.edge([(hal.cx, hal.b + 3), (hal.cx, fy - 3)], tone="magenta", label="calls")
    cy = oa.b + 16 + 56
    inner = f.region_inner_top(cy)
    cw = (mw - 32 - 3 * 12) / 4
    parts = kbfig.row(f, 16, inner, mw - 32, [
        dict(title="Start-up runner", lines=["kStartupPlan"], role="handler", accent=True),
        dict(title="Health", lines=["record, seqlock"], role="handler", accent=True),
        dict(title="Snapshot", lines=["seqlock"], role="handler", accent=True),
        dict(title="Liveness", lines=["beat"], role="handler", accent=True)], gap=12)
    f.region(0, cy, mw, parts[0].b + 16 - cy, "Handler core", tone="violet", sub="HandlerCore<Handler, Driver, Snapshot, Command, Reply>")
    f.edge([(gd.cx + 60, gd.b + 19), (gd.cx + 60, cy - 3)], tone="violet", label="ServiceKey")
    dy = parts[0].b + 16 + 56
    drv = f.card(0, dy, mw, "Driver", ["std::optional, built by the plan"], tone="green", accent=True)
    f.edge([(drv.cx, parts[0].b + 19), (drv.cx, drv.y - 3)], tone="violet", label="builds")
    port = f.card(0, drv.b + 48, mw, "Binding Port", ["driver hooks over kb::iface: I2cHooks, SpiHooks, UartHooks, AdcHooks, GpioHooks"], tone="violet", accent=True)
    f.edge([(drv.cx, drv.b + 3), (drv.cx, port.y - 3)], tone="green")
    iface = f.card(0, port.b + 48, mw, "kb::iface contract", ["the HAL's bus, pins and clock"], tone="blue", accent=True)
    f.edge([(port.cx, port.b + 3), (port.cx, iface.y - 3)], tone="violet")
    bx = mw + 60
    dom = f.card(bx, oa.y, W - bx, "BusDomain<Bus, ThreadModel>", ["one bus and its declared thread model"], tone="teal", accent=True)
    f.edge([(mw + 3, oa.cy + 6), (dom.x - 3, dom.cy)], tone="teal", label="owns")
    return f
