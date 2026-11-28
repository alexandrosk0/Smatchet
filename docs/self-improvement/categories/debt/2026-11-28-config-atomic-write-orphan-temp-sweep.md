# Orphaned config temp files are never swept

- **Category**: debt
- **Priority**: P3
- **Date**: 2026-11-28
- **Status**: open

## What happened

`ConfigManager::AtomicWriteTextFile` (via `fileio::WriteFileAtomically`) now writes through unique `<path>.part-<pid>-<n>` temp siblings instead of a reused `<path>.tmp`. A crash between the temp write and the rename leaves an owner-only copy of the config content (which can hold secrets) beside the config file, and nothing removes it. The attachment and icon caches sweep their own orphans; the config folder does not.

## Concrete next action

Add a startup sweep of the config folder using `fileio::IsTempSiblingOf` for each config file name, with a grace age so a concurrent writer's live temp is never removed.
