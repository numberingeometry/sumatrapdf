/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "utils/BaseUtil.h"
#include "utils/WinUtil.h"
#include "utils/Dpi.h"

#include "wingui/UIModels.h"

#include "wingui/Layout.h"
#include "wingui/WinGui.h"

struct FileState;
struct TabState;
struct SessionData;

#include "MainWindow.h"
#include "Theme.h"

#include "utils/Log.h"

// Forward declaration - defined in MainWindow.cpp
struct MainWindow;
MainWindow* FindMainWindowByHwnd(HWND hwnd);

//--- Tabs

Kind kindTabs = "tabs";

// non-selected tabs narrower than this hide their close button so that
// clicks drag/select instead of accidentally closing the tab
constexpr int kMinTabWidthForClose = 64;

// timer driving the tab-slide animation (drag swap-slide, collapse/expand)
constexpr UINT_PTR kTabAnimTimerId = 0x7AB;

using Gdiplus::Bitmap;
using Gdiplus::Color;
using Gdiplus::CompositingQualityHighQuality;
using Gdiplus::Font;
using Gdiplus::Graphics;
using Gdiplus::GraphicsPath;
using Gdiplus::Ok;
using Gdiplus::PathData;
using Gdiplus::Pen;
using Gdiplus::Region;
using Gdiplus::SolidBrush;
using Gdiplus::Status;
using Gdiplus::StringAlignmentCenter;
using Gdiplus::StringFormat;
using Gdiplus::TextRenderingHintClearTypeGridFit;
using Gdiplus::UnitPixel;

static void HwndTabsSetItemSize(HWND hwnd, Size sz) {
    TabCtrl_SetItemSize(hwnd, sz.dx, sz.dy);
}

// hwnd is kept LTR (like the canvas); UI direction comes from the parent frame
static bool IsTabsRtl(HWND hwnd) {
    HWND parent = GetParent(hwnd);
    return parent && HwndIsRtl(parent);
}

TabInfo::~TabInfo() {
    str::Free(text);
    str::Free(tooltip);
}

void TabsCtrl::ScheduleRepaint() {
    HwndScheduleRepaint(hwnd);
}

static bool IsVisibleTabForLayout(TabInfo* ti) {
    return ti && !ti->isHiddenByGroupCollapse;
}

static Rect AdvanceLayoutSlot(bool isRtl, int& x, int dx, int dy) {
    if (isRtl) {
        int xStart = x - dx;
        x = xStart;
        return {xStart, 0, dx, dy};
    }
    Rect r = {x, 0, dx, dy};
    x += dx;
    return r;
}

static void ResetTabLayout(TabInfo* ti) {
    ti->r = {};
    ti->rVisible = {};
    ti->rClose = {};
    ti->rCloseHit = {};
    ti->titlePos = {};
}

static const char* GetGroupHeaderLabel(TabsCtrl* tabs, int groupId);

// width of a group's header chip: caret + label + padding, clamped to a sane range.
// The chip is its own layout slot to the left of the group's tabs (Chrome-style), so it
// never overlaps a tab and stays put when the group collapses/expands.
static int GroupChipDx(TabsCtrl* tabs, int groupId) {
    HWND hwnd = tabs->hwnd;
    int pad = DpiScale(hwnd, 6);
    int caret = DpiScale(hwnd, 8);
    const char* label = GetGroupHeaderLabel(tabs, groupId);
    int labelDx = 0;
    if (!str::IsEmpty(label)) {
        labelDx = HwndMeasureText(hwnd, label, tabs->GetFont()).dx;
    }
    int chipDx = pad + caret + pad + labelDx + pad;
    int minDx = DpiScale(hwnd, 34);
    int maxDx = DpiScale(hwnd, 200);
    return std::min(std::max(chipDx, minDx), maxDx);
}

// totals used to distribute remaining width across tabs: sum of group chip widths and
// the number of visible (non-collapsed) tab slots.
static void MeasureTabBarSlots(TabsCtrl* tabs, int* totalChipDxOut, int* visibleTabSlotsOut) {
    int totalChipDx = 0;
    int visibleTabSlots = 0;
    int nTabs = tabs->TabCount();
    for (int i = 0; i < nTabs;) {
        TabInfo* ti = tabs->GetTab(i);
        int groupId = ti->visualTabGroupId;
        if (groupId < 0) {
            if (IsVisibleTabForLayout(ti)) {
                visibleTabSlots++;
            }
            i++;
            continue;
        }
        totalChipDx += GroupChipDx(tabs, groupId);
        int j = i;
        while (j < nTabs && tabs->GetTab(j)->visualTabGroupId == groupId) {
            if (IsVisibleTabForLayout(tabs->GetTab(j))) {
                visibleTabSlots++;
            }
            j++;
        }
        i = j;
    }
    *totalChipDxOut = totalChipDx;
    *visibleTabSlotsOut = visibleTabSlots;
}

static const char* GetGroupHeaderLabel(TabsCtrl* tabs, int groupId) {
    MainWindow* win = FindMainWindowByHwnd(tabs->hwnd);
    if (!win) {
        return nullptr;
    }
    VisualTabGroup* group = win->visualTabGroups.FindGroup(groupId);
    if (!group) {
        return nullptr;
    }
    return group->name;
}

static TabsCtrl::GroupHeaderInfo BuildGroupHeaderInfo(TabsCtrl* tabs, int groupId, const Rect& rChip) {
    TabsCtrl::GroupHeaderInfo header{};
    header.groupId = groupId;
    header.label = GetGroupHeaderLabel(tabs, groupId);
    header.rTabs = rChip;
    header.rHeader = rChip; // the whole chip slot is the clickable / hit-test area

    HWND hwnd = tabs->hwnd;
    int caretSize = DpiScale(hwnd, 8);
    int caretPadX = DpiScale(hwnd, 7);
    int caretY = rChip.y + std::max(0, (rChip.dy - caretSize) / 2);
    if (IsTabsRtl(hwnd)) {
        header.rCaret = {rChip.x + rChip.dx - caretPadX - caretSize, caretY, caretSize, caretSize};
    } else {
        header.rCaret = {rChip.x + caretPadX, caretY, caretSize, caretSize};
    }
    return header;
}

static COLORREF GetGroupHeaderColor(TabsCtrl* tabs, int groupId, COLORREF fallback) {
    MainWindow* win = FindMainWindowByHwnd(tabs->hwnd);
    if (win) {
        VisualTabGroup* group = win->visualTabGroups.FindGroup(groupId);
        if (group && !IsSpecialColor(group->color)) {
            return group->color;
        }
    }
    return AccentColor(fallback, 35);
}

