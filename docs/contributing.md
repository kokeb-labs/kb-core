# Contributing

The organisation's rules apply ([.github/CONTRIBUTING.md](../.github/CONTRIBUTING.md)). What is particular to
kb-core:

- **Fresh code under the pipeline.** Each unit goes through design, build, self-review, the local gate,
  independent review and fixes before it is merged ([ADR-0005](adr/ADR-0005-no-code-port.md)).
- **C++20 and concepts only.** No virtual functions; a new base class needs a row in the register of
  [ADR-0006](adr/ADR-0006-concepts-only.md).
- **Layering.** A change never adds an include or a link across the rules in [Layering rules](layering.md); a
  change to a boundary file, a deviation record or the rules themselves needs a second reviewer.
- **The port seam.** Code in `components/ports/<mcu>/` uses only installed kb-core headers; nothing in `components/lib/`
  reaches into
  `components/ports/`.
- **One part per pull request** where possible; the CHANGELOG entry goes under that part.
- **Driver pins** change only to a release or release-candidate tag whose CI passed; never a branch, never an
  edit inside `components/drivers/`.
- **Tests with the change**: unit tests in the part, an integration test for a binding, a docs test for every new
  code block.
- Sign off every commit (`git commit -s`). No attribution lines.
