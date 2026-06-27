# Visual Tab Group Headers Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add clickable visual tab group headers to the SumatraPDF tab bar and make header clicks collapse or expand grouped tabs while preserving the existing Session 2 persistence behavior.

**Architecture:** Keep `MainWindow::visualTabGroups` and `WindowTab::visualTabGroupId` as the source of truth, compute per-tab collapsed visibility in app code, and extend `TabsCtrl` to derive transient header geometry for paint and hit-testing. Avoid a second layout engine; build the feature as an overlay on the existing fixed-width tab row.

**Tech Stack:** C++, Win32 tab control wrapper, GDI+, SumatraPDF unit test harness, MSBuild / VS2022 Debug x64

---

## File Structure

- Modify: `src/MainWindow.h`
  Responsibility: add small helper APIs for collapsed-visibility decisions if needed by both app glue and tests.
- Modify: `src/MainWindow.cpp`
  Responsibility: implement helper logic that answers whether a grouped tab should remain visible when its group is collapsed.
- Modify: `src/wingui/WinGui.h`
  Responsibility: extend `TabInfo`, `TabsCtrl::MouseState`, and `TabsCtrl` event/state definitions for header geometry and header-click events.
- Modify: `src/wingui/TabsCtrl.cpp`
  Responsibility: derive visible group spans, paint headers, suppress hidden collapsed tabs, and emit header-click events.
- Modify: `src/Tabs.cpp`
  Responsibility: push collapsed visibility into `TabInfo`, handle header-click events, and refresh tab state after group collapse or selection changes.
- Modify: `src/SumatraUnitTests.cpp`
  Responsibility: cover helper-level collapse visibility behavior and protect against regressions in the chosen collapse policy.

### Task 1: Add Collapsed-Visibility Helper And Tests

**Files:**
- Modify: `src/MainWindow.h`
- Modify: `src/MainWindow.cpp`
- Modify: `src/SumatraUnitTests.cpp`

- [ ] **Step 1: Write the failing test**

Add a focused helper-level test near `visualTabGroupsTest()` in `src/SumatraUnitTests.cpp` that captures the chosen policy: tabs in expanded groups stay visible, tabs in collapsed groups hide unless they are the selected tab.

```cpp
static void visualTabGroupCollapsedVisibilityTest() {
    VisualTabGroupState state;
    VisualTabGroup* group = state.CreateGroupWithId(3, "Research", MkColor(0x33, 0x66, 0x99));

    group->collapsed = false;
    utassert(ShouldShowGroupedTab(state, group->id, false));
    utassert(ShouldShowGroupedTab(state, group->id, true));

    group->collapsed = true;
    utassert(!ShouldShowGroupedTab(state, group->id, false));
    utassert(ShouldShowGroupedTab(state, group->id, true));

    utassert(ShouldShowGroupedTab(state, -1, false));
}
```

Wire it into the unit test runner alongside `visualTabGroupsTest();`.

- [ ] **Step 2: Run test to verify it fails**

Run: `out\dbg64\test_util.exe`

Expected: FAIL with an error equivalent to `ShouldShowGroupedTab` not being declared or defined.

- [ ] **Step 3: Write minimal implementation**

Add a tiny helper declaration to `src/MainWindow.h`:

```cpp
bool ShouldShowGroupedTab(const VisualTabGroupState& state, int groupId, bool isSelectedTab);
```

Implement it in `src/MainWindow.cpp`:

```cpp
bool ShouldShowGroupedTab(const VisualTabGroupState& state, int groupId, bool isSelectedTab) {
    if (groupId < 0) {
        return true;
    }
    VisualTabGroup* group = state.FindGroup(groupId);
    if (!group) {
        return true;
    }
    if (!group->collapsed) {
        return true;
    }
    return isSelectedTab;
}
```

This gives one shared rule for app glue, `TabsCtrl` state, and tests.

- [ ] **Step 4: Run test to verify it passes**

Run: `out\dbg64\test_util.exe`

Expected: PASS, with the new visibility assertions included in the existing suite.

- [ ] **Step 5: Commit**

```bash
git add src/MainWindow.h src/MainWindow.cpp src/SumatraUnitTests.cpp
git commit -m "test: cover visual tab group collapsed visibility"
```

### Task 2: Add Header Geometry, Paint, And Hit-Testing In TabsCtrl

**Files:**
- Modify: `src/wingui/WinGui.h`
- Modify: `src/wingui/TabsCtrl.cpp`

- [ ] **Step 1: Write the failing test**

Because this behavior is UI-facing and `TabsCtrl` is not directly covered by the existing unit harness, the failing check for this task is a build-time contract: first add the API surface in the plan and call it from `Tabs.cpp` in Task 3 before implementing it here. The expected failure is unresolved members and event types.

Prepare the API shape in `src/wingui/WinGui.h`:

