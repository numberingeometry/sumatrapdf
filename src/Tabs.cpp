/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/Dpi.h"
#include "utils/FileUtil.h"
#include "utils/WinUtil.h"

#include "wingui/UIModels.h"
#include "wingui/Layout.h"
#include "wingui/WinGui.h"

#include "Settings.h"
#include "AppSettings.h"
#include "DocController.h"
#include "EngineBase.h"
#include "EngineAll.h"
#include "DisplayModel.h"
#include "GlobalPrefs.h"
#include "SumatraPDF.h"
#include "SumatraProperties.h"
#include "Notifications.h"
#include "MainWindow.h"
#include "WindowTab.h"
#include "Commands.h"
#include "CommandAvailability.h"
#include "FindBar.h"
#include "Menu.h"
#include "TableOfContents.h"
#include "Tabs.h"
#include "SumatraDialogs.h"
#include "FileHistory.h"
#include "Theme.h"
#include "Translations.h"

#include "utils/Log.h"

static void UpdateTabTitle(WindowTab* tab) {
    if (!tab) {
        return;
    }
    MainWindow* win = tab->win;
    int idx = win->GetTabIdx(tab);
    const char* title = tab->GetTabTitle();
    // respect FullPathInTitle for the tab tooltip too: when the user opted out
    // of showing the full path, don't reveal it on hover either (#3024)
    const char* tooltip = tab->filePath;
    if (tooltip && !gGlobalPrefs->fullPathInTitle) {
        tooltip = path::GetBaseNameTemp(tooltip);
    }
    win->tabsCtrl->SetTextAndTooltip(idx, title, tooltip);
}

int GetTabbarHeight(HWND hwnd, float factor) {
    int tabDy = DpiScale(hwnd, kTabBarDy);
    HFONT hfont = GetAppFont();
    int fontDyWithPadding = FontDyPx(hwnd, hfont) + DpiScale(hwnd, 2);
    if (fontDyWithPadding > tabDy) {
        tabDy = fontDyWithPadding;
    }
    // guard against bad per-window DPI (e.g. under Wine)
    int minDy = DpiScale(HWND_DESKTOP, kTabBarDy);
    int minFontDy = FontDyPx(hwnd, hfont) + DpiScale(HWND_DESKTOP, 2);
    if (minFontDy > minDy) {
        minDy = minFontDy;
    }
    if (tabDy < minDy) {
        tabDy = minDy;
    }
    int res = (int)((float)tabDy * factor);
    if (IsRunningOnWine()) {
        int dpi = DpiGet(hwnd);
        int desktopDpi = DpiGet(HWND_DESKTOP);
        logf(
            "GetTabbarHeight: hwnd=%p factor=%g dpi=%d desktopDpi=%d tabDyScaled=%d fontDy=%d "
            "minDy=%d result=%d\n",
            hwnd, factor, dpi, desktopDpi, DpiScale(hwnd, kTabBarDy), fontDyWithPadding, minDy, res);
    }
    return res;
}

#if 0
static inline Size GetTabSize(HWND hwnd) {
    int dx = DpiScale(hwnd, std::max(gGlobalPrefs->tabWidth, kTabMinDx));
    int dy = GetTabbarHeight(hwnd);
    return Size(dx, dy);
}
#endif

static void ShowTabBar(MainWindow* win, bool show) {
    if (show == win->tabsVisible) {
        return;
    }
    win->tabsVisible = show;
    if (win->tabsCtrl) {
        win->tabsCtrl->SetIsVisible(show);
    }
    RelayoutWindow(win);
}

void UpdateTabWidth(MainWindow* win) {
    int nTabs = (int)win->TabCount();
    bool showSingleTab = SettingsUseTabs() || win->tabsInTitlebar;
    bool showTabs = (nTabs > 1) || (showSingleTab && (nTabs > 0));
    int tabWidth = gGlobalPrefs->tabWidth;
    if (win->tabsCtrl) {
        win->tabsCtrl->tabDefaultDx = tabWidth;
    }
    if (!showTabs) {
        ShowTabBar(win, false);
        return;
    }
    ShowTabBar(win, true);
}

constexpr UINT_PTR CmdCreateVisualTabGroup = 50000;
constexpr UINT_PTR CmdRemoveVisualTabGroup = 50001;
constexpr UINT_PTR CmdRenameVisualTabGroup = 50002;
constexpr UINT_PTR CmdRecolorVisualTabGroup = 50003;
constexpr UINT_PTR CmdToggleVisualTabGroupCollapse = 50004;
constexpr UINT_PTR CmdCloseVisualTabGroup = 50005;
constexpr UINT_PTR CmdDisbandVisualTabGroup = 50006;
constexpr UINT_PTR CmdAddToVisualTabGroupBase = 50100;

static void DeleteVisualTabGroupIfEmpty(MainWindow* win, int groupId) {
    if (!win || groupId == -1) {
        return;
    }
    for (WindowTab* tab : win->Tabs()) {
        if (tab->visualTabGroupId == groupId) {
            return;
        }
    }
    win->visualTabGroups.DeleteGroupById(groupId);
}

static VisualTabGroup* CloneVisualTabGroupToWindow(MainWindow* win, const VisualTabGroup* src) {
    if (!win || !src) {
        return nullptr;
    }
    VisualTabGroup* existing = win->visualTabGroups.FindGroup(src->id);
    if (existing) {
        return existing;
    }
    return win->visualTabGroups.CreateGroupWithId(src->id, src->name, src->color);
}

static void UpdateTabVisualGroupState(MainWindow* win, WindowTab* tab) {
    if (!win || !tab || !win->tabsCtrl) {
        return;
    }
    int idx = win->GetTabIdx(tab);
    if (idx < 0) {
        return;
    }
    TabInfo* ti = win->tabsCtrl->GetTab(idx);
    if (!ti) {
        return;
    }
    // grouped tabs are NOT tinted; membership is shown by the chip + underline.
    // keep the tab's own (manual) color, if any.
    ti->tabColor = tab->tabColor;
    ti->visualTabGroupId = tab->visualTabGroupId;
}

// keep a group's tabs contiguous: if `tab` joined a group whose other members live
// elsewhere in the tab strip (e.g. a reopened closed tab appended at the end), move it
// next to them so the layout draws one chip, not a split group.
void EnsureVisualTabGroupContiguity(MainWindow* win, WindowTab* tab) {
    if (!win || !tab || !win->tabsCtrl) {
        return;
    }
    int groupId = tab->visualTabGroupId;
    if (groupId == -1) {
        return;
    }
    int cur = win->GetTabIdx(tab);
    if (cur < 0) {
        return;
    }
    int n = win->TabCount();
    int anchor = -1; // last index (other than `tab`) that belongs to the same group
    for (int i = 0; i < n; i++) {
        if (i == cur) {
            continue;
        }
        if (win->GetTab(i)->visualTabGroupId == groupId) {
            anchor = i;
        }
    }
    if (anchor < 0) {
        return; // only member; nothing to gather
    }
    if (cur == anchor + 1) {
        return; // already directly after the group's run
    }
    win->tabsCtrl->MoveTabToIndex(cur, anchor + 1);
}

static void AssignTabToVisualGroup(MainWindow* win, WindowTab* tab, VisualTabGroup* group) {
    if (!win || !tab || !group) {
        return;
    }
    int oldGroupId = tab->visualTabGroupId;
    SetVisualTabGroup(tab, group);
    UpdateTabVisualGroupState(win, tab);
    DeleteVisualTabGroupIfEmpty(win, oldGroupId);
    // LayoutTabs() rebuilds the group header bands (fills groupHeaders); without it
    // the membership change repaints with a stale (empty) header list and no band shows
    win->tabsCtrl->LayoutTabs();
    win->tabsCtrl->ScheduleRepaint();
}