// Calculates tab's elements, based on its width and height.
// Generates a GraphicsPath, which is used for painting the tab, etc.
void TabsCtrl::LayoutTabs() {
    Rect rect = ClientRect(hwnd);
    int dy = rect.dy;
    int nTabs = TabCount();
    groupHeaders.Reset();
    if (nTabs == 0) {
        HwndScheduleRepaint(hwnd);
        return;
    }

    int totalChipDx = 0;
    int visibleTabSlots = 0;
    MeasureTabBarSlots(this, &totalChipDx, &visibleTabSlots);
    int dx = tabDefaultDx;
    if (visibleTabSlots > 0) {
        if (tabWidthFrozen && frozenTabDx > 0) {
            dx = frozenTabDx;
        } else {
            int maxDx = (rect.dx - 5 - totalChipDx) / visibleTabSlots;
            dx = std::min(tabDefaultDx, maxDx);
            int minDx = DpiScale(hwnd, 40);
            if (dx < minDx) {
                dx = minDx;
            }
        }
    }
    tabSize = {dx, dy};
    if (IsRunningOnWine()) {
        logf("TabsCtrl::LayoutTabs: hwnd=%p client=(%d,%d) tabSize=(%d,%d) nTabs=%d visibleTabSlots=%d\n", hwnd, rect.dx,
             rect.dy, tabSize.dx, tabSize.dy, nTabs, visibleTabSlots);
    }

    int closeDy = DpiScale(hwnd, 16);
    int closeDx = closeDy;
    int closeY = (dy - closeDy) / 2;
    bool isRtl = IsTabsRtl(hwnd);
    int closePad = 8;

    HFONT hfont = GetFont();
    TooltipInfo* tools = AllocArrayTemp<TooltipInfo>(nTabs);
    int x = isRtl ? rect.dx : 0;
    for (int i = 0; i < nTabs; i++) {
        TabInfo* ti = GetTab(i);
        ResetTabLayout(ti);
        ti->titleSize = HwndMeasureText(hwnd, ti->text, hfont);
        if (IsRunningOnWine() && i == 0) {
            logf("TabsCtrl::LayoutTabs: titleSize=(%d,%d) fontDyPx=%d\n", ti->titleSize.dx, ti->titleSize.dy,
                 FontDyPx(hwnd, hfont));
        }
        if (withToolTips) {
            tools[i].s = ti->tooltip;
            tools[i].id = i;
            tools[i].r = {};
        }
    }

    for (int i = 0; i < nTabs;) {
        TabInfo* ti = GetTab(i);
        int groupId = ti->visualTabGroupId;
        if (groupId < 0) {
            if (IsVisibleTabForLayout(ti)) {
                Rect rTab = AdvanceLayoutSlot(isRtl, x, dx, dy);
                ti->r = rTab;
                ti->rVisible = rTab;
                int titleY = std::max(0, (dy - ti->titleSize.dy) / 2);
                if (isRtl) {
                    ti->rClose = {rTab.x + closePad, closeY, closeDx, closeDy};
                    ti->rCloseHit = {rTab.x, 0, closeDx + 2 * closePad, dy};
                    ti->titlePos = {rTab.x + rTab.dx - 2 - ti->titleSize.dx, titleY};
                } else {
                    int xEnd = rTab.x + rTab.dx;
                    ti->rClose = {xEnd - closeDx - closePad, closeY, closeDx, closeDy};
                    ti->rCloseHit = {xEnd - closeDx - 2 * closePad, 0, closeDx + 2 * closePad, dy};
                    ti->titlePos = {rTab.x + 2, titleY};
                }
                if (withToolTips) {
                    tools[i].r = rTab;
                }
            }
            i++;
            continue;
        }

        // a header chip slot first, then the group's visible member tabs to its right
        int chipDx = GroupChipDx(this, groupId);
        Rect rChip = AdvanceLayoutSlot(isRtl, x, chipDx, dy);
        int hdrIdx = groupHeaders.Size();
        groupHeaders.Append(BuildGroupHeaderInfo(this, groupId, rChip));
        int spanLeft = rChip.x;
        int spanRight = rChip.x + rChip.dx;

        int j = i;
        while (j < nTabs && GetTab(j)->visualTabGroupId == groupId) {
            TabInfo* tRun = GetTab(j);
            if (IsVisibleTabForLayout(tRun)) {
                Rect rTab = AdvanceLayoutSlot(isRtl, x, dx, dy);
                tRun->r = rTab;
                tRun->rVisible = rTab;
                spanLeft = std::min(spanLeft, rTab.x);
                spanRight = std::max(spanRight, rTab.x + rTab.dx);
                int titleY = std::max(0, (dy - tRun->titleSize.dy) / 2);
                if (isRtl) {
                    tRun->rClose = {rTab.x + closePad, closeY, closeDx, closeDy};
                    tRun->rCloseHit = {rTab.x, 0, closeDx + 2 * closePad, dy};
                    tRun->titlePos = {rTab.x + rTab.dx - 2 - tRun->titleSize.dx, titleY};
                } else {
                    int xEnd = rTab.x + rTab.dx;
                    tRun->rClose = {xEnd - closeDx - closePad, closeY, closeDx, closeDy};
                    tRun->rCloseHit = {xEnd - closeDx - 2 * closePad, 0, closeDx + 2 * closePad, dy};
                    tRun->titlePos = {rTab.x + 2, titleY};
                }
                if (withToolTips) {
                    tools[j].r = rTab;
                }
            }
            j++;
        }
        // the group underline spans the chip + all visible member tabs
        groupHeaders[hdrIdx].rTabs = {spanLeft, 0, spanRight - spanLeft, dy};
        i = j;
    }

    if (withToolTips) {
        HWND ttHwnd = GetToolTipsHwnd();
        TooltipRemoveAll(ttHwnd);
        TooltipAddTools(ttHwnd, hwnd, tools, nTabs);
    }

    // while dragging in-strip, the dragged tab's slot follows the cursor (others keep theirs,
    // so a gap opens at the insertion point; the dragged tab is drawn on top in Paint)
    int draggedIdx = (draggingTab && !dragDetached) ? GetSelected() : -1;
    if (draggedIdx >= 0 && draggedIdx < nTabs) {
        TabInfo* dt = GetTab(draggedIdx);
        if (dt && !dt->rVisible.IsEmpty()) {
            int floatX = dragMouseX - grabLocation.x;
            int maxX = rect.dx - dt->rVisible.dx;
            if (floatX < 0) {
                floatX = 0;
            }
            if (floatX > maxX) {
                floatX = maxX;
            }
            int shift = floatX - dt->rVisible.x;
            dt->rVisible.x += shift;
            dt->r.x += shift;
            dt->rClose.x += shift;
            dt->rCloseHit.x += shift;
            dt->titlePos.x += shift;
        }
    }

    // tab-slide animation: rVisible.x currently holds each tab's freshly computed slot (its
    // target). When animating, keep the previous animated x and let the timer ease it toward
    // the slot; otherwise snap. The dragged tab always snaps (it tracks the cursor).
    for (int i = 0; i < nTabs; i++) {
        TabInfo* ti = GetTab(i);
        if (ti->rVisible.IsEmpty()) {
            ti->animInit = false; // hidden: snap into place when it next appears
            continue;
        }
        int slotX = ti->rVisible.x;
        ti->targetX = slotX;
        bool snap = !tabsAnimating || !ti->animInit || (i == draggedIdx);
        if (snap) {
            ti->animX = slotX;
        }
        ti->animInit = true;
        int off = ti->animX - slotX;
        if (off != 0) {
            ti->rVisible.x += off;
            ti->r.x += off;
            ti->rClose.x += off;
            ti->rCloseHit.x += off;
            ti->titlePos.x += off;
        }
    }

    HwndTabsSetItemSize(hwnd, tabSize);
}

