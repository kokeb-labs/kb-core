# components/lib/interfaces/inc/kb/interfaces/

Public contract headers (`#include "kb/interfaces/<bus>.hpp"`): one concept per bus or pin kind, its vocabulary
types (`I2cFailure`, `Level`, `Polarity`, `CanFrame`, `BusStats`), its `*Ref` type-erased value when one exists,
and nothing platform-specific.
