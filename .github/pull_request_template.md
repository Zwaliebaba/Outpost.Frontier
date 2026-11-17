## Summary

<!-- What changes and why, in a few sentences. Name the work packages
(e.g. P1.8) and the C++ libraries involved. For several independent
changes, give each its own subsection and commit. -->

## Changes

<!-- The changes a reviewer must know about, grouped by crate or area.
Mention changes outside the owning crate: CppSource (frozen: only
reference-build fixes, oracle instrumentation or a recorded exception),
CI, xtask, dependencies (each needs a T13 entry), unsafe allowances. -->

## Decisions

<!-- New or amended entries in design/decisions.md, with their IDs and who
made them (owner or technical). Check that the IDs are still free on main.
Write "None" if there are none. -->

## Divergences

<!-- New or changed rows in design/divergences.md (IDs and a one-line
reason each). Every deliberate difference from the C++ behaviour needs
one. Write "None" if there are none. -->

## Testing

<!-- What was run and where (host and target), and what each test checks:
oracle suites, data tests on outdir/, the local checks below. Quote
results that matter (counts, files compared, differences). -->

## Open items

<!-- What is left before the work package or gate is done, follow-ups, or
questions for the owner. Delete the section if there are none. -->

---

- [ ] `cargo fmt --all --check`, `cargo clippy --locked --workspace --all-targets -- -D warnings` and `cargo test --locked --workspace --all-targets` pass (add `--target host-tuple` off Windows)
- [ ] `cargo xtask check-policy` and `cargo deny check` pass
- [ ] Committed vectors were not rewritten to make a failing comparison pass; no dumps or vectors made from `outdir/` are committed
- [ ] Ledger rows, the port map, `design/README.md` progress and `AGENTS.md` are updated where the change affects them