static void RemoveTabFromVisualGroup(MainWindow* win, WindowTab* tab) {
    if (!win || !tab) {
        return;
    }
    int oldGroupId = tab->visualTabGroupId;
    ClearVisualTabGroup(tab);
    UpdateTabVisualGroupState(win, tab);
    DeleteVisualTabGroupIfEmpty(win, oldGroupId);
    // LayoutTabs() rebuilds the group header bands (fills groupHeaders); without it
    // the membership change repaints with a stale header list
    win->tabsCtrl->LayoutTabs();
    win->tabsCtrl->ScheduleRepaint();
}

// Sync each tab-bar item's collapse-hidden flag from its group's collapsed
// state, then re-layout. A collapsed group hides all its tabs except the
// selected one (so the active document stays reachable), matching
// ShouldShowGroupedTab().
void ApplyVisualTabGroupCollapse(MainWindow* win) {
    if (!win || !win->tabsCtrl) {
        return;
    }
    TabsCtrl* tabsCtrl = win->tabsCtrl;
    int selectedIdx = tabsCtrl->GetSelected();
    int nTabs = tabsCtrl->TabCount();
    for (int i = 0; i < nTabs; i++) {
        TabInfo* ti = tabsCtrl->GetTab(i);
        if (!ti) {
            continue;
        }
        bool isSelected = (i == selectedIdx);
        bool show = ShouldShowGroupedTab(win->visualTabGroups, ti->visualTabGroupId, isSelected);
        ti->isHiddenByGroupCollapse = !show;
    }
    tabsCtrl->StartTabAnimation(); // slide tabs as the group collapses/expands
    tabsCtrl->LayoutTabs();
    tabsCtrl->ScheduleRepaint();
}

static VisualTabGroup* GetVisualTabGroupForMenuCmd(MainWindow* win, WindowTab* currentTab, UINT_PTR cmdId) {
    int idx = (int)(cmdId - CmdAddToVisualTabGroupBase);
    if (idx < 0) {
        return nullptr;
    }
    int seen = 0;
    for (VisualTabGroup* group : win->visualTabGroups.groups) {
        if (group->id == currentTab->visualTabGroupId) {
            continue;
        }
        if (seen == idx) {
            return group;
        }
        seen++;
    }
    return nullptr;
}

static void AddVisualTabGroupMenuItems(HMENU popup, MainWindow* win, WindowTab* tabUnderMouse) {
    UINT flags = MF_BYCOMMAND | MF_STRING | MF_ENABLED;
    InsertMenuW(popup, CmdSetTabColor, MF_BYCOMMAND | MF_SEPARATOR, 0, nullptr);
    if (tabUnderMouse->visualTabGroupId != -1) {
        InsertMenuW(popup, CmdSetTabColor, flags, CmdRemoveVisualTabGroup, ToWStrTemp(_TRN("Remove from Group")));
        InsertMenuW(popup, CmdSetTabColor, flags, CmdRenameVisualTabGroup, ToWStrTemp(_TRN("Rename Tab Group")));
        InsertMenuW(popup, CmdSetTabColor, flags, CmdRecolorVisualTabGroup, ToWStrTemp(_TRN("Set Group Color")));
    }

    HMENU addToGroup = CreatePopupMenu();
    bool hasGroups = false;
    int menuIdx = 0;
    for (VisualTabGroup* group : win->visualTabGroups.groups) {
        if (group->id == tabUnderMouse->visualTabGroupId) {
            continue;
        }
        const char* gname = str::IsEmpty(group->name) ? str::FormatTemp("Group %d", group->id) : group->name;
        AppendMenuW(addToGroup, MF_STRING | MF_ENABLED, CmdAddToVisualTabGroupBase + menuIdx, ToWStrTemp(gname));
        menuIdx++;
        hasGroups = true;
    }
    if (!hasGroups) {
        AppendMenuW(addToGroup, MF_STRING | MF_GRAYED, 0, ToWStrTemp(_TRN("(No groups)")));
    }
    UINT submenuFlags = MF_BYCOMMAND | MF_POPUP | (hasGroups ? MF_ENABLED : MF_GRAYED);
    InsertMenuW(popup, CmdSetTabColor, submenuFlags, (UINT_PTR)addToGroup, ToWStrTemp(_TRN("Add To Tab Group")));
    InsertMenuW(popup, CmdSetTabColor, flags, CmdCreateVisualTabGroup, ToWStrTemp(_TRN("New Tab Group")));
}

void RemoveTab(WindowTab* tab) {
    UpdateTabFileDisplayStateForTab(tab);
    MainWindow* win = tab->win;
    int groupId = tab->visualTabGroupId;
    win->tabSelectionHistory->Remove(tab);
    int idx = win->GetTabIdx(tab);
    WindowTab* tab2 = win->tabsCtrl->RemoveTab<WindowTab*>(idx);
    ReportIf(tab != tab2);
    DeleteVisualTabGroupIfEmpty(win, groupId);
    bool closedCurrentTab = (tab == win->CurrentTab());
    if (closedCurrentTab) {
        win->ctrl = nullptr;
        win->currentTabTemp = nullptr;
    }
    UpdateTabWidth(win);

    int nTabs = win->TabCount();
    if (nTabs < 1) {
        return;
    }
    if (!closedCurrentTab) {
        return;
    }
    // if the removed tab was the current one, select another
#if 0
    WindowTab* curr = win->CurrentTab();
    WindowTab* newCurrent = curr;
    if (!curr || newCurrent == tab) {
        // TODO(tabs): why do I need win->tabSelectionHistory.Size() > 0
        if (win->tabSelectionHistory->Size() > 0) {
            newCurrent = win->tabSelectionHistory->Pop();
        } else {
            newCurrent = win->GetTab(0);
        }
    }
    int newIdx = win->GetTabIdx(newCurrent);
    win->tabsCtrl->SetSelected(newIdx);
    tab = win->CurrentTab();
    LoadModelIntoTab(tab);
#else
    // select tab to the right or to the left if nothing to the right
    int newIdx = idx;
    int lastIdx = nTabs - 1;
    if (newIdx > lastIdx) {
        newIdx = lastIdx;
    }
    win->tabsCtrl->SetSelected(newIdx);
    tab = win->CurrentTab();
    // seen in crash report that tab was WindowTab::Type::None
    // TODO: don't know how it could have happened
    if (tab && tab->type != WindowTab::Type::None) {
        LoadModelIntoTab(tab);
    }
#endif
}

static void CloseWindowIfNoDocuments(MainWindow* win) {
    for (auto& tab : win->Tabs()) {
        if (!tab->IsAboutTab()) {
            return;
        }
    }
    // no tabs or only about tab
    CloseWindow(win, true, false);
}

