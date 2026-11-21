"""One asynchronous read on an owner bus.

Reads: The owner task starts the transfer in one service pass and collects the result in a later one; no pass waits.
AsyncPoller owns the buffers and asks the port to start; a Poll in the next pass reports Pending until the peripheral
interrupt has set the owner's event flag and the transfer is complete, then Done with the count. Past the deadline the
result is Done with Timeout, late data included; Cancel makes the poller Idle and releases the buffers.
"""

import kbfig
from kokeb_mkdocs_theme.figkit import Fig


def build(_d):
    f = Fig(
        "async-poll",
        "One asynchronous read",
        alt=kbfig.title_case_alt(
            "Sequence: the owner task starts a read through the poller and the port in pass n; the poller polls and",
            "gets Pending; the interrupt completes the transfer; in pass n+1 the poll returns Done with the byte count",
            "and the completion is Ok. Past the deadline the result is Timeout; Cancel releases the buffers."),
        kicker="Handlers",
        subtitle="Started in one owner pass, collected in a later one. No pass waits.",
        legend=[("brown", "owner task"), ("violet", "poller"), ("teal", "port and interrupt"),
                ("~solid", "call"), ("~dashed", "return")],
    )
    actors = [("O", "Owner task (service pass)", "tool"), ("P", "AsyncPoller (owns the buffers)", "handler"),
              ("B", "Port (I2cAsync / SpiAsync)", "port"), ("I", "Peripheral ISR", "port")]
    msgs = [
        ("O", "P", "pass n: StartWriteRead(...)"),
        ("P", "B", "StartWriteRead(..., Deadline)"),
        ("B", "P", "Ok at once", "ret"),
        ("O", "P", "pass n: Step(now)"),
        ("P", "B", "Poll(now)"),
        ("B", "P", "Pending", "ret"),
        ("I", "B", "transfer done, event flag set"),
        ("O", "P", "pass n+1: Step(now)"),
        ("P", "B", "Poll(now)"),
        ("B", "P", "Done, Ok, count", "ret"),
        ("P", "O", "Completion() Ok, bytes held", "ret"),
        ("note", "P", "Past the deadline: Done with Timeout, late data too. Cancel(): Idle, buffers released."),
    ]
    kbfig.sequence(f, 0, actors, msgs, step=58)
    return f
