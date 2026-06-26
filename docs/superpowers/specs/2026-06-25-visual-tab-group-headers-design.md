# Visual Tab Group Headers Design

## Goal

Implement Session 3 of SumatraPDF visual tab grouping by drawing clickable group headers in the tab bar and wiring header clicks to collapse or expand a group, while preserving the persistence work already completed in Session 2.

## Scope

This slice adds:

- colored visual group headers in the tab bar
- header hit-testing
- collapse and expand behavior driven by header clicks
- repaint and relayout behavior that reflects collapsed state immediately

This slice does not add:

- rename or recolor UI
- drag rules specific to groups
- keyboard shortcuts for group actions
- advanced group container layout separate from the existing tab row

## Existing Context

The current branch already has:

- `MainWindow::visualTabGroups` as the live per-window group model
- `WindowTab::visualTabGroupId` as per-tab membership
- `TabInfo::visualTabGroupId` and `TabInfo::tabColor` propagated into `TabsCtrl`
- persistence for group definitions and tab membership in settings and session restore
- tab context menu commands for creating, assigning, and removing visual tab groups

The current tab control still behaves as a single fixed-width tab row:

- `TabsCtrl::LayoutTabs()` computes tab rectangles from ordered tabs
- `TabsCtrl::Paint()` renders each tab from those rectangles
- `TabsCtrl::TabStateFromMousePosition()` returns per-tab hit information
- `TabsCtrl::WndProc()` handles selection, close, dragging, and migration from tab hit results

## Chosen Approach

Use an overlay-header approach on top of the existing tab row rather than rewriting `TabsCtrl` into an explicit group-container layout engine.

The control will:

- keep the existing tab ordering and fixed-width layout path
- derive transient group-header rectangles from the current ordered tabs
- paint headers as an overlay driven by those derived rectangles
- treat header rectangles as an additional hit-test target
- hide collapsed member tabs from paint and hit-testing, except the selected tab when it belongs to the collapsed group

This keeps the change localized and avoids a broader rewrite of drag and selection behavior.

## Architecture

### Ownership

`MainWindow` remains the owner of persistent and live group state:

- `VisualTabGroupState` owns group definitions
- `VisualTabGroup.collapsed` is the source of truth for current collapse state
- `WindowTab::visualTabGroupId` remains the source of truth for membership

`TabsCtrl` remains a renderer and interaction surface:

- it does not own group definitions
- it derives header geometry from `TabInfo` values already pushed into the control
- it emits a lightweight event when a header is clicked

### Control Additions

`TabsCtrl` gains small transient structures for derived group UI state, for example:

- a header-info structure containing `groupId`, header rect, caret rect, and group span bounds
- a richer mouse-state result that can distinguish between tab hits and header hits
- an event type and callback for header-click notifications

These structures live only inside the tab control API and are recomputed from current tabs during layout or paint.

### App-Level Handling

`Tabs.cpp` handles group header actions because it already connects `TabsCtrl` events to `MainWindow` behavior.

The new handler will:

- locate the clicked `VisualTabGroup`
- toggle `collapsed`
- refresh `TabInfo` state for affected tabs
- trigger relayout and repaint

No new persistence shape is needed because `collapsed` already exists in the model and persisted settings.

## Rendering Model

### Expanded Groups

For an expanded group:

- all member tabs render normally using the existing tab path
- a colored header band is painted above the contiguous run of tabs in that group
- the header shows the group name and a collapse affordance

### Collapsed Groups

For a collapsed group:

- the header remains visible
- grouped member tabs are hidden from direct interaction and painting
- if the selected tab belongs to the group, that selected tab remains visible
- otherwise only the header is visible for that group

This is the selected collapse policy because it gives a real collapsed state without forcing a larger rewrite of tab measurement and ordering logic.

### Layout Derivation

Header geometry is derived from current tab rectangles after normal tab layout:

