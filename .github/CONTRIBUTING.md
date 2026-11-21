# Contributing

This repository follows the [Kokeb Labs engineering standard](https://github.com/kokeb-labs/.github/tree/main/standards)
and the organisation's [CONTRIBUTING.md](https://github.com/kokeb-labs/.github/blob/main/CONTRIBUTING.md). The
kb-core rules are in [docs/contributing.md](../docs/contributing.md); the short version:

- `main` is the only long-lived branch; changes reach it through squash-merged pull requests from `feat/`, `fix/`,
  `docs/`, `ci/` or `chore/` branches. A driver pin bump is `chore/pin-<chip>-<tag>`.
- One logical change per pull request; docs, Doxygen and `CHANGELOG.md` (under the part it touches) in the same
  pull request.
- Sign off every commit (`git commit -s`, DCO). No attribution lines in commits, pull requests or code.
- Run every local check before pushing; push only when it ends with no failure.
- New code is written fresh under the pipeline (design, build, self-review, gate, independent review). Code is
  never copied from older stacks (ADR-0005).
- A change to a boundary file, a deviation record or the layering rules needs a second review.

Questions: open a discussion or write to <kokeblabs@gmail.com>.