```cpp
struct TabInfo {
    // existing fields
    bool isHiddenByGroupCollapse = false;
    Rect rVisible;
};

struct TabsCtrl : Wnd {
    struct GroupHeaderInfo {
        int groupId = -1;
        Rect rHeader{};
        Rect rCaret{};
    };

    struct MouseState {
        int tabIdx = -1;
        bool overClose = false;
        bool inRightHalf = false;
        bool overGroupHeader = false;
        bool overGroupCaret = false;
        int groupId = -1;
        TabInfo* tabInfo = nullptr;
    };

    struct GroupHeaderClickEvent {
        TabsCtrl* tabs = nullptr;
        int groupId = -1;
    };

    using GroupHeaderClickHandler = Func1<GroupHeaderClickEvent*>;

    Vec<GroupHeaderInfo> groupHeaders;
    GroupHeaderClickHandler onGroupHeaderClick;
};
```

This intentionally creates compile pressure for the implementation work in `TabsCtrl.cpp`.

- [ ] **Step 2: Run build to verify it fails**

Run: `MSBuild.exe vs2022\SumatraPDF.vcxproj /p:Configuration=Debug;Platform=x64;BuildProjectReferences=false /v:minimal`

Expected: FAIL once Task 3 starts referencing the new fields, until `TabsCtrl.cpp` implements the new layout, paint, and event behavior.

- [ ] **Step 3: Write minimal implementation**

Implement the smallest viable `TabsCtrl` behavior in `src/wingui/TabsCtrl.cpp`:

1. Derive tab rectangles from the existing layout, but skip collapsed-hidden tabs when assigning visible x positions.
2. Compute one `GroupHeaderInfo` per contiguous visible group run.
3. Extend hit-testing so header rectangles are checked before falling through to ordinary tab hits.
4. Paint header bands after the background and before tab text.
5. On `WM_LBUTTONDOWN`, if the mouse is over a header or caret, emit `onGroupHeaderClick` and stop further tab selection or drag handling.

Use code shaped like:

```cpp
static void BuildGroupHeaders(TabsCtrl* tabs) {
    tabs->groupHeaders.Reset();
    int nTabs = tabs->TabCount();
    int currentGroupId = -1;
    TabsCtrl::GroupHeaderInfo header{};
    for (int i = 0; i < nTabs; i++) {
        TabInfo* ti = tabs->GetTab(i);
        if (ti->isHiddenByGroupCollapse || ti->visualTabGroupId < 0) {
            currentGroupId = -1;
            continue;
        }
        if (ti->visualTabGroupId != currentGroupId) {
            header = {};
            header.groupId = ti->visualTabGroupId;
            header.rHeader = ti->rVisible;
            header.rCaret = {ti->rVisible.x + DpiScale(tabs->hwnd, 4), ti->rVisible.y + DpiScale(tabs->hwnd, 3),
                             DpiScale(tabs->hwnd, 10), DpiScale(tabs->hwnd, 10)};
            tabs->groupHeaders.Append(header);
            currentGroupId = ti->visualTabGroupId;
        } else {
            TabsCtrl::GroupHeaderInfo& last = tabs->groupHeaders.Last();
            last.rHeader.dx = RectDx(ti->rVisible) + (ti->rVisible.x - last.rHeader.x);
        }
    }
}
```

And in hit-testing:

```cpp
for (auto& gh : groupHeaders) {
    if (!gh.rHeader.Contains(pt)) {
        continue;
    }
    res.overGroupHeader = true;
    res.overGroupCaret = gh.rCaret.Contains(pt);
    res.groupId = gh.groupId;
    return res;
}
```

Do not rewrite drag logic. Hidden collapsed tabs should simply never acquire visible rectangles or hit-test results.

- [ ] **Step 4: Run build to verify it passes**

Run: `MSBuild.exe vs2022\SumatraPDF.vcxproj /p:Configuration=Debug;Platform=x64;BuildProjectReferences=false /v:minimal`

Expected: PASS, with `TabsCtrl` building cleanly after the new event and header state are fully implemented.

- [ ] **Step 5: Commit**

```bash
git add src/wingui/WinGui.h src/wingui/TabsCtrl.cpp
git commit -m "feat: add visual tab group headers to tabs control"
```

### Task 3: Wire Collapse Toggling, Refresh TabInfo State, And Verify End-To-End

**Files:**
- Modify: `src/Tabs.cpp`
- Modify: `src/wingui/WinGui.h`
- Modify: `src/wingui/TabsCtrl.cpp`
- Modify: `src/SumatraUnitTests.cpp`

- [ ] **Step 1: Write the failing integration change**

In `src/Tabs.cpp`, add a helper that updates both color and collapsed visibility for every tab in the window, and wire a new group-header callback in `CreateTabbar()`.

Use a concrete shape like:

```cpp
static void UpdateAllTabVisualGroupStates(MainWindow* win) {
    if (!win || !win->tabsCtrl) {
        return;
    }
    int selectedIdx = win->tabsCtrl->GetSelected();
    for (int i = 0; i < win->TabCount(); i++) {
        WindowTab* tab = win->GetTab(i);
        TabInfo* ti = win->tabsCtrl->GetTab(i);
        if (!tab || !ti) {
            continue;
        }
        ti->tabColor = GetEffectiveTabColor(win->visualTabGroups, tab);
        ti->visualTabGroupId = tab->visualTabGroupId;
        ti->isHiddenByGroupCollapse = !ShouldShowGroupedTab(win->visualTabGroups, tab->visualTabGroupId, i == selectedIdx);
    }
    win->tabsCtrl->LayoutTabs();
    win->tabsCtrl->ScheduleRepaint();
}
```

