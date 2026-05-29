<!-- index-summary: tombstone — plans moved to docs/plans/ -->
# `docs/design/` moved → `docs/plans/`

Plan docs were reorganized (agentic-layer-project-independence, Phase D):

- **Active/working plans** → [`docs/plans/active/`](../plans/active/)
- **Shipped/archived plans** → [`docs/plans/shipped/`](../plans/shipped/)
- **Index of shipped plans** → [`docs/plans/INDEX.md`](../plans/INDEX.md) (auto-generated)

New plans go to `docs/plans/active/<slug>.md`; once shipped, `git mv` to
`docs/plans/shipped/` and the index row appears automatically. This tombstone is
removable after a grace period.
