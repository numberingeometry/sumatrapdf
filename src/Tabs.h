/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

int GetTabbarHeight(HWND, float factor = 1.f);

void SaveCurrentWindowTab(MainWindow*);
void LoadModelIntoTab(WindowTab*);

void CreateTabbar(MainWindow*);
WindowTab* AddTabToWindow(MainWindow* win, WindowTab* tab);
void TabsOnCloseWindow(MainWindow*);
void TabsOnChangedDoc(MainWindow*);
void TabsSelect(MainWindow* win, int tabIndex);
void TabsOnCtrlTab(MainWindow* win, bool reverse);
// also shows/hides the tabbar when necessary
void UpdateTabWidth(MainWindow*);
void SetTabsInTitlebar(MainWindow* win, bool inTitlebar);
void RemoveTab(WindowTab*);
// create a new window if win==nullptr
void CollectTabsToClose(MainWindow* win, WindowTab* currTab, Vec<WindowTab*>& toCloseOther,
                        Vec<WindowTab*>& toCloseRight, Vec<WindowTab*>& toCloseLeft);
void CloseAllTabs(MainWindow*);
void MoveTab(MainWindow* win, int dir);
// syncs each tab's collapse-hidden flag from its visual group's collapsed state, then re-lays out
void ApplyVisualTabGroupCollapse(MainWindow* win);
// if `tab` is in a group, moves it next to that group's other members so the group stays contiguous
void EnsureVisualTabGroupContiguity(MainWindow* win, WindowTab* tab);
