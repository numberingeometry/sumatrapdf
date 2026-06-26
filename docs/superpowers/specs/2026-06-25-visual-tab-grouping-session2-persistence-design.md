# Visual Tab Grouping Session 2 Persistence Design

Date: 2026-06-25
Repo: `C:\Users\15002\test-codex-v1\sumatrapdf`
Branch: `sumatra-tab-grouping`

## Goal

Implement only the Session 2 persistence slice for visual tab grouping.

After this slice:

- visual tab groups restore correctly across app restart
- a file reopened later outside session restore remembers its last visual group
- the existing session-style `TabGroup` feature remains unchanged
- no new header drawing, hit-testing, rename, recolor, or collapse UI is added

## Scope

In scope:

- generated settings model changes for visual tab group persistence
- saving per-window visual group definitions into session state
- saving per-tab visual group membership into session state
- saving per-document visual group membership into file history state
- restoring group definitions and membership during startup and document reopen
- regression coverage for the persistence mapping logic

Out of scope:

- tab group header rendering
- collapse or expand behavior beyond persisting the existing flag
- rename or recolor commands
- any changes to the older saved-session `TabGroup` feature in `TabGroupsManage.cpp`

## Persistence Model

The live visual grouping model stays window-local in `MainWindow::visualTabGroups`.
Persistence adds two layers:

1. Session restore layer

- `SessionData` gets a `VisualTabGroups` array
- each `TabState` gets one optional `VisualTabGroup` membership record
- this restores exact per-window group definitions and tab membership after restart

2. Per-document reopen layer

- `FileState` gets one optional `VisualTabGroup` membership record
- this restores a file's last known visual grouping even when the file is reopened later outside session restore

This duplicates some group metadata by design. The duplication is intentional because `SessionData` preserves exact window topology while `FileState` preserves reopen-later behavior for a single document.

## Generated Settings Changes

Add new generated settings structs in `cmd/gen-settings.ts`:

- `VisualTabGroup`
  - `Id: Int`
  - `Name: String`
  - `Color: Color`
  - `Collapsed: Bool`
- `VisualTabRef`
  - `GroupId: Int`
  - `Name: String`
  - `Color: Color`
  - `Collapsed: Bool`

Use them as follows:

- `SessionData.VisualTabGroups: Array<VisualTabGroup>`
- `TabState.VisualTabGroup: Struct<VisualTabRef>`
- `FileState.VisualTabGroup: Struct<VisualTabRef>`

`VisualTabRef` intentionally embeds name, color, and collapsed state instead of storing only an id. This avoids orphaned ids when a document is reopened without any saved window context and allows the window-local group definition to be reconstructed on demand.

## Save Behavior

### Session save

When `RememberSessionState()` snapshots a window:

- copy `win->visualTabGroups` into `SessionData.VisualTabGroups`
- for each saved tab, write its current `visualTabGroupId`
- if a tab belongs to a group, also write the current group name, color, and collapsed flag into the tab's `VisualTabRef`

This makes `SessionData` self-contained and avoids depending on `FileState` during restart restore.

### Per-document save

When a loaded tab updates its persistent `FileState`:

- if the tab is ungrouped, clear `FileState.VisualTabGroup`
- if the tab is grouped, write the group's id, name, color, and collapsed flag into `FileState.VisualTabGroup`

For lazy tabs, when their state is cloned through `TabState`, preserve the same `VisualTabRef` data so re-saving does not discard membership.

## Restore Behavior

### Restart restore

When rebuilding a window from `SessionData`:

- rebuild `win->visualTabGroups` first from `SessionData.VisualTabGroups`
- for each restored tab, apply its `TabState.VisualTabGroup`
- if the referenced group id exists in the window state, assign the tab to that group
- if the group id is missing but the tab carries embedded metadata, create the group definition on demand and then assign the tab

The fallback creation path handles partial or older saved states robustly.

### Reopen later outside session restore

When opening a document from normal file history:

- read `FileState.VisualTabGroup`
- if absent, keep the tab ungrouped
- if present, ensure the current window has a matching group definition
- reuse an existing group with the same id if present
- otherwise create a new window-local group from the persisted metadata and assign the tab

This preserves the user's last known grouping without introducing a global cross-window registry.

## Data Flow and Ownership

- Runtime owner: `MainWindow::visualTabGroups`
- Runtime tab membership owner: `WindowTab::visualTabGroupId`
- Session persistence owner: `SessionData` plus `TabState`
- Per-document persistence owner: `FileState`

No runtime code should start reading persisted settings objects directly after restoration completes. Persisted structs are only an import and export format.

## Error Handling and Compatibility

- Missing or malformed visual group data should fall back to an ungrouped tab
- Unknown group ids should trigger on-demand group creation only if embedded metadata is present
- Existing settings files without any visual tab group fields must continue to load with default empty values
- Saved-session `TabGroup` data remains untouched and semantically separate

## Verification Plan

Implement this slice with TDD for the persistence mapping helpers:

1. Add a failing unit test for round-tripping a grouped tab through the save model
2. Add a failing unit test for restoring a window's group definitions and membership from persisted state
3. Add a failing unit test for reopening a file outside session restore using only `FileState.VisualTabGroup`
4. Regenerate settings code and build the app sources
5. Run `out\dbg64\test_util.exe`

Manual smoke check after build:

- create a visual tab group
- close and reopen the app with session restore enabled
- verify membership and color return
- close the app, reopen one previously grouped file outside session restore, and verify it rejoins a reconstructed visual group

## Risks

- `FileState` and `TabState` are shared with several restore paths, so partial updates can silently drop persistence unless both save and load paths are covered
- generated settings changes can ripple into serialization metadata, so generator output must be treated as part of the change, not hand-edited
- duplicate group ids across unrelated windows are acceptable because runtime groups remain window-local

## Stop Condition

Stop this session after persistence works and tests pass.

Do not begin header drawing, group hit-testing, rename, recolor, or collapse behavior changes in the same implementation pass.