// Finds the index of the tab, which contains the given point.
TabsCtrl::MouseState TabsCtrl::TabStateFromMousePosition(const Point& p) {
    TabsCtrl::MouseState res;
    Point pt = p;
    if (pt.x < 0 || pt.y < 0) {
        return res;
    }

    for (auto& gh : groupHeaders) {
        if (!gh.rHeader.Contains(pt)) {
            continue;
        }
        res.overGroupHeader = true;
        res.overGroupCaret = gh.rCaret.Contains(pt);
        res.groupId = gh.groupId;
        return res;
    }

    int nTabs = TabCount();
    for (int i = 0; i < nTabs; i++) {
        TabInfo* ti = tabs[i];
        Rect r = ti->rVisible;
        if (!r.Contains(pt)) {
            continue;
        }
        res.tabIdx = i;
        bool isSelected = (i == GetSelected());
        bool closeActive = isSelected || r.dx >= kMinTabWidthForClose;
        res.overClose = closeActive && ti->rCloseHit.Contains(pt);
        res.tabInfo = ti;
        Rect rightHalf = r;
        int halfDx = r.dx / 2;
        rightHalf.x = r.x + halfDx;
        rightHalf.dx = halfDx;
        res.inRightHalf = rightHalf.Contains(pt);
        return res;
    }

    return res;
}

Gdiplus::Color GdipCol(COLORREF c) {
    return GdiRgbFromCOLORREF(c);
}

static void FillRoundedRect(Graphics& gfx, SolidBrush& br, Rect r, int radius) {
    radius = std::min(radius, std::min(r.dx, r.dy) / 2);
    if (radius <= 0) {
        gfx.FillRectangle(&br, ToGdipRect(r));
        return;
    }
    int d = radius * 2;
    GraphicsPath path;
    path.AddArc(r.x, r.y, d, d, 180, 90);
    path.AddArc(r.x + r.dx - d, r.y, d, d, 270, 90);
    path.AddArc(r.x + r.dx - d, r.y + r.dy - d, d, d, 0, 90);
    path.AddArc(r.x, r.y + r.dy - d, d, d, 90, 90);
    path.CloseFigure();
    Gdiplus::SmoothingMode prev = gfx.GetSmoothingMode();
    gfx.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
    gfx.FillPath(&br, &path);
    gfx.SetSmoothingMode(prev);
}

// like FillRoundedRect but only the top two corners are rounded (Chrome-style tab)
static void FillTopRoundedRect(Graphics& gfx, SolidBrush& br, Rect r, int radius) {
    radius = std::min(radius, std::min(r.dx, r.dy) / 2);
    if (radius <= 0) {
        gfx.FillRectangle(&br, ToGdipRect(r));
        return;
    }
    int d = radius * 2;
    GraphicsPath path;
    path.AddArc(r.x, r.y, d, d, 180, 90);            // top-left corner
    path.AddArc(r.x + r.dx - d, r.y, d, d, 270, 90); // top-right corner
    path.AddLine(r.x + r.dx, r.y + r.dy, r.x, r.y + r.dy); // square bottom edge
    path.CloseFigure();
    Gdiplus::SmoothingMode prev = gfx.GetSmoothingMode();
    gfx.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
    gfx.FillPath(&br, &path);
    gfx.SetSmoothingMode(prev);
}

static COLORREF TabTextColorForBackground(COLORREF tabBg) {
    COLORREF text = ThemeWindowTextColor();
    if (abs((int)GetLightness(text) - (int)GetLightness(tabBg)) >= 80) {
        return text;
    }
    return IsLightColor(tabBg) ? RGB(0, 0, 0) : RGB(255, 255, 255);
}

bool TabsCtrl::IsValidIdx(int idx) {
    return idx >= 0 && idx < TabCount();
}

