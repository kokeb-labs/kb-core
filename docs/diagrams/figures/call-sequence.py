"""One call through the layers.

Reads: A HAL reads a register of a PF1550 power IC. The HAL calls the guarded handler (its bus is declared guarded),
which takes the handler lock and then the bus lock, both bounded, and calls the driver. The driver calls its own
WriteRead hook, implemented by the kb binding, which calls the ESP32 I2C device of the port. The port answers ok or an
I2cFailure (AddressNack, DataNack, Bus, Timeout, BusStuck); the binding returns a bool and keeps the failure for
GetLastFailure; the driver returns its Result; the handler returns a kb::Result carrying the driver's error in detail.
Only the last step knows it runs on an ESP32.
"""

import kbfig
from kokeb_mkdocs_theme.figkit import Fig


def build(_d):
    f = Fig(
        "call-sequence",
        "One call through the layers",
        alt=kbfig.title_case_alt(
            "Sequence: the HAL calls a guarded handler, which locks and calls the pf1550 driver; the driver calls the kb",
            "binding's WriteRead, which calls the ESP32 I2C device; results return the same way. Only the I2C device knows",
            "it is an ESP32."),
        kicker="Architecture",
        subtitle="A HAL reads a register of a PF1550 power IC. Only the last step knows it runs on an ESP32.",
        legend=[("magenta", "HAL"), ("violet", "handler and binding"), ("green", "driver"), ("teal", "MCU port"),
                ("~solid", "call"), ("~dashed", "return")],
    )
    actors = [("H", "HAL", "product"), ("G", "Guarded handler", "handler"), ("D", "pf1550 driver", "driver"),
              ("B", "kb binding (Pf1550Port)", "handler"), ("I", "ESP32 I2cDevice", "port")]
    msgs = [
        ("H", "G", "Call(timeout, ReadRegister)"),
        ("G", "G", "handler lock, then bus lock", "self"),
        ("G", "D", "driver call"),
        ("D", "B", "WriteRead(addr, tx, n, rx, m)"),
        ("B", "I", "WriteRead(addr, span, span)"),
        ("I", "B", "ok, or I2cFailure", "ret"),
        ("B", "D", "bool, failure kept", "ret"),
        ("D", "G", "the driver's Result", "ret"),
        ("G", "H", "kb::Result with detail", "ret"),
        ("note", "I", "The only step that knows the MCU."),
    ]
    kbfig.sequence(f, 0, actors, msgs, step=58)
    return f
