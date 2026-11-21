"""The kb::rtos backends.

Reads: One API, one seam, four backends. The kb::rtos API (components/lib/rtos/inc) reaches a backend through
kb::rtos::backend (backend.hpp). The none backend is for host tests and bare-metal bring-up; FreeRTOS and ThreadX
each have boundary files that reach the kernel, which is SOUP, either a product's copy or the pinned or ESP-IDF one;
the host backend (std::thread) is for tests only. FreeRTOS and ThreadX tests run against a fake kernel.
"""

import kbfig
from kokeb_mkdocs_theme.figkit import Fig


def build(_d):
    f = Fig(
        "rtos-backends",
        "kb::rtos backends",
        alt=kbfig.title_case_alt(
            "The kb::rtos API reaches one backend through a single seam: none, FreeRTOS, host (tests only) or ThreadX;",
            "FreeRTOS and ThreadX reach their kernel, which is SOUP, through boundary files and are tested on fake kernels."),
        kicker="RTOS",
        subtitle="One API and one seam. Each backend owns its objects in opaque storage.",
        legend=kbfig.legend("rtos", "soup", extra=[("brown", "tests only")]),
    )
    W = f.body_w
    api = f.card(0, 0, W, "kb::rtos API", ["Mutex, RecursiveMutex, Semaphore, Queue, EventFlags, Timer, Thread, CriticalSection (components/lib/rtos/inc)"], tone="amber", accent=True)
    seam = f.card(0, api.b + 40, W, "kb::rtos::backend", ["backend.hpp: the seam. Each native object lives in opaque storage sized there; every backend static_asserts it fits."], tone="amber", accent=True)
    f.edge([(api.cx, api.b + 3), (api.cx, seam.y - 3)], tone="amber")
    y = seam.b + 60
    bk = kbfig.row(f, 0, y, W, [
        dict(title="none", lines=["Host tests, bare-metal bring-up. One context, a deterministic fake clock."], role="amber", accent=True, mono=False),
        dict(title="freertos", lines=["ESP32 default, any FreeRTOS target. Boundary files, static API only."], role="amber", accent=True),
        dict(title="threadx", lines=["Eclipse ThreadX targets. One boundary file."], role="amber", accent=True),
        dict(title="host", lines=["PC tests with real threads (std::thread), never built for a target."], tone="brown", accent=True)], gap=20)
    for b in bk:
        f.edge([(b.cx, seam.b + 3), (b.cx, b.y - 3)], tone="amber")
    y = bk[0].b + 60
    k = kbfig.row(f, bk[1].x, y, bk[2].r - bk[1].x, [
        dict(title="FreeRTOS kernel", lines=["SOUP: ESP-IDF, or KB_FREERTOS_INCLUDE_DIRS"], role="soup", dashed=True),
        dict(title="ThreadX kernel", lines=["SOUP: KB_THREADX_INCLUDE_DIRS, or third_party/threadx"], role="soup", dashed=True)], gap=20)
    f.edge([(bk[1].cx, bk[1].b + 3), (k[0].cx, k[0].y - 3)], tone="slate", label="boundary files")
    f.edge([(bk[2].cx, bk[2].b + 3), (k[1].cx, k[1].y - 3)], tone="slate", label="boundary file")
    f.note(0, y + 6, bk[1].x - 24, "FreeRTOS and ThreadX also run their tests on fake kernels (tests/fakes) with exact ticks.", tone="brown", title="Tests")
    return f