void TabsCtrl::Paint(HDC hdc, const RECT& rc) {
    Point cursorPos = HwndGetCursorPos(hwnd);
    Rect clientRc = ClientRect(hwnd);
    bool mouseInside = clientRc.Contains(cursorPos);
    TabsCtrl::MouseState tabState;
    if (mouseInside) {
        tabState = TabStateFromMousePosition(cursorPos);
    }
    int tabUnderMouse = tabState.tabIdx;
    bool overClose = tabState.overClose && tabState.tabInfo && tabState.tabInfo->canClose;
    int selectedIdx = GetSelected();
    if (IsValidIdx(tabForceShowSelected)) {
        selectedIdx = tabForceShowSelected;
    }

    Graphics gfx(hdc);
    gfx.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
    gfx.SetCompositingQuality(CompositingQualityHighQuality);
    gfx.SetSmoothingMode(Gdiplus::SmoothingModeNone);
    gfx.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    gfx.SetPageUnit(UnitPixel);

    SolidBrush br(GdipCol(ThemeControlBackgroundColor()));
    Font f(hdc, GetFont());

    Gdiplus::Rect gr = ToGdipRect(rc);
    gfx.FillRectangle(&br, gr);

    StringFormat sf(StringFormat::GenericDefault());
    sf.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    sf.SetLineAlignment(StringAlignmentCenter);
    sf.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
    if (IsTabsRtl(hwnd)) {
        sf.SetAlignment(Gdiplus::StringAlignmentFar);
    }

    int n = TabCount();
    COLORREF tabBgSelected = ThemeControlBackgroundColor();
    COLORREF tabBgBackground = AccentColor(tabBgSelected, 25);
    COLORREF tabBgHighlight = AccentColor(tabBgSelected, 35);

    // SourceOver so the rounded tab corners anti-alias against the control background
    gfx.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
    // when dragging in-strip, the dragged tab is drawn as a single unit (background + text +
    // close) AFTER the normal paint loops, so a sliding neighbor's text can never bleed over
    // the dragged tab's background. -1 when not dragging in-strip.
    int dragDrawIdx = (draggingTab && !dragDetached) ? GetSelected() : -1;

    // resolves a tab's background fill color (shared by the bg pill and the close-button bg)
    auto tabBgColorOf = [&](int i, bool isSelected, bool isUnderMouse) -> COLORREF {
        TabInfo* ti = GetTab(i);
        COLORREF tabBgCol = tabBgBackground;
        if (isSelected) {
            tabBgCol = tabBgSelected;
        } else if (isUnderMouse) {
            tabBgCol = tabBgHighlight;
        }
        if (!IsSpecialColor(ti->tabColor)) {
            tabBgCol = ti->tabColor;
            if (!isSelected) {
                tabBgCol = AccentColor(ti->tabColor, isUnderMouse ? 35 : 25);
            }
        }
        return tabBgCol;
    };

    // draws one tab's rounded background pill
    auto paintTabBg = [&](int i) {
        TabInfo* ti = GetTab(i);
        if (!ti || ti->rVisible.IsEmpty()) {
            return;
        }
        bool isSelected = selectedIdx == i;
        bool isUnderMouse = tabUnderMouse == i;
        COLORREF tabBgCol = tabBgColorOf(i, isSelected, isUnderMouse);
        // Chrome-like rounded tabs; a larger radius when active/hovered for a softer feel
        int tabRadius = DpiScale(hwnd, (isSelected || isUnderMouse) ? 9 : 6);
        br.SetColor(GdipCol(tabBgCol));
        FillTopRoundedRect(gfx, br, ti->rVisible, tabRadius);
    };

    // draws one tab's foreground (label, dirty dot, close button)
    auto paintTabFg = [&](int i) {
        TabInfo* ti = GetTab(i);
        if (!ti || ti->rVisible.IsEmpty()) {
            return;
        }
        bool isSelected = selectedIdx == i;
        bool isUnderMouse = tabUnderMouse == i;
        COLORREF tabBgCol = tabBgColorOf(i, isSelected, isUnderMouse);
        COLORREF textColor = TabTextColorForBackground(tabBgCol);

        Rect rClose = ti->rClose;
        Gdiplus::RectF rTxt = ToGdipRectF(ti->rVisible);
        if (IsTabsRtl(hwnd)) {
            rTxt.X += (8 + rClose.dx);
        } else {
            rTxt.X += 8;
        }
        rTxt.Width -= (8 + rClose.dx + 8);
        br.SetColor(GdipCol(textColor));
        TempWStr ws = ToWStrTemp(ti->text);
        gfx.DrawString(ws, -1, &f, rTxt, &sf, &br);

        if (ti->isDirty) {
            gfx.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
            Gdiplus::RectF bounds;
            gfx.MeasureString(ws, -1, &f, rTxt, &sf, &bounds);
            int dotRadius = DpiScale(hwnd, 3);
            int dotX = (int)(bounds.X + bounds.Width) + dotRadius;
            int maxX = (int)(rTxt.X + rTxt.Width) - dotRadius * 2;
            if (dotX > maxX) {
                dotX = maxX;
            }
            int dotY = ti->rVisible.y + (ti->rVisible.dy - dotRadius * 2) / 2;
            SolidBrush redBr(Color(255, 0xEE, 0x22, 0x22));
            gfx.FillEllipse(&redBr, dotX, dotY, dotRadius * 2, dotRadius * 2);
            gfx.SetSmoothingMode(Gdiplus::SmoothingModeNone);
        }

        bool closeVisible = ti->canClose && (isSelected || (isUnderMouse && ti->rVisible.dx >= kMinTabWidthForClose));
        if (closeVisible) {
            DrawCloseButtonArgs closeArgs;
            closeArgs.hdc = hdc;
            closeArgs.r = ti->rClose;
            closeArgs.isHover = overClose && isUnderMouse;
            closeArgs.colBg = tabBgCol;
            DrawCloseButton(closeArgs);
        }
    };

    // pass 1: all tab backgrounds (the dragged tab is skipped and drawn on top at the end)
    for (int i = 0; i < n; i++) {
        if (i == dragDrawIdx) {
            continue;
        }
        paintTabBg(i);
    }

    gfx.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
    for (auto& gh : groupHeaders) {
        COLORREF headerBase = GetGroupHeaderColor(this, gh.groupId, tabBgBackground);
        bool isHoveredHeader = tabState.overGroupHeader && tabState.groupId == gh.groupId;
        COLORREF headerBg = AccentColor(headerBase, isHoveredHeader ? 45 : 30);
        COLORREF headerFg = TabTextColorForBackground(headerBg);

        // Chrome-style group underline running across the chip + all member tabs
        int ulPad = DpiScale(hwnd, 2);
        int ulDy = DpiScale(hwnd, 3);
        Rect rUnder = {gh.rTabs.x + ulPad, gh.rTabs.y + gh.rTabs.dy - ulDy, gh.rTabs.dx - 2 * ulPad, ulDy};
        if (rUnder.dx > 0) {
            br.SetColor(GdipCol(headerBase));
            FillRoundedRect(gfx, br, rUnder, ulDy / 2);
        }

        int pillPadX = DpiScale(hwnd, 2);
        int pillPadY = DpiScale(hwnd, 4);
        Rect rPill = {gh.rHeader.x + pillPadX, gh.rHeader.y + pillPadY, gh.rHeader.dx - 2 * pillPadX,
                      gh.rHeader.dy - 2 * pillPadY};
        br.SetColor(GdipCol(headerBg));
        FillRoundedRect(gfx, br, rPill, DpiScale(hwnd, 6));

        // no caret arrow: the colored pill itself is the affordance, and collapse
        // state is conveyed by whether the group's member tabs are visible
        if (!str::IsEmpty(gh.label)) {
            int labelPad = DpiScale(hwnd, 8);
            Rect rLabel = {gh.rHeader.x + labelPad, gh.rHeader.y, gh.rHeader.dx - 2 * labelPad, gh.rHeader.dy};
            if (rLabel.dx > DpiScale(hwnd, 8)) {
                Gdiplus::RectF rLabelTxt = ToGdipRectF(rLabel);
                StringFormat headerSf(StringFormat::GenericDefault());
                headerSf.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
                headerSf.SetLineAlignment(StringAlignmentCenter);
                headerSf.SetAlignment(StringAlignmentCenter);
                headerSf.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
                br.SetColor(GdipCol(headerFg));
                TempWStr wsHeader = ToWStrTemp(gh.label);
                gfx.DrawString(wsHeader, -1, &f, rLabelTxt, &headerSf, &br);
            }
        }
    }

    // pass 2: all tab foregrounds (the dragged tab is skipped and drawn on top at the end)
    for (int i = 0; i < n; i++) {
        if (i == dragDrawIdx) {
            continue;
        }
        paintTabFg(i);
    }

    // finally, the dragged tab as a single unit on top of everything (background + text +
    // close), so a sliding neighbor's text never overlaps the floating dragged tab
    if (dragDrawIdx >= 0) {
        paintTabBg(dragDrawIdx);
        paintTabFg(dragDrawIdx);
    }
}

HBITMAP TabsCtrl::RenderForDragging(int idx) {
    TabInfo* ti = GetTab(idx);
    if (!ti) {
        return nullptr;
    }
    Rect rDrag = ti->rVisible.IsEmpty() ? ti->r : ti->rVisible;
    Bitmap bitmap(rDrag.dx, rDrag.dy);
    Graphics* gfx = Graphics::FromImage(&bitmap);
    // DrawString() on a bitmap does not work with CompositingModeSourceCopy - obscure bug.
    gfx->SetCompositingMode(Gdiplus::CompositingModeSourceOver);
    gfx->SetCompositingQuality(CompositingQualityHighQuality);
    gfx->SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
    gfx->SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    gfx->SetPageUnit(UnitPixel);

    StringFormat sf(StringFormat::GenericDefault());
    sf.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    sf.SetLineAlignment(StringAlignmentCenter);
    sf.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);

    COLORREF bgCol = tabSelectedBg;
    COLORREF textCol = tabSelectedText;

    SolidBrush br(GdipCol(bgCol));
    Gdiplus::Rect gr(0, 0, rDrag.dx, rDrag.dy);
    gfx->FillRectangle(&br, gr);

    HDC hdc = GetDC(hwnd);
    Font f(hdc, GetFont());
    ReleaseDC(hwnd, hdc);

    Gdiplus::RectF rTxt(0, 0, rDrag.dx, rDrag.dy);
    rTxt.X += 8;
    rTxt.Width -= (8 + 8);
    br.SetColor(GdipCol(textCol));
    TempWStr ws = ToWStrTemp(ti->text);
    gfx->DrawString(ws, -1, &f, rTxt, &sf, &br);

    HBITMAP ret;
    bitmap.GetHBITMAP(Color(255, 255, 255), &ret);
    delete gfx;
    return ret;
}

