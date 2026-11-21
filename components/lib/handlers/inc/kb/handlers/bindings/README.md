# components/lib/handlers/inc/kb/handlers/bindings/

One header per driver, `<name>.hpp` (the driver.yml name), namespace `kb::handlers::<name>`: the port that
satisfies the driver's port concept over kb contracts (through the hook bases), `ToStatus()` for every driver error,
`Config`, `Snapshot`, `Command`/`Reply`, the `Handler` with its `kStartupPlan`, `kRequiresOwner` and `kBus`, and the
app calls that work through either facade. Each binding has an integration test in `tests/integration/<name>/`
against the driver's own chip model and a page in `docs/handlers/<name>.md`.
