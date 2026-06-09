# Sourcetrail code-graph index (`st_query.py`)

A libclang-built symbol graph of the Smatchet C++ tree, queryable from the CLI
for **exact** navigation facts — definition sites, call graph (callers/callees
with call-site line numbers), class members, inheritance. Built by
[Sourcetrail](https://github.com/CoatiSoftware/Sourcetrail) (2021.4.19, the
final release) from the project's compilation database.

## Where this sits vs. grep

This is the project's primary **code-navigation** tool on Claude Code
(`AGENTS.md` § Semantic codebase search) — it answers the call-graph /
definition questions grep can't, and is the first stop before raw text-search:

| Need | Use |
|---|---|
| Exact call graph, definition site, members, inheritance | **`st_query.py`** (this tool) |
| Exhaustive literal/symbol inventory, mechanical rename sweep | text-search (`grep` / `Grep`) |

`st_query.py` answers the questions a grep can't (who *calls* this, not who
*mentions* it) without the cost of re-parsing — it reads a prebuilt SQLite graph.

## Local-only artifacts

The project file and index are **git-ignored** (`.git/info/exclude`) — personal
tooling, 49 MB binary, machine-specific paths. Not shared, not committed:

- `Smatchet.srctrlprj` — project definition (CDB source group, repo root)
- `Smatchet.srctrldb` / `.srctrldb_tmp` — the SQLite index

The `tools/sourcetrail/st_query.py` helper itself is tracked-eligible (portable;
reads whatever `.srctrldb` is present).

## Usage

```bash
python tools/sourcetrail/st_query.py <subcommand> <arg> [--limit N] [--db PATH]
```

| Subcommand | Answers |
|---|---|
| `def <name>` | where is `<name>` defined → `file:line` |
| `refs <name>` | every reference/use of `<name>` |
| `callers <name>` | functions that **call** `<name>` (caller @ call-site) |
| `callees <name>` | functions `<name>` calls (callee @ call-site) |
| `members <name>` | members of a class/struct/namespace/enum |
| `bases <name>` | base classes of `<name>` |
| `derived <name>` | classes deriving from `<name>` |
| `file <path-frag>` | symbols defined in matching file(s) |
| `name <substr>` | symbols whose qualified name contains `<substr>` |
| `schema` | table counts + NodeKind/EdgeKind legend |

Name matching is a case-insensitive suffix match on the qualified name, so
`send`, `JiraClient::send`, and the fully-qualified form all resolve. Unsure of
spelling? Run `name <substr>` first.

```bash
# Who calls NormalizeBaseUrl, and from where?
python tools/sourcetrail/st_query.py callers NormalizeBaseUrl

# What does JiraClient::BuildBrowseUrl call?
python tools/sourcetrail/st_query.py callees JiraClient::BuildBrowseUrl

# Every member of the JiraClient class
python tools/sourcetrail/st_query.py members JiraClient
```

Default DB: `<repo-root>/Smatchet.srctrldb`. Override with `--db` or
`$SMATCHET_SRCTRLDB`.

## Re-indexing (after code changes)

The index is a snapshot — stale after edits. Rebuild with the Sourcetrail CLI
(the GUI cannot be running on the same project):

```powershell
& "C:\Program Files\Sourcetrail\Sourcetrail.exe" index --full Smatchet.srctrlprj
```

Indexing reads `build/ninja-iter-msvc/compile_commands.json` — regenerate that
CDB first (`cmake --preset ninja-iter-msvc`) if source files were added/removed.
A full index of the ~480 app TUs takes ≈5 min. Incremental (`index` without
`--full`) only re-touches changed TUs.

> **Why the MSVC CDB, not clang?** Every `ninja-clang-*` / asan preset stages a
> flattened copy of the tree (`Source_Core/`, `Target_Standalone/` phantom
> paths) that doesn't exist on disk — Sourcetrail would index ~40% missing
> files. Only the `cl.exe` MSVC presets map to the real working tree. The
> bundled libclang indexer handles an MSVC `cl.exe` database fine.

## Open in the GUI

`Sourcetrail.exe` (no args) → File ▸ Open Project ▸ `Smatchet.srctrlprj` for the
visual graph/code explorer. Same `.srctrldb` the CLI queries.

## Notes / limits

- **5428 indexer errors (3 fatal)** on the last build — normal for a partial
  C++ index (unresolved third-party templates, missing system headers). The
  graph is still 17.9 k nodes / 100 k edges; app symbols resolve cleanly.
- `file` table includes pulled-in third-party headers (cpr, nlohmann) recorded
  via INCLUDE edges; query output filters to `Source/` paths where it matters.
- Call edges into `std::` / template instantiations appear in `callees` — real,
  if noisy. Filter mentally to first-party frames.