TabsCtrl::TabsCtrl() {
    kind = kindTabs;
}

// must be called after LayoutTabs()
static void TabsCtrlUpdateAfterChangingTabsCount(TabsCtrl* tabs) {
    HWND hwnd = tabs->hwnd;
    if (GetCapture() == hwnd) {
        ReleaseCapture();
    }
    tabs->tabBeingClosed = -1;
    Point mousePos = HwndGetCursorPos(hwnd);
    auto tabState = tabs->TabStateFromMousePosition(mousePos);
    bool canClose = tabState.tabInfo && tabState.tabInfo->canClose;
    bool overClose = tabState.overClose && canClose;
    int tabUnderMouse = tabState.tabIdx;
    tabs->tabHighlighted = tabUnderMouse;
    tabs->tabHighlightedClose = overClose ? tabUnderMouse : -1;
    if (tabs->draggingTab) {
        tabs->draggingTab = false;
        ImageList_EndDrag();
    }
}

TabsCtrl::~TabsCtrl() {}

static void TriggerSelectionChanged(TabsCtrl* tabs) {
    if (!tabs->onSelectionChanged.IsValid()) {
        return;
    }
    TabsCtrl::SelectionChangedEvent ev;
    ev.tabs = tabs;
    tabs->onSelectionChanged.Call(&ev);
}

static bool TriggerSelectionChanging(TabsCtrl* tabs) {
    if (!tabs->onSelectionChanging.IsValid()) {
        // allow changing
        return false;
    }

    TabsCtrl::SelectionChangingEvent ev;
    tabs->onSelectionChanging.Call(&ev);
    return (LRESULT)ev.preventChanging;
}

static void TriggerTabMigration(TabsCtrl* tabs, int tabIdx, Point p) {
    if (!tabs->onTabMigration.IsValid()) {
        return;
    }
    TabsCtrl::MigrationEvent ev;
    ev.tabs = tabs;
    ev.tabIdx = tabIdx;
    ev.releasePoint = p;
    tabs->onTabMigration.Call(&ev);
}

static void TriggerTabClosed(TabsCtrl* tabs, int tabIdx) {
    if ((tabIdx < 0) || !tabs->onTabClosed.IsValid()) {
        return;
    }
    TabsCtrl::ClosedEvent ev;
    ev.tabs = tabs;
    ev.tabIdx = tabIdx;
    tabs->onTabClosed.Call(&ev);
}

static void TriggerTabDragged(TabsCtrl* tabs, int tab1, int tab2) {
    if (!tabs->onTabDragged.IsValid()) {
        return;
    }
    TabsCtrl::DraggedEvent ev;
    ev.tabs = tabs;
    ev.tab1 = tab1;
    ev.tab2 = tab2;
    tabs->onTabDragged.Call(&ev);
}

static void TriggerGroupHeaderClick(TabsCtrl* tabs, int groupId) {
    if (groupId < 0 || !tabs->onGroupHeaderClick.IsValid()) {
        return;
    }
    TabsCtrl::GroupHeaderClickEvent ev;
    ev.tabs = tabs;
    ev.groupId = groupId;
    tabs->onGroupHeaderClick.Call(&ev);
}

static void TriggerTabGroupDrop(TabsCtrl* tabs, int tabIdx, int groupId) {
    if (tabIdx < 0 || !tabs->onTabGroupDrop.IsValid()) {
        return;
    }
    TabsCtrl::GroupDropEvent ev;
    ev.tabs = tabs;
    ev.tabIdx = tabIdx;
    ev.groupId = groupId;
    tabs->onTabGroupDrop.Call(&ev);
}

static void UpdateAfterDrag(TabsCtrl* tabsCtrl, int tabIdxFrom, int tabIdxTo) {
    int nTabs = tabsCtrl->TabCount();
    bool badState =
        (tabIdxFrom == tabIdxTo) || (tabIdxFrom < 0) || (tabIdxTo < 0) || (tabIdxFrom >= nTabs) || (tabIdxTo > nTabs);
    if (badState) {
        logfa("tabIdxFrom: %d, tabIdxTo: %d, nTabs: %d\n", tabIdxFrom, tabIdxTo, nTabs);
        ReportDebugIf(true);
        return;
    }

    auto&& tabs = tabsCtrl->tabs;
    TabInfo* moved = tabs.At(tabIdxFrom);
    tabs.RemoveAt(tabIdxFrom);
    if (tabIdxFrom < tabIdxTo) {
        // we moved from left to right e.g. from 1 to 3
        // after removing 1 we insert not at 3 but 2
        tabIdxTo -= 1;
    }
    tabs.InsertAt(tabIdxTo, moved);
    tabsCtrl->SetSelected(tabIdxTo);
    tabsCtrl->LayoutTabs();
    TabsCtrlUpdateAfterChangingTabsCount(tabsCtrl);
}

// reorders the tab at `from` to index `to`, reusing the drag-reorder path
void TabsCtrl::MoveTabToIndex(int from, int to) {
    if (from == to) {
        return;
    }
    UpdateAfterDrag(this, from, to);
}

// which group "owns" horizontal position x, ignoring the dragged tab `excludeIdx`:
// over a group's chip or one of its member tabs → that group; in a gap flanked by the
// same group → that group; over empty space or an ungrouped tab → none (-1).
// this lets a member be pulled out of a group by dragging it past the group's edge.
static int GroupAtX(TabsCtrl* tabs, int excludeIdx, int x) {
    for (auto& gh : tabs->groupHeaders) {
        Rect h = gh.rHeader;
        if (x >= h.x && x < h.x + h.dx) {
            return gh.groupId;
        }
    }
    int n = tabs->TabCount();
    for (int i = 0; i < n; i++) {
        if (i == excludeIdx) {
            continue;
        }
        Rect r = tabs->GetTab(i)->rVisible;
        if (!r.IsEmpty() && x >= r.x && x < r.x + r.dx) {
            return tabs->GetTab(i)->visualTabGroupId;
        }
    }
    // in a gap: "inside" a group only if flanked on both sides by the same group
    int leftG = -1, rightG = -1, bestL = -1000000000, bestR = 1000000000;
    for (int i = 0; i < n; i++) {
        if (i == excludeIdx) {
            continue;
        }
        Rect r = tabs->GetTab(i)->rVisible;
        if (r.IsEmpty()) {
            continue;
        }
        if (r.x + r.dx <= x && r.x > bestL) {
            bestL = r.x;
            leftG = tabs->GetTab(i)->visualTabGroupId;
        }
        if (r.x >= x && r.x < bestR) {
            bestR = r.x;
            rightG = tabs->GetTab(i)->visualTabGroupId;
        }
    }
    if (leftG != -1 && leftG == rightG) {
        return leftG;
    }
    return -1;
}