static void MaybeMigrateTab(WindowTab* tab, MainWindow* newWin, Point releasePt) {
    MainWindow* oldWin = tab->win;
    VisualTabGroup* oldGroup = oldWin->visualTabGroups.FindGroup(tab->visualTabGroupId);

    // don't migrate if it's only one document tab and not
    // dragging over a window
    int nDocTabs = 0;
    for (auto& t : oldWin->Tabs()) {
        if (t->IsAboutTab()) continue;
        nDocTabs++;
    }
    if (nDocTabs == 1 && !newWin) return;

    auto engine = tab->GetEngine();
    if (EngineHasUnsavedAnnotations(engine)) {
        return;
    }

    RemoveTab(tab);

    if (!newWin) {
        if (IsZoomed(oldWin->hwndFrame)) {
            // dragging a tab out of a maximized window: like Chrome, create a
            // normal (non-maximized) window with the size the source window
            // would have when restored, positioned at the cursor so it lands
            // on the new window's tab strip
            WINDOWPLACEMENT wp{};
            wp.length = sizeof(wp);
            GetWindowPlacement(oldWin->hwndFrame, &wp);
            int dx = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
            int dy = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
            int x = releasePt.x - DpiScale(oldWin->hwndFrame, 100);
            int y = releasePt.y - GetTabbarHeight(oldWin->hwndFrame) / 2;
            Rect rect = ShiftRectToWorkArea(Rect(x, y, dx, dy), oldWin->hwndFrame, true);
            newWin = CreateAndShowMainWindow(nullptr, false);
            if (!newWin) {
                return;
            }
            MoveWindow(newWin->hwndFrame, rect);
            ShowMainWindow(newWin, WIN_STATE_NORMAL);
        } else {
            newWin = CreateAndShowMainWindow(nullptr);
        }
        if (!newWin) {
            return;
        }
    }

    // TODO: we should be able to just slide the existing tab
    // into the new window, preserving its controller and
    // the entire state, but this crashes/renders badly etc.
    // More work needed. The code that should work but doesn't:
    // tab->win = newWin;
    // newWin->currentTabTemp = AddTabToWindow(newWin, tab);
    // newWin->ctrl = tab->ctrl;
    // UpdateUiForCurrentTab(newWin);
    // newWin->showSelection = tab->selectionOnPage != nullptr;
    // HwndSetFocus(newWin->hwndFrame);
    // newWin->RedrawAll(true);
    // TabsOnChangedDoc(newWin);
    WindowTab* newTab = new WindowTab(newWin);
    newTab->SetFilePath(tab->filePath);
    newTab->SetDisplayName(tab->displayName);
    SetVisualTabGroup(newTab, CloneVisualTabGroupToWindow(newWin, oldGroup));
    newWin->currentTabTemp = AddTabToWindow(newWin, newTab);
    newWin->ctrl = nullptr;
    LoadArgs args(tab->filePath, newWin);
    args.SetDisplayName(tab->displayName);
    args.forceReuse = true;
    args.noSavePrefs = true;
    LoadDocument(&args);
    delete tab;

    CloseWindowIfNoDocuments(oldWin);
}

// Selects the given tab (0-based index)
// tabIndex can come from settings file so must be sanitized
void TabsSelect(MainWindow* win, int tabIndex) {
    auto tabs = win->Tabs();
    int nTabs = tabs.Size();
    logf("TabsSelect: tabIndex: %d, nTabs: %d\n", tabIndex, nTabs);
    if (nTabs == 0) {
        logf("TabsSelect: skipping because nTabs = %d\n", nTabs);
        return;
    }
    if (tabIndex < 0 || tabIndex >= nTabs) {
        tabIndex = 0;
        logf("TabsSelect: fixing tabIndex to 0\n");
    }
    TabsCtrl* tabsCtrl = win->tabsCtrl;
    int currIdx = tabsCtrl->GetSelected();
    if (tabIndex == currIdx) {
        return;
    }

    bool isShowingPageInfo = (GetNotificationForGroup(win->hwndCanvas, kNotifPageInfo) != nullptr);

    // same work as in onSelectionChanging and onSelectionChanged
    SaveCurrentWindowTab(win);
    int prevIdx = tabsCtrl->SetSelected(tabIndex);
    if (prevIdx < 0) {
        return;
    }
    WindowTab* tab = tabs[tabIndex];
    LoadModelIntoTab(tab);
    if (isShowingPageInfo) {
        PostMessageW(win->hwndFrame, WM_COMMAND, CmdTogglePageInfo, 0);
    }
}

// clang-format off
extern bool SaveAnnotationsToExistingFile(WindowTab*);
extern bool SaveAnnotationsToMaybeNewPdfFile(WindowTab*);

static MenuDef menuDefContextTab[] = {
    // these top items are removed unless the document has unsaved changes;
    // text matches the "Unsaved changes" close dialog
    {
        _TRN("&Save to existing PDF"),
        CmdSaveAnnotations,
    },
    {
        _TRN("Save to &new PDF"),
        CmdSaveAnnotationsNewFile,
    },
    {
        _TRN("&Discard changes"),
        CmdDiscardAnnotations,
    },
    {
        kMenuSeparator,
        0,
    },
    {
        _TRN("Properties..."),
        CmdProperties,
    },
    {
        _TRN("Show in folder"),
        CmdShowInFolder,
    },
    {
        _TRN("Copy File Path"),
        CmdCopyFilePath,
    },
    {
        _TRN("Open In New Window"),
        CmdDuplicateInNewWindow,
    },
    {
        _TRN("Set Tab Color"),
        CmdSetTabColor,
    },
    {
        kMenuSeparator,
        0,
    },
    {
        _TRN("Close"),
        CmdClose,
    },
    {
        _TRN("Close Other Tabs"),
        CmdCloseOtherTabs,
    },
    {
        _TRN("Close Tabs To The Right"),
        CmdCloseTabsToTheRight,
    },
    {
        _TRN("Close Tabs To The Left"),
        CmdCloseTabsToTheLeft,
    },
    {
        _TRN("Close All Tabs"),
        CmdCloseAllTabs,
    },
    {
        kMenuSeparator,
        0,
    },
    {
        _TRN("Save Tab Group"),
        CmdTabGroupSave,
    },
    {
        _TRN("Restore Tab Group"),
        CmdTabGroupRestore,
    },
    {
        nullptr,
        0,
    },
};
// clang-format on

void CollectTabsToClose(MainWindow* win, WindowTab* currTab, Vec<WindowTab*>& toCloseOther,
                        Vec<WindowTab*>& toCloseRight, Vec<WindowTab*>& toCloseLeft) {
    int nTabs = win->TabCount();
    bool seenCurrent = false;
    for (int i = 0; i < nTabs; i++) {
        WindowTab* tab = win->Tabs()[i];
        if (tab->IsAboutTab()) {
            continue;
        }
        if (currTab == tab) {
            seenCurrent = true;
            continue;
        }
        toCloseOther.Append(tab);
        if (seenCurrent) {
            toCloseRight.Append(tab);
        } else {
            toCloseLeft.Append(tab);
        }
    }
}

void CloseAllTabs(MainWindow* win) {
    // can't close while iterating over the tabs so collect them first
    Vec<WindowTab*> toClose;
    int nTabs = win->TabCount();
    for (int i = 0; i < nTabs; i++) {
        WindowTab* t = win->GetTab(i);
        if (t->IsAboutTab()) {
            continue;
        }
        toClose.Append(t);
    }
    for (WindowTab* t : toClose) {
        CloseTab(t, false);
    }
}

// TODO: add "Move to another window" sub-menu
// prompts for a new name and applies it to the group (empty clears the name)
static void RenameVisualTabGroupInteractive(MainWindow* win, VisualTabGroup* group) {
    if (!win || !group) {
        return;
    }
    char* newName = Dialog_RenameTabGroup(win->hwndFrame, group->name);
    if (!newName) {
        return; // cancelled
    }
    str::ReplaceWithCopy(&group->name, newName);
    str::Free(newName);
    // the chip width depends on the label, so re-layout before repainting
    win->tabsCtrl->LayoutTabs();
    win->tabsCtrl->ScheduleRepaint();
    SaveSettings();
}

// prompts for a new color and applies it to the group
static void RecolorVisualTabGroupInteractive(MainWindow* win, VisualTabGroup* group) {
    if (!win || !group) {
        return;
    }
    COLORREF newColor;
    if (!Dialog_SetGroupColor(win->hwndFrame, group->color, newColor)) {
        return; // cancelled
    }
    group->color = newColor;
    win->tabsCtrl->LayoutTabs();
    win->tabsCtrl->ScheduleRepaint();
    SaveSettings();
}

