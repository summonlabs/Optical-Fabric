# Contributing to Optical Fabric

Thanks for contributing. Optical Fabric is Apache-2.0 licensed and accepts
contributions from individuals and organizations.

## License and contributions

By submitting a contribution you agree that your contribution is offered under
the Apache License 2.0, and you warrant that you have the right to do so. No
Contributor License Agreement (CLA) is required.

## How to contribute

1. Open an issue, or choose an existing one, describing the change you intend
   to make.
2. Make your changes on a feature branch off `main`.
3. Keep the change focused and consistent with the existing style and design.
4. Build and run the test suite locally before opening a pull request.
5. Describe the completed work accurately in the imperative tense; keep history
   linear and do not squash away implementation history.

## Quality expectations

- Code must compile warning-clean with the project's configured warning flags
  (MSVC `/W4 /WX`, GCC/Clang `-Wall -Wextra -Wpedantic -Wshadow -Werror`).
- New behavior must be covered by tests and pass the full test suite.
- Documentation must be updated for public API or behavior changes.
- Do not add timeouts or watchdogs to tests. A hanging test is a defect and must
  be diagnosed, not masked.
- No telemetry transmission: all measurements and observability data stay local
  and are written to operator-selected files.
- Do not claim hardware behavior that the runtime does not implement. Optical
  Fabric never drives photonic hardware, and synthetic evidence must stay
  labelled synthetic.

## Review

Maintainers review for correctness, determinism, safety, and honest reporting.
Benchmark or performance claims must reflect measured, reproducible results and
update the relevant methodology documentation where they affect metrics.