// in-strip reorder while dragging: moves the dragged tab to index `to` and selects it,
// WITHOUT releasing capture / ending the drag or repainting (caller re-lays out)
void TabsCtrl::ReorderDuringDrag(int from, int to) {
    if (from < 0 || to < 0 || from == to) {
        return;
    }
    int n = TabCount();
    if (from >= n || to >= n) {
        return;
    }
    TabInfo* moved = tabs.At(from);
    tabs.RemoveAt(from);
    tabs.InsertAt(to, moved);
    SendMessageW(hwnd, WM_SETREDRAW, FALSE, 0);
    TabCtrl_SetCurSel(hwnd, to);
    SendMessageW(hwnd, WM_SETREDRAW, TRUE, 0);
}

void TabsCtrl::StartTabAnimation() {
    if (!tabsAnimating) {
        tabsAnimating = true;
        SetTimer(hwnd, kTabAnimTimerId, 15, nullptr);
    }
}

// advance each tab's animated x one step toward its slot; returns true while still moving
bool TabsCtrl::AnimateTick() {
    int draggedIdx = (draggingTab && !dragDetached) ? GetSelected() : -1;
    bool anyMoving = false;
    int n = TabCount();
    for (int i = 0; i < n; i++) {
        TabInfo* ti = GetTab(i);
        if (!ti || ti->rVisible.IsEmpty() || i == draggedIdx) {
            continue;
        }
        int delta = ti->targetX - ti->animX;
        if (delta == 0) {
            continue;
        }
        int step = delta / 3;
        if (delta >= -2 && delta <= 2) {
            step = delta; // close enough: snap this frame
        } else if (step == 0) {
            step = (delta > 0) ? 1 : -1;
        }
        ti->animX += step;
        ti->rVisible.x += step;
        ti->r.x += step;
        ti->rClose.x += step;
        ti->rCloseHit.x += step;
        ti->titlePos.x += step;
        if (ti->animX != ti->targetX) {
            anyMoving = true;
        }
    }
    return anyMoving;
}

LRESULT TabsCtrl::OnNotifyReflect(WPARAM wp, LPARAM lp) {
    NMHDR* hdr = (NMHDR*)lp;
    switch (hdr->code) {
        case TCN_SELCHANGING:
            return (LRESULT)TriggerSelectionChanging(this);

        case TCN_SELCHANGE:
            TriggerSelectionChanged(this);
            HwndScheduleRepaint(hwnd);
            break;

        case TTN_GETDISPINFOA:
        case TTN_GETDISPINFOW:
            break;
    }
    return 0;
}

static bool CanDragTab(TabInfo* tab) {
    if (tab->isPinned) return false;
    return true;
}