// disbands the group: every member leaves the group (tabs stay open)
static void DisbandVisualTabGroup(MainWindow* win, int groupId) {
    if (!win || groupId < 0) {
        return;
    }
    Vec<WindowTab*> members;
    for (WindowTab* tab : win->Tabs()) {
        if (tab->visualTabGroupId == groupId) {
            members.Append(tab);
        }
    }
    // RemoveTabFromVisualGroup re-layouts and deletes the group once the last member leaves
    for (WindowTab* tab : members) {
        RemoveTabFromVisualGroup(win, tab);
    }
    SaveSettings();
}

// closes every tab in the group
static void CloseVisualTabGroup(MainWindow* win, int groupId) {
    if (!win || groupId < 0) {
        return;
    }
    Vec<WindowTab*> members;
    for (WindowTab* tab : win->Tabs()) {
        if (tab->visualTabGroupId == groupId) {
            members.Append(tab);
        }
    }
    for (WindowTab* tab : members) {
        CloseTab(tab, false);
    }
}

// ===== Chrome-style visual tab group editor popup =====
// A frameless popup shown when a group's header band is right-clicked: a name edit box, a
// row of color swatches, and the group actions (collapse/expand, ungroup, close). Mirrors
// Chrome's tab-group editor. Built with plain GDI; dismisses on focus loss / Esc / Enter.

static const WCHAR* kGroupEditorClass = L"SumatraVisualTabGroupEditor";

enum {
    kGroupActCollapse = 0,
    kGroupActUngroup,
    kGroupActClose,
    kGroupActCount,
};

struct VisualTabGroupEditor {
    MainWindow* win = nullptr;
    int groupId = -1;
    HWND hwnd = nullptr;
    HWND hwndEdit = nullptr;
    WNDPROC editOrigProc = nullptr;
    HFONT font = nullptr;
    HBRUSH editBgBrush = nullptr;
    COLORREF bgCol = 0;
    COLORREF fgCol = 0;
    COLORREF editBgCol = 0;
    COLORREF hoverCol = 0;
    int hotSwatch = -1;
    int hotAction = -1;
    bool committed = false;
    bool destroying = false;
    // layout (client coords), filled by LayoutGroupEditor
    RECT rEdit{};
    RECT rSwatch[16]{};
    int nSwatch = 0;
    RECT rAction[kGroupActCount]{};
    int sepY = 0;
    int pad = 0;
    int swatchDiam = 0;
};

static COLORREF BlendCol(COLORREF a, COLORREF b, int pct) {
    int r = (GetRValue(a) * (100 - pct) + GetRValue(b) * pct) / 100;
    int g = (GetGValue(a) * (100 - pct) + GetGValue(b) * pct) / 100;
    int bl = (GetBValue(a) * (100 - pct) + GetBValue(b) * pct) / 100;
    return RGB(r, g, bl);
}

static COLORREF ContrastTextCol(COLORREF bg) {
    int lum = (GetRValue(bg) * 299 + GetGValue(bg) * 587 + GetBValue(bg) * 114) / 1000;
    return lum < 128 ? RGB(0xf0, 0xf0, 0xf0) : RGB(0x20, 0x20, 0x20);
}

static const char* GroupEditorActionLabel(VisualTabGroup* group, int act) {
    switch (act) {
        case kGroupActCollapse:
            return group && group->collapsed ? _TRA("Expand group") : _TRA("Collapse group");
        case kGroupActUngroup:
            return _TRA("Ungroup");
        case kGroupActClose:
            return _TRA("Close group");
    }
    return "";
}

static void LayoutGroupEditor(VisualTabGroupEditor* ed) {
    HWND hwnd = ed->hwnd;
    int pad = DpiScale(hwnd, 12);
    int editH = DpiScale(hwnd, 26);
    int sd = DpiScale(hwnd, 20);
    int sgap = DpiScale(hwnd, 9);
    int rowH = DpiScale(hwnd, 32);
    ed->pad = pad;
    ed->swatchDiam = sd;

    RECT rc;
    GetClientRect(hwnd, &rc);
    int w = rc.right - rc.left;

    int y = pad;
    ed->rEdit = RECT{pad, y, w - pad, y + editH};
    y += editH + pad;

    int sx = pad;
    for (int i = 0; i < ed->nSwatch; i++) {
        ed->rSwatch[i] = RECT{sx, y, sx + sd, y + sd};
        sx += sd + sgap;
    }
    y += sd + pad;

    ed->sepY = y - pad / 2;
    for (int i = 0; i < kGroupActCount; i++) {
        ed->rAction[i] = RECT{0, y, w, y + rowH};
        y += rowH;
    }
}

static void CommitGroupEditorName(VisualTabGroupEditor* ed) {
    if (ed->committed) {
        return;
    }
    ed->committed = true;
    VisualTabGroup* group = ed->win->visualTabGroups.FindGroup(ed->groupId);
    if (!group) {
        return; // group was disbanded/closed by an action
    }
    TempStr entered = HwndGetTextTemp(ed->hwndEdit);
    if (!str::Eq(entered ? entered : "", group->name ? group->name : "")) {
        str::ReplaceWithCopy(&group->name, entered ? entered : "");
        ed->win->tabsCtrl->LayoutTabs();
        ed->win->tabsCtrl->ScheduleRepaint();
    }
    SaveSettings();
}