Add the callback hook:

```cpp
static void MainWindowTabGroupHeaderClicked(MainWindow* win, TabsCtrl::GroupHeaderClickEvent* ev) {
    VisualTabGroup* group = win->visualTabGroups.FindGroup(ev->groupId);
    if (!group) {
        return;
    }
    group->collapsed = !group->collapsed;
    UpdateAllTabVisualGroupStates(win);
    SaveSettings();
}
```

And in `CreateTabbar()`:

```cpp
tabsCtrl->onGroupHeaderClick = MkFunc1(MainWindowTabGroupHeaderClicked, win);
```

At first this should fail to compile or behave correctly until all call sites refresh state after selection and grouping changes.

- [ ] **Step 2: Run build to verify it fails**

Run: `MSBuild.exe vs2022\SumatraPDF.vcxproj /p:Configuration=Debug;Platform=x64;BuildProjectReferences=false /v:minimal`

Expected: FAIL or produce obviously incomplete behavior until all tab-state refresh paths are updated.

- [ ] **Step 3: Write minimal implementation**

Finish the app-level wiring in `src/Tabs.cpp`:

1. Replace the local one-tab-only refresh helper with `UpdateAllTabVisualGroupStates()` wherever collapse can affect multiple tabs.
2. Call the full refresh helper after:
   - assigning a tab to a group
   - removing a tab from a group
   - tab selection changes
   - header click collapse toggles
   - tab migration into a new window
3. Keep `UpdateVisualTabGroupState(win, tab)` for point updates only if still useful, but ensure collapsed visibility always comes from the full-window refresh.

Use the final behavior shape:

```cpp
static void AssignTabToVisualGroup(MainWindow* win, WindowTab* tab, VisualTabGroup* group) {
    if (!win || !tab || !group) {
        return;
    }
    int oldGroupId = tab->visualTabGroupId;
    SetVisualTabGroup(tab, group);
    DeleteVisualTabGroupIfEmpty(win, oldGroupId);
    UpdateAllTabVisualGroupStates(win);
}

static void RemoveTabFromVisualGroup(MainWindow* win, WindowTab* tab) {
    if (!win || !tab) {
        return;
    }
    int oldGroupId = tab->visualTabGroupId;
    ClearVisualTabGroup(tab);
    DeleteVisualTabGroupIfEmpty(win, oldGroupId);
    UpdateAllTabVisualGroupStates(win);
}

static void MainWindowTabSelectionChanged(MainWindow* win, TabsCtrl::SelectionChangedEvent* ev) {
    bool isShowingPageInfo = (GetNotificationForGroup(win->hwndCanvas, kNotifPageInfo) != nullptr);
    int currentIdx = win->tabsCtrl->GetSelected();
    WindowTab* tab = win->Tabs()[currentIdx];
    LoadModelIntoTab(tab);
    UpdateAllTabVisualGroupStates(win);
    if (isShowingPageInfo) {
        PostMessageW(win->hwndFrame, WM_COMMAND, CmdTogglePageInfo, 0);
    }
}
```

This makes collapse state visible immediately and keeps selected-tab visibility correct.

- [ ] **Step 4: Run verification**

Run the unit tests:

`out\dbg64\test_util.exe`

Expected: PASS, including the new collapsed-visibility coverage.

Run the debug build:

`MSBuild.exe vs2022\SumatraPDF.vcxproj /p:Configuration=Debug;Platform=x64;BuildProjectReferences=false /v:minimal`

Expected: PASS.

Manual verification in the debug app:

- create a visual tab group with two or more tabs
- confirm the header band appears with the expected color and label
- click the header and confirm the group collapses
- confirm the selected tab remains visible if it belongs to the collapsed group
- select a tab outside the group and confirm the collapsed group reduces to only its header
- click again to expand
- close, reopen, and confirm the persisted `collapsed` state is restored

- [ ] **Step 5: Commit**

```bash
git add src/Tabs.cpp src/wingui/WinGui.h src/wingui/TabsCtrl.cpp src/SumatraUnitTests.cpp
git commit -m "feat: add collapsible visual tab group headers"
```

## Self-Review

- Spec coverage: this plan covers header drawing, header hit-testing, collapse toggling, selected-tab visibility rules, and persistence validation. It intentionally defers rename, recolor, keyboard navigation, and group-drag polish.
- Placeholder scan: no `TODO`, `TBD`, or deferred implementation markers remain in the execution steps.
- Type consistency: the plan consistently uses `ShouldShowGroupedTab`, `GroupHeaderInfo`, `GroupHeaderClickEvent`, `onGroupHeaderClick`, and `UpdateAllTabVisualGroupStates` across tasks.