LRESULT TabsCtrl::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Point mousePos = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    if (WM_MOUSELEAVE == msg) {
        mousePos = HwndGetCursorPos(hwnd);
    }

    TabsCtrl::MouseState tabState;

    bool overClose = false;
    bool canClose = true;
    int tabUnderMouse = -1;

    if ((msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || (msg == WM_MOUSELEAVE)) {
        tabState = TabStateFromMousePosition(mousePos);
        tabUnderMouse = tabState.tabIdx;
        canClose = tabState.tabInfo && tabState.tabInfo->canClose;
        overClose = tabState.overClose && canClose;
        lastMousePos = mousePos;
        // TempStr msgName = WinMsgNameTemp(msg);
        //  logfa("msg; %s, tabUnderMouse: %d, overClose: %d\n", msgName, tabUnderMouse, (int)overClose);
    }

    if (draggingTab && msg == WM_MOUSEMOVE) {
        int stripDy = ClientRect(hwnd).dy;
        int detach = DpiScale(hwnd, 30);
        bool wantDetached = (mousePos.y > stripDy + detach) || (mousePos.y < -detach);
        if (wantDetached) {
            POINT p(mousePos.x, mousePos.y);
            MapWindowPoints(hwnd, NULL, &p, 1);
            if (!dragDetached) {
                // pulled out of the strip: start the floating tear-off image
                dragDetached = true;
                int hl = GetSelected();
                HBITMAP hbmp = (hl >= 0) ? RenderForDragging(hl) : nullptr;
                if (hbmp) {
                    TabInfo* thl = GetTab(hl);
                    HIMAGELIST himl = ImageList_Create(thl->r.dx, thl->r.dy, 0, 1, 0);
                    ImageList_Add(himl, hbmp, NULL);
                    ImageList_BeginDrag(himl, 0, grabLocation.x, grabLocation.y);
                    DeleteObject(hbmp);
                    DeleteObject(himl);
                    ImageList_DragEnter(NULL, p.x, p.y);
                }
                HwndScheduleRepaint(hwnd);
            }
            ImageList_DragMove(p.x, p.y);
            return 0;
        }
        if (dragDetached) {
            // came back into the strip: drop the floating image, resume in-strip drag
            dragDetached = false;
            ImageList_EndDrag();
        }
        // in-strip: the dragged tab floats with the cursor; other tabs shift around the gap,
        // and its group is whatever region the float currently sits over
        dragMouseX = mousePos.x;
        int from = GetSelected();
        if (from >= 0) {
            int floatCenter = dragMouseX - grabLocation.x + tabSize.dx / 2;
            int nTabs = TabCount();
            int target = 0;
            for (int i = 0; i < nTabs; i++) {
                if (i == from) {
                    continue;
                }
                TabInfo* t = GetTab(i);
                if (t->rVisible.IsEmpty()) {
                    continue;
                }
                // compare against the slot (targetX), not the mid-animation position
                if (floatCenter > t->targetX + t->rVisible.dx / 2) {
                    target++;
                }
            }
            if (target != from) {
                ReorderDuringDrag(from, target);
                from = GetSelected();
            }
            GetTab(from)->visualTabGroupId = GroupAtX(this, from, floatCenter);
            StartTabAnimation(); // neighbours slide to their new slots
            LayoutTabs();
            HwndScheduleRepaint(hwnd);
        }
        return 0;
    }

    // Check if mouse has moved beyond system drag threshold
    bool beyondDragThreshold = false;
    if (msg == WM_MOUSEMOVE && GetCapture() == hwnd && !draggingTab) {
        if (tabHighlighted >= 0 && tabHighlighted < TabCount()) {
            int cxDrag = GetSystemMetrics(SM_CXDRAG);
            int cyDrag = GetSystemMetrics(SM_CYDRAG);
            beyondDragThreshold = (abs(mousePos.x - grabLocation.x - GetTab(tabHighlighted)->rVisible.x) > cxDrag) ||
                                  (abs(mousePos.y - grabLocation.y - GetTab(tabHighlighted)->rVisible.y) > cyDrag);
        }
    }

    switch (msg) {
        case WM_NCHITTEST: {
            if (false) {
                return HTCLIENT;
            }
            // parts that are HTTRANSPARENT are used to move the window
            if (!inTitleBar || hwnd == GetCapture()) {
                return HTCLIENT;
            }
            HwndScreenToClient(hwnd, mousePos);
            tabState = TabStateFromMousePosition(mousePos);
            if (tabState.tabIdx >= 0 || tabState.overGroupHeader) {
                return HTCLIENT;
            }
            return HTTRANSPARENT;
        }

        case WM_SIZE:
            LayoutTabs();
            break;

        case WM_TIMER:
            if (wp == kTabAnimTimerId) {
                bool moving = AnimateTick();
                if (!moving) {
                    KillTimer(hwnd, kTabAnimTimerId);
                    tabsAnimating = false;
                }
                HwndScheduleRepaint(hwnd);
                return 0;
            }
            break;

        case WM_MOUSELEAVE:
            if (tabWidthFrozen) {
                tabWidthFrozen = false;
                LayoutTabs();
            }
            if (tabHighlighted != tabUnderMouse || tabHighlightedClose != -1) {
                tabHighlighted = tabUnderMouse;
                tabHighlightedClose = -1;
                HwndScheduleRepaint(hwnd);
            }
            break;

        case WM_MOUSEMOVE: {
            TrackMouseLeave(hwnd);
            bool isDragging = (GetCapture() == hwnd);
            int hl = tabHighlighted;
            if (isDragging && beyondDragThreshold) {
                if (GetSelected() < 0) {
                    return 0;
                }
                // begin an in-strip drag: no floating image; the tab reorders within the
                // strip and only tears off into a floating image if pulled out (handled above)
                draggingTab = true;
                dragDetached = false;
                dragMouseX = mousePos.x;
                return 0;
            }

            if (hl != tabUnderMouse) {
                tabHighlighted = tabUnderMouse;
                // logf("tab: WM_MOUSEMOVE: tabHighlighted = tabUnderMouse: %d\n", tabHighlighted);
                // note: hl == -1 possible repro: we start drag, a file gets loaded via DDE etc.
                // which re-layouts tabs and mouse is no longer over a tab
                if (isDragging && hl != -1 && tabUnderMouse != -1) {
                    // send notification if the highlighted tab is dragged over another
                    if (!CanDragTab(GetTab(tabUnderMouse))) {
                        TriggerTabDragged(this, hl, tabUnderMouse);
                        UpdateAfterDrag(this, hl, tabUnderMouse);
                    }
                } else {
                    // highlight a different tab
                    HwndScheduleRepaint(hwnd);
                }
                return 0;
            }
            int xHl = -1;
            if (overClose && !isDragging) {
                xHl = hl;
            }
            // logfa("inX=%d, hl=%d, xHl=%d, xHighlighted=%d\n", (int)inX, hl, xHl, tab->xHighlighted);
            if (tabHighlightedClose != xHl) {
                // logfa("before invalidate, xHl=%d, xHighlited=%d\n", xHl, tab->xHighlighted);
                tabHighlightedClose = xHl;
                HwndScheduleRepaint(hwnd);
            }
            return 0;
        }

        case WM_LBUTTONDOWN: {
            if (tabState.overGroupHeader) {
                tabHighlighted = -1;
                tabHighlightedClose = -1;
                tabBeingClosed = -1;
                TriggerGroupHeaderClick(this, tabState.groupId);
                HwndScheduleRepaint(hwnd);
                return 0;
            }
            tabHighlighted = tabUnderMouse;
            if (overClose) {
                HwndScheduleRepaint(hwnd);
                tabBeingClosed = tabUnderMouse;
                return 0;
            }
            if (tabUnderMouse < 0) {
                return 0;
            }

            int selectedTab = GetSelected();
            if (tabUnderMouse != selectedTab) {
                bool stopChange = TriggerSelectionChanging(this);
                if (stopChange) {
                    return 0;
                }
                SetSelected(tabUnderMouse);
                TriggerSelectionChanged(this);
                // LoadModelIntoTab() can pump messages; ensure tabs are fully painted.
                HwndRepaintNow(hwnd);
            }
            TabInfo* ti = GetTab(tabUnderMouse);
            if (ti->isPinned) {
                return 0;
            }

            grabLocation.x = mousePos.x - ti->rVisible.x;
            grabLocation.y = mousePos.y - ti->rVisible.y;
            SetCapture(hwnd);
            return 0;
        }

        case WM_LBUTTONUP: {
            bool isDragging = (GetCapture() == hwnd);
            if (isDragging) {
                ReleaseCapture();
            }
            if (tabBeingClosed != -1 && tabUnderMouse == tabBeingClosed && overClose) {
                // freeze tab widths so next close button stays under cursor
                // unfreezes when mouse leaves the tab control
                frozenTabDx = tabSize.dx;
                tabWidthFrozen = true;
                // send notification that the tab is closed
                TriggerTabClosed(this, tabBeingClosed);
                // TriggerTabClosed() might have destroyed the window and this TabsCtrl
                if (!FindMainWindowByHwnd(hwnd)) {
                    return 0;
                }
                HwndScheduleRepaint(hwnd);
                tabBeingClosed = -1;
                return 0;
            }
            // we don't always get WM_MOUSEMOVE before WM_LBUTTONUP so
            // update tabHighlighted
            tabHighlighted = tabUnderMouse;

            if (!draggingTab) {
                return 0;
            }
            draggingTab = false;
            int selectedTab = GetSelected();

            if (dragDetached) {
                dragDetached = false;
                ImageList_EndDrag();
                // torn out of the strip → migrate to a new/other window
                POINT p(mousePos.x, mousePos.y);
                ClientToScreen(hwnd, &p);
                Point scPoint(p.x, p.y);
                TriggerTabMigration(this, selectedTab, scPoint);
                return 0;
            }

            // in-strip drop: the tab was live-reordered into place and its visual group was
            // updated as it moved; commit that membership to the model. animate the dropped
            // tab from the cursor into its final slot.
            StartTabAnimation();
            TriggerTabGroupDrop(this, selectedTab, GetTab(selectedTab)->visualTabGroupId);
            HwndScheduleRepaint(hwnd);
            return 0;
        }

        case WM_MBUTTONDOWN: {
            // middle-clicking unconditionally closes the tab

            tabBeingClosed = tabUnderMouse;
            if (tabBeingClosed < 0 || !canClose) {
                return 0;
            }
            TriggerTabClosed(this, tabBeingClosed);
            // TriggerTabClosed() might have destroyed the window and this TabsCtrl
            if (!FindMainWindowByHwnd(hwnd)) {
                return 0;
            }
            HwndScheduleRepaint(hwnd);
            return 0;
        }

        case WM_ERASEBKGND:
            // We paint the full client in WM_PAINT. Don't erase here: TabCtrl_SetCurSel
            // invalidates native (LTR) item rects while we lay out tabs manually in RTL.
            return TRUE;

        case WM_NCPAINT:
            return 0; // prevent native tab control from drawing its edge

        case WM_NCCALCSIZE:
            return 0; // remove non-client area so no edge is reserved

        case WM_PAINT: {
            // TabCtrl_SetCurSel invalidates native (LTR) item rects; we lay out tabs
            // manually (RTL tabs start from the right). Avoid BeginPaint's clip region.
            RECT clientRc = ClientRECT(hwnd);
            HDC hdc = GetDC(hwnd);
            DoubleBuffer buffer(hwnd, ToRect(clientRc));
            Paint(buffer.GetDC(), clientRc);
            buffer.Flush(hdc);
            ReleaseDC(hwnd, hdc);
            ValidateRect(hwnd, nullptr);
            return 0;
        }
    }

    return WndProcDefault(hwnd, msg, wp, lp);
}

