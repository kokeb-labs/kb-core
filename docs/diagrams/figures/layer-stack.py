"""The layer stack of a kb-core product.

Reads: From top to bottom. The product application calls the board HAL. The board HAL talks to kb::handlers and
picks one MCU port; handlers use the driver of each chip and the interface contracts, the contracts rest on
kb::rtos, and kb::rtos rests on kb::utils. The MCU port satisfies the interface contracts and touches the vendor
SDK in its boundary files only; the RTOS backends touch the kernel in theirs. The vendor SDK, kernels and mbedTLS
are SOUP: never analysed, never edited.
"""

import kbfig
from kokeb_mkdocs_theme.figkit import Fig


def build(_d):
    f = Fig(
        "layer-stack",
        "The layer stack",
        alt=kbfig.title_case_alt(
            "Layer stack, top to bottom: the product application; the board HAL repository; kb-core with kb::handlers,",
            "kb::interfaces, kb::rtos and kb::utils, each depending on the next; below it the drivers used by handlers,",
            "the MCU ports that satisfy the interface contracts, and the vendor SOUP reached only through boundary files."),
        kicker="Architecture",
        subtitle="Arrows point from a layer to what it depends on. Only boundary files touch vendor code.",
        legend=kbfig.legend("product", "handler", "contract", "rtos", "core", "port", "driver", "soup"),
    )
    W = f.body_w
    gap = 18
    _band, (app,) = kbfig.lane(f, 0, 0, W, "Product", [
        dict(title="Application tasks", lines=["control, protocols, safety layer"], role="product", accent=True)])
    hal_y = app.b + 64
    _b, (facade, cfg, bind) = kbfig.lane(f, 0, hal_y, W, "Board HAL", [
        dict(title="Board facade and managers", lines=["built on kb-hal-common"], role="product"),
        dict(title="Board config", lines=["bus table, thread model per bus, chip list, pins"], role="product"),
        dict(title="Platform bind", lines=["builds the port objects, sets the start-up order"], role="product"),
    ], weights=[1.5, 1.1, 1.4], gap=gap)
    f.connect(app, facade, "bottom", "top", at=0.97, bt=0.95, label="calls")
    kb_y = bind.b + 64 + 20
    inner = f.region_inner_top(kb_y)
    cw = (W - 32 - 3 * gap) / 4
    cards = [
        ("kb::handlers", ["bindings, handler core, start-up pipeline, owner and guarded access"], "handler"),
        ("kb::interfaces", ["contracts: C++20 concepts only, plus the host test port"], "contract"),
        ("kb::rtos", ["API; backends none, FreeRTOS, ThreadX"], "rtos"),
        ("kb::utils", ["Status/Result, units, containers, CRC, seqlock"], "core"),
    ]
    h = max(kbfig.need_h(f, cw, t, ls) for t, ls, _r in cards)
    kb_b = inner + h + 16
    f.region(0, kb_y, W, kb_b - kb_y, "kb-core", tone="brand")
    bx = [f.card(16 + i * (cw + gap), inner, cw, t, ls, tone=kbfig.tone(r), h=h, accent=True)
          for i, (t, ls, r) in enumerate(cards)]
    handlers, ifaces, rtos, utils = bx
    for a, b in ((handlers, ifaces), (ifaces, rtos), (rtos, utils)):
        f.edge([(a.r + 3, a.cy), (b.x - 3, b.cy)], tone="rail")
    f.connect(facade, handlers, "bottom", "top", at=0.95, bt=0.9, label="uses")

    low = kb_b + 64
    drv_h = kbfig.need_h(f, cw, "driver-<chip>", ["brand-free, own CI at level high, pinned release tags"])
    drv = f.card(16, low, cw, "driver-<chip>", ["brand-free, own CI at level high, pinned release tags"],
                 tone="green", accent=True, h=drv_h)
    f.connect(handlers, drv, "bottom", "top", at=0.9, bt=0.9, label="binds")
    px = 16 + cw + gap - 16
    pw = W - px
    _b, (esp, stm) = kbfig.lane(f, px, low - 40, pw, "MCU ports", [
        dict(title="port-esp32", lines=["components/ports/esp32, own CMake project"], role="port", accent=True),
        dict(title="port-stm32", lines=["planned, own repository"], role="port", dashed=True)],
        role="port", weights=[1, 1])
    f.connect(esp, ifaces, "top", "bottom", at=0.8, bt=0.5, label="satisfies")
    soup = f.card(0, max(esp.b, drv.b) + 64, W, "SOUP",
                  ["ESP-IDF, STM32 HAL, FreeRTOS, ThreadX, mbedTLS. Reached only through boundary files of the ports "
                   "and the RTOS backends; never analysed, never edited."], tone="slate", dashed=True)
    f.connect(esp, soup, "bottom", "top", at=0.8, label="boundary files only")
    return f
