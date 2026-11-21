"""The ThreadX backend.

Reads: The kb::rtos API reaches the seam, kb::rtos::backend. The ThreadX backend is two files: threadx_isr.cpp, where
every FromIsr call is the same call with TX_NO_WAIT, and threadx_boundary.cpp, the only boundary file, which creates
ThreadX objects on kb-owned control blocks and stacks. The kernel is SOUP and never edited; it comes from a product's
copy (KB_THREADX_INCLUDE_DIRS, for STM32Cube or a vendor SDK) or from the pinned submodule third_party/threadx at a
release tag, with KB_THREADX_PORT choosing cortex_m0, cortex_m4, cortex_m7 or linux. Tests run the boundary on a fake
kernel (contract and seam, exact ticks) and, on the pinned kernel's Linux port, as a threaded suite with priority
inheritance.
"""

import kbfig
from kokeb_mkdocs_theme.figkit import Fig


def build(_d):
    f = Fig(
        "rtos-threadx",
        "The ThreadX backend",
        alt=kbfig.title_case_alt(
            "The kb::rtos API and seam lead to two ThreadX backend files; the boundary file reaches the ThreadX kernel,",
            "which comes either from a product's copy or the pinned submodule; tests run it on a fake kernel and on the",
            "Linux port."),
        kicker="RTOS",
        subtitle="One boundary file between the kb::rtos seam and the kernel, three ways to get the kernel.",
        legend=kbfig.legend("rtos", "soup", extra=[("brown", "tests")]),
    )
    W = f.body_w
    cw = (W - 2 * 40) / 3
    api, seam = kbfig.row(f, 0, 0, W * 0.66, [
        dict(title="kb::rtos API", lines=["Mutex, RecursiveMutex, Semaphore, Queue, EventFlags, Timer, Thread, CriticalSection"], role="rtos", accent=True),
        dict(title="kb::rtos::backend", lines=["backend.hpp, the seam"], role="rtos", accent=True)], gap=50)
    f.edge([(api.r + 3, api.cy), (seam.x - 3, seam.cy)], tone="amber")
    y = api.b + 70
    isr, bnd, fake = kbfig.row(f, 0, y, W, [
        dict(title="threadx_isr.cpp", lines=["every FromIsr call is the same call with TX_NO_WAIT"], role="rtos", accent=True),
        dict(title="threadx_boundary.cpp", lines=["the only boundary file; creates objects on kb-owned control blocks and stacks"], role="rtos", accent=True),
        dict(title="Fake kernel", lines=["tests/fakes/threadx: contract and seam, exact ticks (kb_rtos_tests_threadx)"], tone="brown", accent=True)], gap=40)
    f.edge([(seam.cx, seam.b + 3), (seam.cx, seam.b + 34), (isr.cx, seam.b + 34), (isr.cx, isr.y - 3)], tone="amber")
    f.edge([(seam.cx + 40, seam.b + 3), (seam.cx + 40, seam.b + 34), (bnd.cx, seam.b + 34), (bnd.cx, bnd.y - 3)], tone="amber")
    f.edge([(isr.r + 3, isr.cy), (bnd.x - 3, bnd.cy)], tone="amber")
    f.edge([(bnd.r + 3, bnd.cy), (fake.x - 3, fake.cy)], tone="brown", dashed=True)
    ky = bnd.b + 70
    inner = f.region_inner_top(ky)
    kw = (W - 32 - 18) / 2
    pinned = f.card(16, inner, kw, "Pinned submodule", ["third_party/threadx at a release tag; KB_THREADX_PORT = cortex_m0, cortex_m4, cortex_m7 or linux"], tone="slate", dashed=True, accent=True)
    prod = f.card(16 + kw + 18, inner, kw, "A product's copy", ["KB_THREADX_INCLUDE_DIRS: STM32Cube or a vendor SDK"], tone="slate", dashed=True, accent=True, h=pinned.h)
    f.region(0, ky, W, pinned.b + 16 - ky, "ThreadX kernel", tone="slate", sub="SOUP, never edited")
    f.edge([(bnd.cx, bnd.b + 3), (bnd.cx, ky - 3)], tone="slate", label="tx_*_create")
    ly = ky + (pinned.b + 16 - ky) + 60
    linux = f.card(16, ly, kw, "Linux port", ["threaded suite with priority inheritance"], tone="brown", accent=True)
    f.edge([(pinned.cx, pinned.b + 19), (linux.cx, linux.y - 3)], tone="brown", dashed=True, label="kb_rtos_tests_threadx_linux")
    return f
