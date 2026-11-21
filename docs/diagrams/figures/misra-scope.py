"""MISRA C++:2023 scope at assurance level high.

Reads: Four zones, from strongest claim to none. kb-core's own code under components/lib is claimed compliant with
zero repository deviations: utils, interface contracts and host port, the rtos public API, and handlers with bindings.
Boundary files, the RTOS backends' and the ports' files that touch vendor code, are compliant with recorded deviations.
Each driver at its pinned tag is covered by its own MISRA run at level high. SOUP (ESP-IDF, STM32 HAL, FreeRTOS,
ThreadX, mbedTLS) is out of scope, listed with versions, included with -isystem and never reported.
"""

import kbfig
from kokeb_mkdocs_theme.figkit import Fig


def build(_d):
    f = Fig(
        "misra-scope",
        "MISRA C++:2023 scope",
        alt=kbfig.title_case_alt(
            "Four zones from strongest claim to none: kb-core own code claimed compliant with zero deviations; boundary",
            "files compliant with recorded deviations; drivers covered by their own evidence; SOUP out of scope and never",
            "reported."),
        kicker="Assurance",
        subtitle="What is claimed for each zone, at assurance level high.",
        legend=[("green", "claimed, zero deviations"), ("amber", "claimed, recorded deviations"),
                ("blue", "covered by its own evidence"), ("slate", "out of scope")],
    )
    W = f.body_w
    z1, c1 = kbfig.lane(f, 0, 0, W, "Claimed compliant", [
        dict(title="kb::utils", lines=["phase 1"], role="core", accent=True),
        dict(title="kb::interfaces", lines=["contracts and host port, phase 1"], role="contract", accent=True),
        dict(title="kb::rtos", lines=["public API, phase 2"], role="rtos", accent=True),
        dict(title="kb::handlers", lines=["handlers and bindings, phase 4"], role="handler", accent=True)],
        role="ok", sub="kb-core own code, components/lib/**, zero repository deviations")
    z2, c2 = kbfig.lane(f, 0, z1.b + 56, W, "Boundary files", [
        dict(title="RTOS backend boundary files", lines=["components/lib/rtos/backends/freertos/*_boundary.cpp"], mono=True, role="amber"),
        dict(title="Port boundary files", lines=["components/ports/esp32/src/*_boundary.cpp (own records)"], mono=True, role="amber")],
        role="amber", sub="compliant with recorded deviations (DEV-KBC-*, DEV-KBP-*)")
    z3, c3 = kbfig.lane(f, 0, z2.b + 56, W, "Covered by their own evidence", [
        dict(title="driver-<chip> at its pinned tag", lines=["own ru-misra run at level high"], role="driver", accent=True)],
        role="contract")
    z4, c4 = kbfig.lane(f, 0, z3.b + 56, W, "SOUP, out of scope", [
        dict(title="Vendor and third-party code", lines=["ESP-IDF, STM32 HAL, FreeRTOS, ThreadX, mbedTLS, listed with versions"], role="soup", dashed=True)],
        role="soup")
    f.edge([(W - 200, z1.b + 3), (W - 200, z2.y - 3)], tone="green", label="includes the boundary files")
    f.edge([(W - 200, z3.b + 3), (W - 200, z4.y - 3)], tone="rail", dashed=True, label="-isystem, never reported")
    return f