- find contiguous tabs sharing the same `visualTabGroupId`
- compute the group span from the first and last visible member tab
- compute a header band within that span
- reserve a small clickable caret area inside the header

The implementation should avoid storing stale rectangles across state changes. A fresh derivation each layout or paint keeps drag, selection, and restore behavior aligned with current tab order.

## Interaction Model

### Hit-Testing

`TabsCtrl::TabStateFromMousePosition()` should distinguish:

- pointer over a visible tab
- pointer over a visible tab close button
- pointer over a group header
- pointer over a group header caret
- pointer over empty tab-bar space

Collapsed hidden tabs must not be individually hit-testable.

### Click Behavior

On left click of a group header or caret:

- emit a group-header-click event with `groupId`
- do not start tab drag
- do not select a tab as a side effect

For ordinary visible tabs, existing selection, close, and drag behavior should remain unchanged.

### Context Menu and Dragging

This slice does not add group-header context menus or group dragging.

Rules:

- tab context menus continue to work for visible tabs
- hidden tabs inside collapsed groups are not reachable until the group is expanded
- drag and reorder continue to operate only on visible tabs

## Data Flow

1. `TabsCtrl` receives updated `TabInfo` membership ids from existing app-level sync.
2. During layout or paint, `TabsCtrl` derives current group header rectangles from visible tab order.
3. User clicks a header.
4. `TabsCtrl` hit-testing identifies the header and raises a header-click event carrying `groupId`.
5. `Tabs.cpp` toggles `VisualTabGroup.collapsed`.
6. App-level code refreshes affected `TabInfo` state and requests relayout and repaint.
7. `TabsCtrl` recomputes visible tab and header geometry from the new collapsed state.

## Error Handling And Edge Cases

- If a `groupId` from hit-testing no longer resolves in `MainWindow::visualTabGroups`, ignore the click.
- If a collapsed group has no selected member, render only the header.
- If the selected tab belongs to a collapsed group, keep that tab visible so selection state remains understandable.
- If a group becomes empty because tabs move or close, existing cleanup behavior should continue to remove the group.
- If tab order changes, group header spans should follow the new order automatically on next layout.
- About tabs should remain ungrouped and unaffected.

## DPI And Visual Constraints

The header band and caret need to scale using the existing DPI helpers already used in the tab control.

The first version should prioritize:

- readable group name text
- obvious color association with member tabs
- reliable hit area for the collapse affordance

It does not need pixel-perfect Chrome parity.

## Testing Strategy

### Automated

Add focused unit coverage for any pure helper logic that can be exercised outside UI message pumping, such as:

- deriving visible-membership behavior for collapsed groups
- determining whether a tab in a collapsed group should remain visible
- grouping contiguous tabs by `visualTabGroupId`

Avoid fake UI tests that only mirror implementation details.

### Manual

Verify in the debug app:

- expanded grouped tabs show a header with the correct name and color
- clicking a header collapses the group
- collapsed groups hide member tabs except the selected tab when applicable
- clicking again expands the group
- tab selection and close still work for visible tabs
- dragging visible tabs still works
- persisted collapsed state survives restart through the existing session persistence path

## Files Expected To Change

- `src/wingui/WinGui.h`
- `src/wingui/TabsCtrl.cpp`
- `src/Tabs.cpp`
- `src/MainWindow.cpp` or `src/MainWindow.h` if a small helper is needed for visible-state sync
- `src/SumatraUnitTests.cpp` for helper-level regression tests

## Non-Goals

This design intentionally defers:

- rename group UI
- recolor group UI
- group-level context menu actions
- keyboard navigation for group headers
- custom drag rules for collapsed groups
- a second independent group layout engine

## Success Criteria

The slice is complete when:

- grouped tabs display a distinct colored header in the tab bar
- header clicks toggle collapse and expand state
- collapsed groups follow the chosen visibility policy
- existing tab selection, close, and drag behavior still works for visible tabs
- existing persistence continues to restore the collapsed state correctly
