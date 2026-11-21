"""The kb repository family.

Reads: Five levels, consumers on top. A product organisation owns the product application and its board HAL. The
kokeb-labs organisation holds kb-core (utils, rtos, interfaces, handlers and the driver pins), kb-hal-common (shared HAL
bases), the reference board HALs and, planned, the configurator, an application template and tested version sets. The
kokeb-labs-drivers organisation holds each driver and each MCU port. The board HAL builds on kb-hal-common and one port;
both rest on kb-core, which pins the drivers. The configurator reads kb-core releases and fills the application template.
Only kb-core exists today; the other names are planned.
"""

import kbfig
from kokeb_mkdocs_theme.figkit import Fig


def build(_d):
    f = Fig(
        "repo-family",
        "The kb repository family",
        alt=kbfig.title_case_alt(
            "Repository graph with consumers on top: the product application uses a product board HAL, which uses",
            "kb-hal-common and an MCU port; both depend on kb-core, which pins the drivers. The configurator reads kb-core",
            "releases and fills the application template; tested version sets list sets of kb-core."),
        kicker="Ports and repositories",
        subtitle="Only kb-core exists today. Dashed cards are planned repositories.",
        legend=[("magenta", "product organisation"), ("brand", "kokeb-labs"), ("teal", "kokeb-labs-drivers"),
                ("green", "driver"), ("brown", "tool, planned")],
    )
    W = f.body_w
    cw, gap = 245, 24
    X = [i * (cw + gap) for i in range(4)]
    gy = 84  # row pitch
    R = [0, 1, 2, 3, 4]
    rh = 88
    ys = [i * (rh + 70) for i in R]
    def card(c, r, t, lines, tone, **kw):
        return f.card(X[c], ys[r], cw, t, lines, tone=tone, h=rh, **kw)
    pa = card(0, 0, "product application", ["in a product organisation"], "magenta", accent=True)
    pb = card(0, 1, "board-<name>", ["product board HAL"], "magenta", accent=True)
    kboard = card(1, 1, "kb-board-<board>", ["reference board HALs"], "brand", dashed=True, accent=True)
    halc = card(1, 2, "kb-hal-common", ["shared HAL bases"], "brand", dashed=True, accent=True)
    port = card(2, 2, "port-<family>", ["port-esp32, port-stm32"], "teal", accent=True)
    conf = card(3, 0, "kb-configurator", ["hosted configurator site"], "brown", dashed=True, accent=True)
    tmpl = card(3, 1, "kb-app-template", ["starter project it fills"], "brown", dashed=True, accent=True)
    trains = card(3, 2, "kb-trains", ["tested version sets"], "brown", dashed=True, accent=True)
    core_w = X[3] + cw
    core = f.card(0, ys[3], core_w, "kb-core", ["utils, rtos, interfaces, handlers, driver pins"], tone="brand", accent=True, h=rh)
    drv = f.card(0, ys[4], cw, "driver-<chip>", ["one repository per chip"], tone="green", accent=True, h=rh)
    f.connect(pa, pb, "bottom", "top")
    f.connect(pb, halc, "bottom", "top", at=0.8, bt=0.2)
    f.connect(pb, port, "bottom", "top", at=0.4, bt=0.3)
    f.connect(kboard, halc, "bottom", "top")
    f.connect(kboard, port, "bottom", "top", at=0.9, bt=0.1)
    f.connect(halc, core, "bottom", "top", at=0.5, bt=0.3)
    f.connect(port, core, "bottom", "top", at=0.5, bt=0.7)
    f.connect(core, drv, "bottom", "top", at=0.1, bt=0.5, label="pins")
    f.connect(conf, tmpl, "bottom", "top", dashed=True, label="fills")
    f.connect(trains, core, "bottom", "top", at=0.2, bt=0.97, dashed=True)
    f.edge([(conf.r + 3, conf.cy), (X[3] + cw + 36, conf.cy), (X[3] + cw + 36, core.cy), (core.r + 3, core.cy)], dashed=True, label="reads", label_at=0.55)
    return f
