# ntc_thermistor handler

An NTC thermistor in a resistor divider (beta, Steinhart-Hart or table curve), through
`kb::handlers::ntc_thermistor::Handler<Adc>` (`kb/handlers/bindings/ntc_thermistor.hpp`, driver
`driver-ntc-thermistor` at the pin in `components/drivers/drivers.lock.yml`). The divider node is read through one
`kb::iface::AdcChannel` as raw codes.

## What it gives the application

| Kind | Name | What |
| --- | --- | --- |
| Snapshot | `Snapshot` | `temperature_mc`, `resistance_ohm`, `code` (the mean code), `uncertainty_mc` (bound on the declared tolerances and one LSB) |
| Read | `ReadTemperature(io, timeout)` | A fresh averaged reading (a sample, also published to `Latest()`) |
| Command | `ReadResistance(io, timeout)` | The thermistor's resistance now, ohms |
| Command | `ReadRawCode(io, timeout)` | One conversion, the raw code (bring-up, wiring) |

`io` is either facade. The kb ADC channel is the thermistor's channel, so `Config::chip.adc_channel` is not used;
`Config::chip.adc_bits` must match the channel's resolution, and a table curve must outlive the handler (a
`static constexpr` array). The optional bias (excitation) pin is `Config::bias`: absent, the divider is always
powered.

## Thread model

`kRequiresOwner = false`. A thermistor changes slowly and needs no servicing, so a guarded channel is the default;
on an owner channel the owner task samples every `sample_period` and other tasks read `Latest()`.

## Start-up

| Stage | Kinds | Mode | Driver call | Deadline |
| --- | --- | --- | --- | --- |
| 0 | `Identify + Configure` | sync | `EnsureInitialized()`: configuration check (curve, window, divider, waits), bias off; no conversion | `Config::start_deadline` |

The chip is passive: there is nothing to identify, and an open or shorted thermistor shows at the first reading,
not at the start. An invalid configuration fails stage 0 with `InvalidArgument`; a bias pin that cannot be driven
fails it with `BusError` (`Bus` in `detail`).

## Health

| Driver error | kb::Status | Effect |
| --- | --- | --- |
| `NotInitialized` | `NotReady` | Faulted at once |
| `OpenCircuit` (broken thermistor, cut wire, missing part) | `NoDevice` | Counts toward `fault_after` |
| `ShortCircuit`, `Unstable`, `DeviceIdMismatch`, `DeviceFault` | `Fault` | Counts toward `fault_after` |
| `BelowRange`, `AboveRange`, `CodeOutOfScale` | `OutOfRange` | Counts toward `fault_after` |
| `Bus` (a conversion or the bias pin failed) | `BusError` | Counts toward `fault_after` |
| `InvalidArgument`, `Unsupported`, `Timeout`, `Crc`, `Busy` | the same name | `InvalidArgument` is refused (recorded, not counted); the others count toward `fault_after` |

Recovery is the generic plan: the driver has no `Deinitialize()`, so it is destroyed and started again.

## Example

```cpp
using Adc = board::BoardTempAdc;                                  // a kb::iface::AdcChannel
using Io = kb::handlers::ntc_thermistor::Handler<Adc>;
kb::handlers::ntc_thermistor::Config config{};
config.chip.curve = ntc_thermistor::presets::kGeneric10kB3950;
config.chip.min_temperature_mc = -40'000;
config.chip.max_temperature_mc = 125'000;
kb::handlers::BusDomain<Adc, kb::handlers::ThreadModel::Guarded> adc1{adc};
kb::handlers::Guarded<Io, decltype(adc1)> ntc{adc1, adc, timing, config};

static_cast<void>(adc1.Create());
static_cast<void>(ntc.Create());
if (ntc.EnsureStarted(kb::Milliseconds{10U}).IsOk()) {
  const kb::Result<int32_t> mc = kb::handlers::ntc_thermistor::ReadTemperature(ntc, kb::Milliseconds{10U});
}
```

Tests: `tests/integration/ntc_thermistor/` (the driver's ADC model behind a `kb::iface::AdcChannel`, the bias pin a
host GPIO fake, both thread models).