HWND TabsCtrl::Create(TabsCtrl::CreateArgs& args) {
    CreateControlArgs cargs;
    cargs.parent = args.parent;
    cargs.isRtl = args.isRtl;
    cargs.font = args.font;
    cargs.className = WC_TABCONTROLW;
    withToolTips = args.withToolTips;
    tabDefaultDx = args.tabDefaultDx;

    cargs.style = WS_CHILD | WS_CLIPSIBLINGS | WS_VISIBLE | TCS_FOCUSNEVER | TCS_FIXEDWIDTH | TCS_FORCELABELLEFT;
    if (withToolTips) {
        cargs.style |= TCS_TOOLTIPS;
    }

    HWND hwnd = CreateControl(cargs);
    if (!hwnd) {
        return nullptr;
    }

    if (withToolTips) {
        HWND ttHwnd = GetToolTipsHwnd();
        SetWindowStyle(ttHwnd, TTS_NOPREFIX, true);
    }
    return hwnd;
}

Size TabsCtrl::GetIdealSize() {
    Size sz{32, 128};
    return sz;
}

int TabsCtrl::TabCount() {
    int n = TabCtrl_GetItemCount(hwnd);
    return n;
}

// takes ownership of tab
int TabsCtrl::InsertTab(int idx, TabInfo* tab) {
    ReportIf(idx < 0);
    TCITEMW item{};
    item.mask = TCIF_TEXT;
    item.pszText = ToWStrTemp(tab->text);
    int res = TabCtrl_InsertItem(hwnd, idx, &item);
    if (res < 0) {
        return res;
    }
    tabs.InsertAt(idx, tab);
    // LayoutTabs() must be before SetSelected() because SetSelected()
    // triggers sync repaint which paints tab texts in wrong positions
    // because we didn't position them yet in layout.
    LayoutTabs();
    SetSelected(idx);
    TabsCtrlUpdateAfterChangingTabsCount(this);
    return idx;
}

void TabsCtrl::SetTextAndTooltip(int idx, const char* text, const char* tooltip) {
    TabInfo* tab = GetTab(idx);
    str::ReplaceWithCopy(&tab->text, text);
    str::ReplaceWithCopy(&tab->tooltip, tooltip);
    LayoutTabs();
    HwndScheduleRepaint(hwnd);
}

void TabsCtrl::SetTabDirty(int idx, bool dirty) {
    TabInfo* tab = GetTab(idx);
    if (tab && tab->isDirty != dirty) {
        tab->isDirty = dirty;
        LayoutTabs(); // rebuilds tooltips from current ti->tooltip values
        // LayoutTabs only schedules a repaint; force it so the dirty (red dot)
        // indicator updates immediately (e.g. right after editing a form field)
        HwndRepaintNow(hwnd);
    }
}

// returns userData because it's not owned by TabsCtrl
UINT_PTR TabsCtrl::RemoveTab(int idx) {
    ReportIf(idx < 0);
    ReportIf(idx >= TabCount());
    BOOL ok = TabCtrl_DeleteItem(hwnd, idx);
    ReportIf(!ok);
    TabInfo* tab = tabs[idx];
    UINT_PTR userData = tab->userData;
    tabs.RemoveAt(idx);
    delete tab;
    int selectedTab = GetSelected();
    if (idx < selectedTab) {
        SetSelected(selectedTab - 1);
    } else if (idx == selectedTab) {
        SetSelected(0);
    }
    LayoutTabs();
    TabsCtrlUpdateAfterChangingTabsCount(this);
    return userData;
}

void TabsCtrl::SwapTabs(int idx1, int idx2) {
    TabInfo* tmp = tabs[idx1];
    tabs[idx1] = tabs[idx2];
    tabs[idx2] = tmp;
}

// Note: the caller should take care of deleting userData
void TabsCtrl::RemoveAllTabs() {
    TabCtrl_DeleteAllItems(hwnd);
    DeleteVecMembers(tabs);
    tabs.Reset();
    LayoutTabs();
    TabsCtrlUpdateAfterChangingTabsCount(this);
}

TabInfo* TabsCtrl::GetTab(int idx) {
    // defensive: a bad index (e.g. -1 from TabCtrl_GetCurSel when nothing is
    // selected, or tabUnderMouse/tabHighlighted == -1 during a DDE-triggered
    // reload) would otherwise read tabs[idx] out of bounds. Report and bail so
    // call sites fail safe instead of indexing the Vec with a negative index.
    bool badIdx = idx < 0 || idx >= TabCount();
    ReportIf(badIdx);
    if (badIdx) {
        return nullptr;
    }
    return tabs[idx];
}

int TabsCtrl::GetSelected() {
    int idx = TabCtrl_GetCurSel(hwnd);
    return idx;
}

int TabsCtrl::SetSelected(int idx) {
    int nTabs = TabCount();
    if (idx < 0 || idx >= nTabs) {
        logf("TabsCtrl::SetSelected(): idx: %d, TabsCount(): %d\n", idx, nTabs);
    }
    ReportIf(idx < 0 || idx >= nTabs);
    // Suppress native tab invalidation (LTR item rects); we repaint the full client.
    SendMessageW(hwnd, WM_SETREDRAW, FALSE, 0);
    int prevSelectedIdx = TabCtrl_SetCurSel(hwnd, idx);
    SendMessageW(hwnd, WM_SETREDRAW, TRUE, 0);
    HwndRepaintNow(hwnd);
    return prevSelectedIdx;
}

void TabsCtrl::SetHighlighted(int idx) {
    int oldSelectedIdx = GetSelected();
    if (IsValidIdx(tabForceShowSelected)) {
        oldSelectedIdx = tabForceShowSelected;
    }
    int newSelectedIdx = GetSelected();
    if (IsValidIdx(idx)) {
        newSelectedIdx = idx;
    }
    if (tabForceShowSelected == idx) {
        return;
    }
    tabForceShowSelected = idx;
    if (oldSelectedIdx == newSelectedIdx) {
        return;
    }
    HwndRepaintNow(hwnd);
}

HWND TabsCtrl::GetToolTipsHwnd() {
    HWND res = TabCtrl_GetToolTips(hwnd);
    return res;
}

