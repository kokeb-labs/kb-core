"""Layer dependency rules.

Reads: Two columns. On the left, the allowed dependencies: the application uses the board HAL, kb::rtos and
kb::utils; the board HAL uses kb::handlers, kb::interfaces and one MCU port; the MCU port uses installed interface
headers and, in boundary files, the vendor SDK; handlers use drivers and interfaces; interfaces use kb::rtos; kb::rtos
uses kb::utils. On the right, the forbidden dependencies, each with the check that catches it: the application using
interfaces or the SDK directly, handlers using the SDK or an MCU port, interfaces using an MCU port, a driver using
kb::utils, and kb::utils using kb::rtos.
"""

import kbfig
from kokeb_mkdocs_theme.figkit import Fig

ALLOWED = [
    ("Application", "product", "Board HAL", "product", ""),
    ("Application", "product", "kb::rtos", "rtos", ""),
    ("Application", "product", "kb::utils", "core", ""),
    ("Board HAL", "product", "kb::handlers", "handler", ""),
    ("Board HAL", "product", "kb::interfaces", "contract", ""),
    ("Board HAL", "product", "MCU port", "port", ""),
    ("MCU port", "port", "kb::interfaces", "contract", "installed headers only"),
    ("MCU port", "port", "SDK headers", "soup", "boundary files only"),
    ("kb::handlers", "handler", "driver-<chip>", "driver", ""),
    ("kb::handlers", "handler", "kb::interfaces", "contract", ""),
    ("kb::interfaces", "contract", "kb::rtos", "rtos", ""),
    ("kb::rtos", "rtos", "kb::utils", "core", ""),
]
FORBIDDEN = [
    ("Application", "product", "kb::interfaces", "contract", "caught by the include checker and the symbol lint"),
    ("Application", "product", "SDK headers", "soup", "caught by the include checker and the symbol lint"),
    ("kb::handlers", "handler", "SDK headers", "soup", "caught by the poison build and the include checker"),
    ("kb::handlers", "handler", "MCU port", "port", "caught by the CMake DAG check"),
    ("kb::interfaces", "contract", "MCU port", "port", "caught by the include checker (LAY-010)"),
    ("driver-<chip>", "driver", "kb::utils", "core", "caught by the driver CI and the include checker"),
    ("kb::utils", "core", "kb::rtos", "rtos", "caught by the CMake target graph"),
]


def build(_d):
    f = Fig(
        "dependency-rules",
        "Which layer may depend on which",
        alt=kbfig.title_case_alt(
            "Two lists of dependency arrows between layers. Allowed: twelve, from the application down to kb::utils.",
            "Forbidden: seven, each labelled with the check that catches it."),
        kicker="Layering",
        subtitle="Every forbidden edge is caught by a named check before it can merge.",
        legend=kbfig.legend("product", "handler", "contract", "rtos", "core", "port", "driver", "soup"),
    )
    W = f.body_w
    half = (W - 40) / 2
    ya = kbfig.pairs(f, 20, 60, half - 40, ALLOWED, edge_tone="green")
    gx = half + 40
    yb = kbfig.pairs(f, gx + 20, 60, half - 40, FORBIDDEN, edge_tone="rose", dashed=True)
    f.region(0, 0, half, ya + 6, "Allowed", tone="green")
    f.region(gx, 0, half, yb + 6, "Forbidden", tone="rose")
    f.note(gx, yb + 30, half, "A dashed arrow is a forbidden edge and a solid one is allowed, so the meaning does not "
           "rest on colour alone.", tone="slate", title="Reading the arrows")
    return f
