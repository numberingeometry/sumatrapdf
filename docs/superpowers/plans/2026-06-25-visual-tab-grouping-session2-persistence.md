# Visual Tab Grouping Session 2 Persistence Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Persist visual tab groups across restart and per-document reopen without changing drawing or group-management UI beyond existing Session 1 behavior.

**Architecture:** Keep the live visual grouping model window-local in `MainWindow::visualTabGroups` and persist it through generated settings structs attached to `SessionData`, `TabState`, and `FileState`. Restore paths rebuild runtime groups from persisted metadata and then reattach tabs by group id, with a fallback that reconstructs missing group definitions from per-tab or per-file metadata.

**Tech Stack:** C++, Win32, SumatraPDF settings generator in `cmd/gen-settings.ts`, Bun-based code generation, Sumatra unit tests, MSBuild.

---

## File Structure

- Modify: `cmd/gen-settings.ts`
  Responsibility: define new generated persistence structs and fields for `FileState`, `TabState`, and `SessionData`.
- Modify: `src/Settings.h`
  Responsibility: generated output for the new settings structs and metadata.
- Modify: `src/GlobalPrefs.cpp`
  Responsibility: ensure `NewTabState()` copies visual tab group membership from `FileState`.
- Modify: `src/AppSettings.cpp`
  Responsibility: clone visual tab persistence fields and save session group definitions and per-tab membership.
- Modify: `src/SumatraPDF.cpp`
  Responsibility: persist per-document visual group membership into `FileState` and restore from `FileState` during reopen flows.
- Modify: `src/SumatraStartup.cpp`
  Responsibility: rebuild window-local visual groups from `SessionData` and restore per-tab membership on startup.
- Modify: `src/MainWindow.h`
  Responsibility: expose small helper declarations for converting between runtime and persisted visual group structs if needed.
- Modify: `src/MainWindow.cpp`
  Responsibility: implement small helper functions for copying runtime visual group state to and from persisted structs.
- Modify: `src/SumatraUnitTests.cpp`
  Responsibility: add persistence-focused tests for round-trip save and restore mapping.

### Task 1: Add failing persistence unit tests

**Files:**
- Modify: `src/SumatraUnitTests.cpp`
- Read: `src/MainWindow.h`
- Read: `src/Settings.h`

- [ ] **Step 1: Write the failing test for copying runtime group data into persisted tab and file state**

Add a new test block near `visualTabGroupsTest()` that exercises helper functions instead of UI:

```cpp
static void visualTabGroupPersistenceTest() {
    VisualTabGroupState state;
    VisualTabGroup* group = state.CreateGroupWithId(7, "Research", RGB(0x11, 0x22, 0x33));
    group->collapsed = true;

    WindowTab tab(nullptr);
    SetVisualTabGroup(&tab, group);

    FileState* fs = NewFileState("C:\\docs\\a.pdf");
    TabState* ts = NewTabState(fs);

    SaveVisualTabGroupToFileState(state, &tab, fs);
    SaveVisualTabGroupToTabState(state, &tab, ts);

    utassert(fs->visualTabGroup.groupId == 7);
    utassert(str::Eq(fs->visualTabGroup.name, "Research"));
    utassert(fs->visualTabGroup.colorParsed->col == RGB(0x11, 0x22, 0x33));
    utassert(fs->visualTabGroup.collapsed == true);

    utassert(ts->visualTabGroup.groupId == 7);
    utassert(str::Eq(ts->visualTabGroup.name, "Research"));

    DeleteFileState(fs);
    DeleteTabState(ts);
}
```

- [ ] **Step 2: Run the unit test target to verify it fails**

Run: `out\dbg64\test_util.exe`

Expected: build or test failure referencing missing `visualTabGroup` persistence fields or missing helper functions such as `SaveVisualTabGroupToFileState`.

- [ ] **Step 3: Write the failing test for restoring from `SessionData` and `FileState`**

Extend the same test function with restore assertions:

```cpp
    VisualTabGroupState restoredState;
    RestoreVisualTabGroupFromTabState(restoredState, ts);
    utassert(restoredState.FindGroup(7) != nullptr);

    WindowTab reopened(nullptr);
    RestoreVisualTabGroupFromFileState(restoredState, &reopened, fs);
    utassert(reopened.visualTabGroupId == 7);
```

- [ ] **Step 4: Run the unit test target again to verify it still fails for the expected missing behavior**

Run: `out\dbg64\test_util.exe`

Expected: failure still points to missing fields or restore helpers, not unrelated crashes.

### Task 2: Add generated settings fields for visual group persistence

**Files:**
- Modify: `cmd/gen-settings.ts`
- Modify: `src/Settings.h`

- [ ] **Step 1: Add the new generated structs in `cmd/gen-settings.ts`**

Insert new field definitions next to the existing tab/session structs:

```ts
const visualTabGroup: Field[] = [
  mkField("Id", Int, -1, "stable visual tab group id"),
  mkField("Name", Str, "", "name of the visual tab group"),
  mkField("Color", Color, "", "color of the visual tab group"),
  mkField("Collapsed", Bool, false, "if true, the visual tab group is collapsed"),
];

const visualTabRef: Field[] = [
  mkField("GroupId", Int, -1, "visual tab group id for this tab or file"),
  mkField("Name", Str, "", "name of the referenced visual tab group"),
  mkField("Color", Color, "", "color of the referenced visual tab group"),
  mkField("Collapsed", Bool, false, "collapsed flag copied from the visual tab group"),
];
```

- [ ] **Step 2: Attach the new fields to `FileState`, `TabState`, and `SessionData`**

Add these entries in `cmd/gen-settings.ts`:

```ts
  setStructName(mkStruct("VisualTabGroup", visualTabRef, "persisted visual tab group membership"), "VisualTabRef"),
```

and append these fields:

```ts
  setStructName(mkStruct("VisualTabGroup", visualTabRef, "persisted visual tab group membership"), "VisualTabRef"),
```

to `fileSettings` and `tabState`, plus:

```ts
  setVersion(
    setStructName(mkArray("VisualTabGroups", visualTabGroup, "visual tab groups for this window"), "VisualTabGroup"),
    "3.7",
  ),
```

to `sessionData`.

- [ ] **Step 3: Regenerate `src/Settings.h`**

Run: `bun cmd/gen-settings.ts`

Expected: `src/Settings.h` now contains `struct VisualTabGroup`, `struct VisualTabRef`, and the new metadata fields on `FileState`, `TabState`, and `SessionData`.

- [ ] **Step 4: Verify the generated output contains the new types**

Run: `rg -n "struct VisualTabGroup|struct VisualTabRef|visualTabGroup|visualTabGroups" src/Settings.h`

Expected: matches for both new struct definitions and the new field metadata.

### Task 3: Implement persistence helper functions and clone support

**Files:**
- Modify: `src/MainWindow.h`
- Modify: `src/MainWindow.cpp`
- Modify: `src/GlobalPrefs.cpp`
- Modify: `src/AppSettings.cpp`

- [ ] **Step 1: Add helper declarations for persisting and restoring visual groups**

Add declarations in `src/MainWindow.h` near the existing visual tab group helpers:

```cpp
void SaveVisualTabGroupToFileState(const VisualTabGroupState& state, const WindowTab* tab, FileState* fs);
void SaveVisualTabGroupToTabState(const VisualTabGroupState& state, const WindowTab* tab, TabState* ts);
void RestoreVisualTabGroupFromFileState(MainWindow* win, WindowTab* tab, const VisualTabRef& ref);
void RestoreVisualTabGroupFromTabState(MainWindow* win, WindowTab* tab, const VisualTabRef& ref);
void SaveVisualTabGroupsToSessionData(const VisualTabGroupState& state, SessionData* sd);
void RestoreVisualTabGroupsFromSessionData(MainWindow* win, const SessionData* sd);
```

- [ ] **Step 2: Implement the minimal conversion helpers in `src/MainWindow.cpp`**

Add focused helpers that copy fields and reconstruct groups on demand:

```cpp
static void ClearVisualTabRef(VisualTabRef& ref) {
    str::ReplaceWithCopy(&ref.name, nullptr);
    str::ReplaceWithCopy(&ref.color, nullptr);
    ref.groupId = -1;
    ref.collapsed = false;
}

static void SaveVisualTabRef(const VisualTabGroupState& state, const WindowTab* tab, VisualTabRef& ref) {
    ClearVisualTabRef(ref);
    if (!tab || tab->visualTabGroupId == -1) {
        return;
    }
    VisualTabGroup* group = state.FindGroup(tab->visualTabGroupId);
    if (!group) {
        return;
    }
    ref.groupId = group->id;
    str::ReplaceWithCopy(&ref.name, group->name);
    SerializeColor(&ref.color, group->color);
    ref.collapsed = group->collapsed;
}
```

and implement restore by finding or creating a matching runtime group before calling `SetVisualTabGroup(tab, group)`.

- [ ] **Step 3: Update `NewTabState()` or clone logic to copy the new `VisualTabRef`**

In `src/GlobalPrefs.cpp`, after `TabState* state = (TabState*)DeserializeStruct(&gTabStateInfo, nullptr);`, preserve the file-level membership:

```cpp
    state->visualTabGroup.groupId = fs->visualTabGroup.groupId;
    str::ReplaceWithCopy(&state->visualTabGroup.name, fs->visualTabGroup.name);
    str::ReplaceWithCopy(&state->visualTabGroup.color, fs->visualTabGroup.color);
    state->visualTabGroup.collapsed = fs->visualTabGroup.collapsed;
```

- [ ] **Step 4: Update `CloneTabState()` and `CloneSessionData()` to carry the new fields**

Add the copies in `src/AppSettings.cpp`:

```cpp
    dst->visualTabGroup.groupId = src->visualTabGroup.groupId;
    str::ReplaceWithCopy(&dst->visualTabGroup.name, src->visualTabGroup.name);
    str::ReplaceWithCopy(&dst->visualTabGroup.color, src->visualTabGroup.color);
    dst->visualTabGroup.collapsed = src->visualTabGroup.collapsed;
```

