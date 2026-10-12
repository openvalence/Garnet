---
paths:
  - "**"
---

# Governance: a pointer, not a copy

Garnet follows the Canon. It does not restate it.

- **Doctrine home:** `../Nucleus/.claude/rules/governance.md` (C-1..C-12: one
  fact one home, status-never-in-prose, stamp-or-hearsay, the Canon Flag duty,
  frozen list, amendment ritual, American English, comments-are-constraints).
  Read it there; a copy here would be a second home for the same fact, which
  C-1 forbids. The Valence repo has no copy of it.
- **Wire numbers, protocol behavior:** the Valence repo
  (`spec/registry/registry.yaml`, `spec/SPEC.md`). Never a code-local magic
  number for anything wire-visible. A gap in the spec is fixed in the Valence
  repo FIRST, then coded against here.
- **The Valence spec and registry are READ-ONLY from here.** Changes there
  are RFCs (`rfc-lifecycle` formula). Client library code (`../Valence/lib/valence`)
  changes in the Valence repo on its own branch.
- **Volatile truth** (what is measured, what is built, open questions) lives
  on this repo's own dev board: `bd`, prefix `gar`. Not in prose here.
- **Design rulings** live in `docs/DESIGN.md`, one home.

Garnet is a CLIENT. It never commands a machine around its hub, and the
on-screen Halt is visible on every screen and never remappable away.