static void PaintGroupEditor(VisualTabGroupEditor* ed, HDC hdc) {
    HWND hwnd = ed->hwnd;
    VisualTabGroup* group = ed->win->visualTabGroups.FindGroup(ed->groupId);
    RECT rc;
    GetClientRect(hwnd, &rc);

    // background, separator, hovered-row highlight and action text (plain GDI)
    HBRUSH bgBr = CreateSolidBrush(ed->bgCol);
    FillRect(hdc, &rc, bgBr);
    DeleteObject(bgBr);

    HPEN sepPen = CreatePen(PS_SOLID, 1, BlendCol(ed->bgCol, ed->fgCol, 25));
    HGDIOBJ op = SelectObject(hdc, sepPen);
    MoveToEx(hdc, ed->pad, ed->sepY, nullptr);
    LineTo(hdc, rc.right - ed->pad, ed->sepY);
    SelectObject(hdc, op);
    DeleteObject(sepPen);

    HGDIOBJ of = SelectObject(hdc, ed->font);
    SetBkMode(hdc, TRANSPARENT);
    for (int i = 0; i < kGroupActCount; i++) {
        RECT r = ed->rAction[i];
        if (ed->hotAction == i) {
            HBRUSH hb = CreateSolidBrush(ed->hoverCol);
            FillRect(hdc, &r, hb);
            DeleteObject(hb);
        }
        SetTextColor(hdc, ed->fgCol);
        RECT rt = r;
        rt.left += ed->pad;
        TempWStr ws = ToWStrTemp(GroupEditorActionLabel(group, i));
        DrawTextW(hdc, ws, -1, &rt, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
    }
    SelectObject(hdc, of);

    // color swatches via GDI+ so the circles are smooth (anti-aliased), like Chrome
    int n = 0;
    const COLORREF* palette = VisualTabGroupPalette(&n);
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    for (int i = 0; i < ed->nSwatch && i < n; i++) {
        RECT r = ed->rSwatch[i];
        int d = r.right - r.left;
        bool selected = group && (palette[i] == group->color);
        bool hot = (ed->hotSwatch == i);
        int inset = (selected || hot) ? DpiScale(hwnd, 3) : 0;
        if (selected || hot) {
            // outer ring with a gap, then the color disc inset inside it
            Gdiplus::Pen pen(Gdiplus::Color(GetRValue(ed->fgCol), GetGValue(ed->fgCol), GetBValue(ed->fgCol)),
                             (Gdiplus::REAL)DpiScale(hwnd, 2));
            g.DrawEllipse(&pen, r.left, r.top, d - 1, d - 1);
        }
        Gdiplus::SolidBrush br(Gdiplus::Color(GetRValue(palette[i]), GetGValue(palette[i]), GetBValue(palette[i])));
        g.FillEllipse(&br, r.left + inset, r.top + inset, d - 2 * inset, d - 2 * inset);
    }
}

static int GroupEditorSwatchAt(VisualTabGroupEditor* ed, POINT pt) {
    for (int i = 0; i < ed->nSwatch; i++) {
        if (PtInRect(&ed->rSwatch[i], pt)) {
            return i;
        }
    }
    return -1;
}

static int GroupEditorActionAt(VisualTabGroupEditor* ed, POINT pt) {
    for (int i = 0; i < kGroupActCount; i++) {
        if (PtInRect(&ed->rAction[i], pt)) {
            return i;
        }
    }
    return -1;
}

static void GroupEditorApplyColor(VisualTabGroupEditor* ed, int swatchIdx) {
    int n = 0;
    const COLORREF* palette = VisualTabGroupPalette(&n);
    if (swatchIdx < 0 || swatchIdx >= n) {
        return;
    }
    VisualTabGroup* group = ed->win->visualTabGroups.FindGroup(ed->groupId);
    if (!group) {
        return;
    }
    group->color = palette[swatchIdx];
    // live preview: the chip + underline pick up the new color immediately
    ed->win->tabsCtrl->LayoutTabs();
    ed->win->tabsCtrl->ScheduleRepaint();
    InvalidateRect(ed->hwnd, nullptr, FALSE);
}

static void GroupEditorRunAction(VisualTabGroupEditor* ed, int act) {
    MainWindow* win = ed->win;
    int groupId = ed->groupId;
    CommitGroupEditorName(ed); // keep any name edit before acting
    ed->destroying = true;     // suppress the re-entrant WM_ACTIVATE destroy
    DestroyWindow(ed->hwnd);   // ed is freed in WM_NCDESTROY; don't touch it after this
    VisualTabGroup* group = win->visualTabGroups.FindGroup(groupId);
    switch (act) {
        case kGroupActCollapse:
            if (group) {
                group->collapsed = !group->collapsed;
                ApplyVisualTabGroupCollapse(win);
                SaveSettings();
            }
            return;
        case kGroupActUngroup:
            DisbandVisualTabGroup(win, groupId);
            return;
        case kGroupActClose:
            CloseVisualTabGroup(win, groupId);
            return;
    }
}

static LRESULT CALLBACK GroupEditorEditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* ed = (VisualTabGroupEditor*)GetWindowLongPtr(GetParent(hwnd), GWLP_USERDATA);
    if (ed && msg == WM_KEYDOWN && (wp == VK_RETURN || wp == VK_ESCAPE)) {
        ed->destroying = true;
        DestroyWindow(ed->hwnd);
        return 0;
    }
    if (ed && msg == WM_CHAR && (wp == VK_RETURN || wp == VK_ESCAPE)) {
        return 0; // swallow so the edit doesn't beep
    }
    WNDPROC orig = ed ? ed->editOrigProc : nullptr;
    return orig ? CallWindowProc(orig, hwnd, msg, wp, lp) : DefWindowProc(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK VisualTabGroupEditorProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* ed = (VisualTabGroupEditor*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
    if (!ed && msg != WM_CREATE) {
        return DefWindowProc(hwnd, msg, wp, lp);
    }
    switch (msg) {
        case WM_CREATE: {
            auto* cs = (CREATESTRUCT*)lp;
            ed = (VisualTabGroupEditor*)cs->lpCreateParams;
            ed->hwnd = hwnd;
            SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)ed);

            // rounded popup corners for a softer, Chrome-like panel
            int rad = DpiScale(hwnd, 8);
            HRGN rgn = CreateRoundRectRgn(0, 0, cs->cx + 1, cs->cy + 1, rad, rad);
            SetWindowRgn(hwnd, rgn, TRUE); // window owns the region now

            ed->bgCol = ThemeControlBackgroundColor();
            ed->fgCol = ContrastTextCol(ed->bgCol);
            ed->editBgCol = BlendCol(ed->bgCol, ed->fgCol, 10);
            ed->hoverCol = BlendCol(ed->bgCol, ed->fgCol, 14);
            ed->editBgBrush = CreateSolidBrush(ed->editBgCol);
            int fh = -DpiScale(hwnd, 13);
            ed->font = CreateFontW(fh, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                   CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

            LayoutGroupEditor(ed);
            RECT re = ed->rEdit;
            ed->hwndEdit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, re.left, re.top,
                                           re.right - re.left, re.bottom - re.top, hwnd, (HMENU)1,
                                           (HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE), nullptr);
            SendMessageW(ed->hwndEdit, WM_SETFONT, (WPARAM)ed->font, TRUE);
            VisualTabGroup* group = ed->win->visualTabGroups.FindGroup(ed->groupId);
            if (group && group->name) {
                HwndSetText(ed->hwndEdit, group->name);
            }
            ed->editOrigProc = (WNDPROC)SetWindowLongPtr(ed->hwndEdit, GWLP_WNDPROC, (LONG_PTR)GroupEditorEditProc);
            return 0;
        }
        case WM_CTLCOLOREDIT: {
            HDC dc = (HDC)wp;
            SetTextColor(dc, ed->fgCol);
            SetBkColor(dc, ed->editBgCol);
            return (LRESULT)ed->editBgBrush;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            PaintGroupEditor(ed, hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_MOUSEMOVE: {
            POINT pt{(short)LOWORD(lp), (short)HIWORD(lp)};
            int hs = GroupEditorSwatchAt(ed, pt);
            int ha = (hs < 0) ? GroupEditorActionAt(ed, pt) : -1;
            if (hs != ed->hotSwatch || ha != ed->hotAction) {
                ed->hotSwatch = hs;
                ed->hotAction = ha;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                if (GroupEditorSwatchAt(ed, pt) >= 0 || GroupEditorActionAt(ed, pt) >= 0) {
                    SetCursor(LoadCursor(nullptr, IDC_HAND));
                    return TRUE;
                }
            }
            break;
        case WM_LBUTTONUP: {
            POINT pt{(short)LOWORD(lp), (short)HIWORD(lp)};
            int hs = GroupEditorSwatchAt(ed, pt);
            if (hs >= 0) {
                GroupEditorApplyColor(ed, hs);
                return 0;
            }
            int ha = GroupEditorActionAt(ed, pt);
            if (ha >= 0) {
                GroupEditorRunAction(ed, ha); // destroys the window
                return 0;
            }
            return 0;
        }
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE && !ed->destroying) {
                ed->destroying = true;
                DestroyWindow(hwnd);
            }
            return 0;
        case WM_DESTROY:
            CommitGroupEditorName(ed);
            if (ed->editOrigProc && ed->hwndEdit) {
                SetWindowLongPtr(ed->hwndEdit, GWLP_WNDPROC, (LONG_PTR)ed->editOrigProc);
            }
            return 0;
        case WM_NCDESTROY:
            if (ed) {
                if (ed->font) {
                    DeleteObject(ed->font);
                }
                if (ed->editBgBrush) {
                    DeleteObject(ed->editBgBrush);
                }
                delete ed;
                SetWindowLongPtr(hwnd, GWLP_USERDATA, 0);
            }
            return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

// right-clicking a group header band opens a Chrome-style editor for the whole group
static void ShowVisualTabGroupEditor(MainWindow* win, int groupId, POINT pt) {
    VisualTabGroup* group = win->visualTabGroups.FindGroup(groupId);
    if (!group) {
        return;
    }
    HINSTANCE hinst = (HINSTANCE)GetModuleHandleW(nullptr);
    static bool classRegistered = false;
    if (!classRegistered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_DROPSHADOW;
        wc.lpfnWndProc = VisualTabGroupEditorProc;
        wc.hInstance = hinst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = kGroupEditorClass;
        RegisterClassExW(&wc);
        classRegistered = true;
    }

    auto* ed = new VisualTabGroupEditor();
    ed->win = win;
    ed->groupId = groupId;
    int nPal = 0;
    VisualTabGroupPalette(&nPal);
    ed->nSwatch = nPal;

    // compute popup size up front (same DPI as the frame window)
    HWND frame = win->hwndFrame;
    int pad = DpiScale(frame, 12);
    int editH = DpiScale(frame, 26);
    int sd = DpiScale(frame, 20);
    int sgap = DpiScale(frame, 9);
    int rowH = DpiScale(frame, 32);
    int swatchRowW = nPal * sd + (nPal - 1) * sgap;
    int minW = DpiScale(frame, 230);
    int w = swatchRowW + 2 * pad;
    if (w < minW) {
        w = minW;
    }
    int h = pad + editH + pad + sd + pad + kGroupActCount * rowH + pad;

    // keep the popup on-screen near the click point
    int x = pt.x;
    int y = pt.y;
    RECT wa{};
    SystemParametersInfo(SPI_GETWORKAREA, 0, &wa, 0);
    if (x + w > wa.right) {
        x = wa.right - w;
    }
    if (y + h > wa.bottom) {
        y = wa.bottom - h;
    }
    if (x < wa.left) {
        x = wa.left;
    }
    if (y < wa.top) {
        y = wa.top;
    }

    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kGroupEditorClass, L"", WS_POPUP, x, y, w, h, frame,
                                nullptr, hinst, ed);
    if (!hwnd) {
        delete ed;
        return;
    }
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    if (ed->hwndEdit) {
        SetFocus(ed->hwndEdit);
        EditSelectAll(ed->hwndEdit);
    }
}

static void TabsContextMenu(ContextMenuEvent* ev) {
    MainWindow* win = FindMainWindowByHwnd(ev->w->hwnd);
    TabsCtrl* tabsCtrl = (TabsCtrl*)ev->w;
    TabsCtrl::MouseState tabState = tabsCtrl->TabStateFromMousePosition(ev->mouseWindow);
    // right-click on a group header band → a Chrome-style editor for the whole group
    if (tabState.overGroupHeader && tabState.groupId >= 0) {
        ShowVisualTabGroupEditor(win, tabState.groupId, ToPOINT(ev->mouseScreen));
        return;
    }
    int tabIdx = tabState.tabIdx;
    if (tabIdx < 0) {
        return;
    }

    WindowTab* tabUnderMouse = win->Tabs()[tabIdx];
    if (tabUnderMouse->IsAboutTab()) {
        return;
    }
    POINT pt = ToPOINT(ev->mouseScreen);

    Vec<WindowTab*> toCloseOther;
    Vec<WindowTab*> toCloseRight;
    Vec<WindowTab*> toCloseLeft;
    CollectTabsToClose(win, tabUnderMouse, toCloseOther, toCloseRight, toCloseLeft);

    DisplayModel* dmTab = tabUnderMouse->AsFixed();
    EngineBase* tabEngine = dmTab ? dmTab->GetEngine() : nullptr;

    // Build the command context for the tab under the mouse, which may differ
    // from the current tab that NewAppCommandCtx() keys off. Without a context
    // command availability is evaluated against an empty (no-document) state,
    // which removes almost every item (leaving only "Restore Tab Group").
    BuildMenuCtx* ctx = NewBuildMenuCtx(tabUnderMouse, Point{0, 0});
    ctx->tab = tabUnderMouse;
    ctx->isDocLoaded = true; // tabUnderMouse is a real (non-about) document tab
    ctx->filePath = tabUnderMouse->filePath;
    ctx->supportsAnnots = EngineSupportsAnnotations(tabEngine) && !win->isFullScreen;
    ctx->hasUnsavedAnnotations = EngineHasUnsavedAnnotations(tabEngine);
    ctx->canCloseOtherTabs = !toCloseOther.IsEmpty();
    ctx->canCloseTabsToRight = !toCloseRight.IsEmpty();
    ctx->canCloseTabsToLeft = !toCloseLeft.IsEmpty();

    HMENU popup = BuildMenuFromDef(menuDefContextTab, CreatePopupMenu(), ctx);
    DeleteBuildMenuCtx(ctx);
    AddVisualTabGroupMenuItems(popup, win, tabUnderMouse);

    if (!tabUnderMouse->ctrl || tabUnderMouse->visualTabGroupId != -1) {
        MenuSetEnabled(popup, CmdSetTabColor, false);
    }
    // the save/discard items only make sense when the document has unsaved
    // changes (e.g. filled form fields, added annotations); otherwise remove
    // them, then clean up the separator they leave behind
    if (!EngineHasUnsavedAnnotations(tabEngine)) {
        DeleteMenu(popup, CmdSaveAnnotations, MF_BYCOMMAND);
        DeleteMenu(popup, CmdSaveAnnotationsNewFile, MF_BYCOMMAND);
        DeleteMenu(popup, CmdDiscardAnnotations, MF_BYCOMMAND);
        RemoveBadMenuSeparators(popup);
    }
    MarkMenuOwnerDraw(popup);
    uint flags = TPM_RETURNCMD | TPM_RIGHTBUTTON;
    int cmdId = TrackPopupMenu(popup, flags, pt.x, pt.y, 0, win->hwndFrame, nullptr);
    FreeMenuOwnerDrawInfoData(popup);
    DestroyMenu(popup);
    switch (cmdId) {
        case CmdCreateVisualTabGroup: {
            VisualTabGroup* group = win->visualTabGroups.CreateDefaultGroup();
            AssignTabToVisualGroup(win, tabUnderMouse, group);
            return;
        }
        case CmdRemoveVisualTabGroup: {
            RemoveTabFromVisualGroup(win, tabUnderMouse);
            return;
        }
        case CmdRenameVisualTabGroup: {
            RenameVisualTabGroupInteractive(win, win->visualTabGroups.FindGroup(tabUnderMouse->visualTabGroupId));
            return;
        }
        case CmdRecolorVisualTabGroup: {
            RecolorVisualTabGroupInteractive(win, win->visualTabGroups.FindGroup(tabUnderMouse->visualTabGroupId));
            return;
        }
        case CmdClose: {
            CloseTab(tabUnderMouse, false);
            return;
        }

        case CmdCloseAllTabs: {
            CloseAllTabs(win);
            return;
        }
        case CmdCloseOtherTabs: {
            for (WindowTab* t : toCloseOther) {
                CloseTab(t, false);
            }
            return;
        }
        case CmdCloseTabsToTheRight: {
            for (WindowTab* t : toCloseRight) {
                CloseTab(t, false);
            }
            return;
        }
        case CmdCloseTabsToTheLeft: {
            for (WindowTab* t : toCloseLeft) {
                CloseTab(t, false);
            }
            return;
        }
        case CmdShowInFolder: {
            SumatraOpenPathInDefaultFileManager(tabUnderMouse->filePath);
            return;
        }
        case CmdCopyFilePath: {
            CopyFilePath(tabUnderMouse);
            return;
        }
        case CmdDuplicateInNewWindow: {
            DuplicateTabInNewWindow(tabUnderMouse);
            return;
        }
        case CmdProperties: {
            ShowProperties(win->hwndFrame, tabUnderMouse->ctrl);
            return;
        }
        case CmdSetTabColor: {
            COLORREF curColor = tabUnderMouse->tabColor;
            bool isUnset = (curColor == kColorUnset);
            if (isUnset) {
                curColor = ThemeControlBackgroundColor();
            }
            COLORREF newColor;
            bool newIsUnset;
            if (!Dialog_SetTabColor(win->hwndFrame, curColor, isUnset, newColor, newIsUnset)) {
                return;
            }
            tabUnderMouse->tabColor = newIsUnset ? kColorUnset : newColor;
            // update TabInfo
            TabInfo* ti = tabsCtrl->GetTab(tabIdx);
            if (ti) {
                ti->tabColor = tabUnderMouse->tabColor;
            }
            // persist to FileState
            FileState* fs = gFileHistory.FindByPath(tabUnderMouse->filePath);
            if (fs) {
                if (newIsUnset) {
                    str::ReplaceWithCopy(&fs->tabCol, "");
                } else {
                    TempStr colorStr = SerializeColorTemp(newColor);
                    str::ReplaceWithCopy(&fs->tabCol, colorStr);
                }
                fs->tabColParsed.wasParsed = false;
            }
            SaveSettings();
            tabsCtrl->ScheduleRepaint();
            return;
        }
        case CmdSaveAnnotations: {
            SaveAnnotationsToExistingFile(tabUnderMouse);
            return;
        }
        case CmdSaveAnnotationsNewFile: {
            SaveAnnotationsToMaybeNewPdfFile(tabUnderMouse);
            return;
        }
        case CmdDiscardAnnotations: {
            // revert to the on-disk version, discarding unsaved changes
            TabsSelect(win, tabIdx);
            ReloadDocument(win, false);
            return;
        }
    }
    if (cmdId >= CmdAddToVisualTabGroupBase) {
        VisualTabGroup* group = GetVisualTabGroupForMenuCmd(win, tabUnderMouse, cmdId);
        if (group) {
            AssignTabToVisualGroup(win, tabUnderMouse, group);
            return;
        }
    }
    // everything we forward to main window
    HwndSendCommand(win->hwndFrame, cmdId);
}

static void MainWindowTabClosed(MainWindow* win, TabsCtrl::ClosedEvent* ev) {
    int closedTabIdx = ev->tabIdx;
    WindowTab* tab = win->GetTab(closedTabIdx);
    CloseTab(tab, false);
}

static void MainWindowTabSelectionChanging(MainWindow* win, TabsCtrl::SelectionChangingEvent* ev) {
    // TODO: Should we allow the switch of the tab if we are in process of printing?
    SaveCurrentWindowTab(win);
    ev->preventChanging = false;
}

static void MainWindowTabSelectionChanged(MainWindow* win, TabsCtrl::SelectionChangedEvent* ev) {
    bool isShowingPageInfo = (GetNotificationForGroup(win->hwndCanvas, kNotifPageInfo) != nullptr);
    int currentIdx = win->tabsCtrl->GetSelected();
    WindowTab* tab = win->Tabs()[currentIdx];
    LoadModelIntoTab(tab);
    if (isShowingPageInfo) {
        PostMessageW(win->hwndFrame, WM_COMMAND, CmdTogglePageInfo, 0);
    }
}

static void MainWindowTabMigration(MainWindow* win, TabsCtrl::MigrationEvent* ev) {
    WindowTab* tab = win->GetTab(ev->tabIdx);
    MainWindow* releaseWnd = nullptr;
    POINT p;
    p.x = ev->releasePoint.x;
    p.y = ev->releasePoint.y;
    HWND hwnd = WindowFromPoint(p);
    if (hwnd != nullptr) {
        releaseWnd = FindMainWindowByHwnd(hwnd);
    }
    if (releaseWnd == win) {
        // don't re-add to the same window
        releaseWnd = nullptr;
    }
    MaybeMigrateTab(tab, releaseWnd, ev->releasePoint);
}

// when collapsing a group that holds the active tab, pick a tab to switch to:
// nearest ungrouped tab to the right, then to the left, else any tab outside the group.
static int FindTabToSelectOnCollapse(MainWindow* win, int groupId) {
    int n = win->TabCount();
    int sel = win->tabsCtrl->GetSelected();
    for (int i = sel + 1; i < n; i++) {
        if (win->GetTab(i)->visualTabGroupId == -1) {
            return i;
        }
    }
    for (int i = sel - 1; i >= 0; i--) {
        if (win->GetTab(i)->visualTabGroupId == -1) {
            return i;
        }
    }
    for (int i = 0; i < n; i++) {
        if (win->GetTab(i)->visualTabGroupId != groupId) {
            return i;
        }
    }
    return -1;
}

// clicking a group header (or its caret) toggles the group's collapsed state
static void MainWindowGroupHeaderClick(MainWindow* win, TabsCtrl::GroupHeaderClickEvent* ev) {
    if (!win || !ev) {
        return;
    }
    VisualTabGroup* group = win->visualTabGroups.FindGroup(ev->groupId);
    if (!group) {
        return;
    }
    bool willCollapse = !group->collapsed;
    if (willCollapse) {
        // Chrome-style: a collapsed group hides ALL its tabs, so if the active tab is
        // inside it, switch to another tab first (otherwise the selection would be hidden).
        WindowTab* cur = win->CurrentTab();
        if (cur && cur->visualTabGroupId == group->id) {
            int idx = FindTabToSelectOnCollapse(win, group->id);
            if (idx < 0) {
                // nowhere else to go (every tab is in this group): leave it expanded
                return;
            }
            TabsSelect(win, idx);
        }
    }
    group->collapsed = willCollapse;
    // freeze tab widths so the clicked header chip stays put under the cursor (like Chrome);
    // the freeze auto-clears when the mouse leaves the tab bar (see TabsCtrl WM_MOUSELEAVE)
    TabsCtrl* tc = win->tabsCtrl;
    if (tc && tc->tabSize.dx > 0) {
        tc->frozenTabDx = tc->tabSize.dx;
        tc->tabWidthFrozen = true;
    }
    ApplyVisualTabGroupCollapse(win);
}

// a tab was dropped onto a group (groupId >= 0) or out of any group (groupId == -1)
static void MainWindowTabGroupDrop(MainWindow* win, TabsCtrl::GroupDropEvent* ev) {
    if (!win || !ev) {
        return;
    }
    WindowTab* tab = win->GetTab(ev->tabIdx);
    if (!tab) {
        return;
    }
    int curGroup = tab->visualTabGroupId;
    if (ev->groupId < 0) {
        if (curGroup != -1) {
            RemoveTabFromVisualGroup(win, tab); // re-syncs the bar item + re-lays out
            return;
        }
    } else if (curGroup != ev->groupId) {
        VisualTabGroup* group = win->visualTabGroups.FindGroup(ev->groupId);
        if (group) {
            AssignTabToVisualGroup(win, tab, group); // re-syncs + re-lays out
            if (group->collapsed) {
                ApplyVisualTabGroupCollapse(win);
            }
            return;
        }
    }
    // membership didn't change: a live in-strip drag may have left a stale visual group on
    // the tab-bar item, so re-sync it from the model and re-lay out
    UpdateTabVisualGroupState(win, tab);
    win->tabsCtrl->LayoutTabs();
    win->tabsCtrl->ScheduleRepaint();
}

void CreateTabbar(MainWindow* win) {
    TabsCtrl::CreateArgs args;
    args.parent = win->hwndFrame;
    args.withToolTips = true;
    args.font = GetAppFont();
    int tabWidth = gGlobalPrefs->tabWidth;
    args.tabDefaultDx = tabWidth;
    args.isRtl = false; // LTR hwnd; RTL tab order follows parent frame (see UpdateWindowRtlLayout)

    TabsCtrl* tabsCtrl = new TabsCtrl();
    tabsCtrl->onTabClosed = MkFunc1(MainWindowTabClosed, win);
    tabsCtrl->onSelectionChanging = MkFunc1(MainWindowTabSelectionChanging, win);
    tabsCtrl->onSelectionChanged = MkFunc1(MainWindowTabSelectionChanged, win);
    tabsCtrl->onContextMenu = MkFunc1Void(TabsContextMenu);
    tabsCtrl->onTabMigration = MkFunc1(MainWindowTabMigration, win);
    tabsCtrl->onGroupHeaderClick = MkFunc1(MainWindowGroupHeaderClick, win);
    tabsCtrl->onTabGroupDrop = MkFunc1(MainWindowTabGroupDrop, win);
    tabsCtrl->Create(args);
    win->tabsCtrl = tabsCtrl;
    win->tabSelectionHistory = new Vec<WindowTab*>();
}

// verifies that WindowTab state is consistent with MainWindow state
static NO_INLINE void VerifyWindowTab(MainWindow* win, WindowTab* tdata) {
    ReportIf(tdata->ctrl != win->ctrl);
#if 0
    // disabling this check. best I can tell, external apps can change window
    // title and trigger this
    auto winTitle = win::GetTextTemp(win->hwndFrame);
    if (!str::Eq(winTitle.Get(), tdata->frameTitle.Get())) {
        logf(L"VerifyWindowTab: winTitle: '%s', tdata->frameTitle: '%s'\n", winTitle.Get(), tdata->frameTitle.Get());
        ReportIf(!str::Eq(winTitle.Get(), tdata->frameTitle));
    }
#endif
    bool expectedTocVisibility = tdata->showToc; // if not in presentation mode
    if (PM_DISABLED != win->presentation) {
        expectedTocVisibility = false; // PM_BLACK_SCREEN, PM_WHITE_SCREEN
        if (PM_ENABLED == win->presentation) {
            expectedTocVisibility = tdata->showTocPresentation;
        }
    }
    ReportDebugIf(win->tocVisible != expectedTocVisibility);
    ReportIf(tdata->canvasRc != win->canvasRc);
}

// Must be called when the active tab is losing selection.
// This happens when a new document is loaded or when another tab is selected.
void SaveCurrentWindowTab(MainWindow* win) {
    if (!win) {
        return;
    }
    if (!win->tabsCtrl) {
        return;
    }
    // the find UI (compact bar or floating window) belongs to the previous tab's
    // search; close it when leaving the tab (HideFindBar also drops the cached
    // results so the next tab can't show or navigate into the old document's
    // matches)
    HideFindBar(win);

    int current = win->tabsCtrl->GetSelected();
    if (-1 == current) {
        return;
    }
    if (win->CurrentTab() != win->Tabs().at(current)) {
        return; // TODO: restore ReportIf() ?
    }

    WindowTab* tab = win->CurrentTab();
    if (win->tocLoaded && tab->ctrl) {
        TocTree* tocTree = tab->ctrl->GetToc();
        UpdateTocExpansionState(tab->tocState, win->tocTreeView, tocTree);
    }
    VerifyWindowTab(win, tab);

    // update the selection history
    win->tabSelectionHistory->Remove(tab);
    win->tabSelectionHistory->Append(tab);
}

WindowTab* AddTabToWindow(MainWindow* win, WindowTab* tab) {
    ReportIf(!win);
    if (!win) {
        return nullptr;
    }
    if (!win->tabsCtrl) {
        return nullptr;
    }

    auto tabs = win->tabsCtrl;
    int idx = win->TabCount();
    bool useTabs = SettingsUseTabs();
    bool noHomeTab = gGlobalPrefs->noHomeTab;
    bool createHomeTab = useTabs && !noHomeTab && (idx == 0);
    if (createHomeTab) {
        WindowTab* homeTab = new WindowTab(win);
        homeTab->type = WindowTab::Type::About;
        homeTab->canvasRc = win->canvasRc;
        TabInfo* newTab = new TabInfo();
        newTab->text = str::Dup("Home");
        newTab->tooltip = nullptr;
        newTab->isPinned = true;
        newTab->canClose = true;
        newTab->userData = (UINT_PTR)homeTab;
        int insertedIdx = tabs->InsertTab(idx, newTab);
        ReportIf(insertedIdx != 0);
        idx++;
    }

    tab->canvasRc = win->canvasRc;
    TabInfo* newTab = new TabInfo();
    newTab->text = str::Dup(tab->GetTabTitle());
    newTab->tooltip = str::Dup(tab->filePath);
    newTab->userData = (UINT_PTR)tab;
    newTab->tabColor = GetEffectiveTabColor(win->visualTabGroups, tab);
    newTab->visualTabGroupId = tab->visualTabGroupId;

    int insertedIdx = tabs->InsertTab(idx, newTab);
    ReportIf(insertedIdx == -1);
    tabs->SetSelected(insertedIdx);
    UpdateTabWidth(win);
    return tab;
}

// Refresh the tab's title
void TabsOnChangedDoc(MainWindow* win) {
    WindowTab* tab = win->CurrentTab();
    ReportIf(!tab != !win->TabCount());
    if (!tab) {
        return;
    }

    int tabIdx = win->GetTabIdx(tab);
    int selectedIdx = win->tabsCtrl->GetSelected();
    if (tabIdx != selectedIdx) {
        logf("TabsonChangeDoc: tabIdx (%d) != selectedIdx (%d)\n", tabIdx, selectedIdx);
        ReportDebugIf(tabIdx != selectedIdx);
    }
    VerifyWindowTab(win, tab);
    UpdateTabTitle(tab);
}

// Called when we're closing an entire window (quitting)
void TabsOnCloseWindow(MainWindow* win) {
    // TODO: I've seen a crash here where it seems like we've deleted the only main window
    // but somehow we still process Esc and we get here. this might not be enough
    if (!win->tabsCtrl) {
        return;
    }
    auto tabs = win->Tabs();
    DeleteVecMembers(tabs);
    win->tabsCtrl->RemoveAllTabs();
    win->tabSelectionHistory->Reset();
    win->currentTabTemp = nullptr;
    win->ctrl = nullptr;
}

void SetTabsInTitlebar(MainWindow* win, bool inTitleBar) {
    if (inTitleBar == win->tabsInTitlebar) {
        return;
    }
    win->tabsInTitlebar = inTitleBar;
    win->tabsCtrl->inTitleBar = inTitleBar;
    if (inTitleBar) {
        RelayoutCaption(win);
    }
    uint flags = SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOSIZE | SWP_NOMOVE;
    SetWindowPos(win->hwndFrame, nullptr, 0, 0, 0, 0, flags);
}

// Selects the next (or previous) tab.
void TabsOnCtrlTab(MainWindow* win, bool reverse) {
    if (!win) {
        return;
    }
    int count = win->TabCount();
    if (count < 2) {
        return;
    }
    int idx = win->tabsCtrl->GetSelected() + 1;
    if (reverse) {
        idx -= 2;
    }
    idx += count; // ensure > 0
    idx = idx % count;
    TabsSelect(win, idx);
}

void MoveTab(MainWindow* win, int dir) {
    if (!win) {
        return;
    }
    int nTabs = win->TabCount();
    int idx = win->tabsCtrl->GetSelected();
    int newIdx = idx + dir;
    if (newIdx < 0) {
        return;
    }
    if (newIdx >= nTabs) {
        return;
    }
    win->tabsCtrl->SwapTabs(idx, newIdx);
    win->tabsCtrl->SetSelected(newIdx);
    win->tabsCtrl->LayoutTabs();
}