and copy `SessionData::visualTabGroups` entries when cloning session data.

- [ ] **Step 5: Run the unit test target to verify progress**

Run: `out\dbg64\test_util.exe`

Expected: earlier missing-symbol failures move to save/restore call sites or remaining runtime wiring gaps.

### Task 4: Wire session save and startup restore

**Files:**
- Modify: `src/AppSettings.cpp`
- Modify: `src/SumatraStartup.cpp`

- [ ] **Step 1: Save visual group definitions and per-tab membership in `RememberSessionState()`**

Insert calls in `src/AppSettings.cpp`:

```cpp
        SaveVisualTabGroupsToSessionData(win->visualTabGroups, windowState);
```

and for each tab:

```cpp
            TabState* ts = NewTabState(fs);
            SaveVisualTabGroupToTabState(win->visualTabGroups, tab, ts);
            windowState->tabStates->Append(ts);
```

For lazy tabs that clone `tab->tabState`, preserve their existing `visualTabGroup`.

- [ ] **Step 2: Restore window group definitions before tabs are attached**

In `src/SumatraPDF.cpp` or `src/SumatraStartup.cpp`, after creating a window from `SessionData`, restore the group list first:

```cpp
    if (data) {
        RestoreVisualTabGroupsFromSessionData(win, data);
    }
```

- [ ] **Step 3: Restore each tab's membership from `TabState` during startup**

In `SetTabState()` or the startup restore path, call:

```cpp
    RestoreVisualTabGroupFromTabState(tab->win, tab, state->visualTabGroup);
```

only after `tab->win` is valid.

- [ ] **Step 4: Run the unit test target to verify session restore path passes**

Run: `out\dbg64\test_util.exe`

Expected: persistence test reaches the file-history reopen path or passes if both restore paths are complete.

### Task 5: Wire per-document file history persistence and reopen restore

**Files:**
- Modify: `src/SumatraPDF.cpp`
- Modify: `src/Tabs.cpp`

- [ ] **Step 1: Persist group membership into `FileState` whenever display state is updated**

In `UpdateTabFileDisplayStateForTab()` after `tab->ctrl->GetDisplayState(fs);`, add:

```cpp
    MainWindow* win = tab->win;
    if (win) {
        SaveVisualTabGroupToFileState(win->visualTabGroups, tab, fs);
    }
```

- [ ] **Step 2: Restore group membership when a file is opened from history**

In the code path that applies `FileState` values to a new tab, add:

```cpp
    if (tab->win) {
        RestoreVisualTabGroupFromFileState(tab->win, tab, fs->visualTabGroup);
    }
```

Use the same on-demand group creation fallback as the session restore path.

- [ ] **Step 3: Keep explicit ungroup operations clearing persisted file history**

Ensure any existing code that ungroups a tab still reaches the updated `UpdateTabFileDisplayStateForTab()` path so `FileState.visualTabGroup` is cleared on the next save. No extra UI changes are needed.

- [ ] **Step 4: Run the unit test target to verify all persistence tests pass**

Run: `out\dbg64\test_util.exe`

Expected: `Passed all ... tests`, including the new persistence assertions.

### Task 6: Build verification and smoke checks

**Files:**
- Modify: none
- Verify: generated and source changes from previous tasks

- [ ] **Step 1: Format touched source files**

Run:

```powershell
clang-format -i src/MainWindow.h src/MainWindow.cpp src/GlobalPrefs.cpp src/AppSettings.cpp src/SumatraPDF.cpp src/SumatraStartup.cpp src/SumatraUnitTests.cpp
```

Expected: no output, files reformatted in place.

- [ ] **Step 2: Compile the app sources**

Run:

```powershell
MSBuild.exe vs2022\SumatraPDF.vcxproj /t:ClCompile /p:Configuration=Debug;Platform=x64 /m
```

Expected: compile succeeds for the touched app sources.

- [ ] **Step 3: Run unit tests again after formatting and compile**

Run: `out\dbg64\test_util.exe`

Expected: all tests pass.

- [ ] **Step 4: Manually smoke-check the feature**

Run:

```powershell
.\out\dbg64\SumatraPDF-dll.exe -for-testing
```

Manual check:

- create a visual group for one tab
- close and reopen the app with session restore enabled outside `-for-testing`
- verify the grouped tab restores with the same color
- reopen one previously grouped file from recent files in a clean window
- verify it reconstructs a matching visual group in that window

Expected: both restart restore and reopen-later behavior work without any new drawing UI.

## Self-Review

- Spec coverage: generator changes, session restore, per-document reopen, compatibility, and tests are all mapped to tasks above.
- Placeholder scan: no `TODO` or undefined “test later” steps remain.
- Type consistency: the plan uses `VisualTabGroup`, `VisualTabRef`, `SaveVisualTabGroupToFileState`, `SaveVisualTabGroupToTabState`, `RestoreVisualTabGroupFromFileState`, `RestoreVisualTabGroupFromTabState`, and `SaveVisualTabGroupsToSessionData` consistently across tasks.
