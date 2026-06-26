/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"

#include "AppTools.h"

#include "utils/FileUtil.h"
#include "utils/WinUtil.h"
#include "utils/StrFormat.h"
#include "utils/ScopedWin.h"
#include "utils/SettingsUtil.h"

#include "wingui/UIModels.h"

#include "Settings.h"
#include "DocController.h"
#include "EngineBase.h"
#include "GlobalPrefs.h"
#include "Flags.h"
#include "Commands.h"
#include "MainWindow.h"
#include "WindowTab.h"

#include <float.h>
#include <math.h>

// must be last to over-write assert()
#include "utils/UtAssert.h"

#define utassert_fequal(a, b) utassert(fabs(a - b) < FLT_EPSILON);

static void ParseCommandLineTest() {
    {
        Flags i;
        ParseFlags(GetLifetimeArena(), L"SumatraPDF.exe -bench foo.pdf", i);
        utassert(2 == i.pathsToBenchmark.Size());
        utassert(str::Eq("foo.pdf", i.pathsToBenchmark.At(0)));
        utassert(nullptr == i.pathsToBenchmark.At(1));
    }

    {
        Flags i;
        ParseFlags(GetLifetimeArena(), L"SumatraPDF.exe -bench foo.pdf -fwdsearch-width 5", i);
        utassert(i.globalPrefArgs.Size() == 2);
        const char* s = i.globalPrefArgs.At(0);
        utassert(str::Eq(s, "-fwdsearch-width"));
        s = i.globalPrefArgs.At(1);
        utassert(str::Eq(s, "5"));
        utassert(2 == i.pathsToBenchmark.Size());
        utassert(str::Eq("foo.pdf", i.pathsToBenchmark.At(0)));
        utassert(nullptr == i.pathsToBenchmark.At(1));
    }

    {
        Flags i;
        ParseFlags(GetLifetimeArena(), L"SumatraPDF.exe -bench bar.pdf loadonly", i);
        utassert(2 == i.pathsToBenchmark.Size());
        utassert(str::Eq("bar.pdf", i.pathsToBenchmark.At(0)));
        utassert(str::Eq("loadonly", i.pathsToBenchmark.At(1)));
    }

    {
        Flags i;
        ParseFlags(GetLifetimeArena(), L"SumatraPDF.exe -bench bar.pdf 1 -set-color-range 0x123456 #abCDef", i);
        utassert(i.globalPrefArgs.Size() == 3);
        utassert(2 == i.pathsToBenchmark.Size());
        utassert(str::Eq("bar.pdf", i.pathsToBenchmark.At(0)));
        utassert(str::Eq("1", i.pathsToBenchmark.At(1)));
    }

    {
        Flags i;
        ParseFlags(GetLifetimeArena(), L"SumatraPDF.exe -bench bar.pdf 1-5,3   -bench some.pdf 1,3,8-34", i);
        utassert(4 == i.pathsToBenchmark.Size());
        utassert(str::Eq("bar.pdf", i.pathsToBenchmark.At(0)));
        utassert(str::Eq("1-5,3", i.pathsToBenchmark.At(1)));
        utassert(str::Eq("some.pdf", i.pathsToBenchmark.At(2)));
        utassert(str::Eq("1,3,8-34", i.pathsToBenchmark.At(3)));
    }

    {
        Flags i;
        ParseFlags(GetLifetimeArena(), L"SumatraPDF.exe -presentation -bgcolor 0xaa0c13 foo.pdf -invert-colors bar.pdf",
                   i);
        utassert(true == i.enterPresentation);
        utassert(true == i.invertColors);
        utassert(2 == i.fileNames.Size());
        utassert(0 == i.fileNames.Find("foo.pdf"));
        utassert(1 == i.fileNames.Find("bar.pdf"));
    }

    {
        Flags i;
        ParseFlags(GetLifetimeArena(), L"SumatraPDF.exe -bg-color 0xaa0c13 -invertcolors rosanna.pdf", i);
        utassert(true == i.invertColors);
        utassert(1 == i.fileNames.Size());
        utassert(0 == i.fileNames.Find("rosanna.pdf"));
    }

    {
        Flags i;
        ParseFlags(GetLifetimeArena(), LR"(SumatraPDF.exe "foo \" bar \\.pdf" un\"quoted.pdf)", i);
        utassert(2 == i.fileNames.Size());
        utassert(0 == i.fileNames.Find(R"(foo " bar \\.pdf)"));
        utassert(1 == i.fileNames.Find(R"(un"quoted.pdf)"));
    }

    {
        Flags i;
        ParseFlags(
            GetLifetimeArena(),
            L"SumatraPDF.exe -page 37 -view continuousfacing -zoom fitcontent -scroll 45,1234         -reuse-instance",
            i);
        utassert(0 == i.fileNames.Size());
        utassert(i.pageNumber == 37);
        utassert(i.startView == DisplayMode::ContinuousFacing);
        utassert(i.startZoom == kZoomFitContent);
        utassert(i.startScroll.x == 45 && i.startScroll.y == 1234);
    }

    {
        Flags i;
        ParseFlags(GetLifetimeArena(), LR"(SumatraPDF.exe -view "single page" -zoom 237.45 -scroll -21,-1)", i);
        utassert(0 == i.fileNames.Size());
        utassert(i.startView == DisplayMode::SinglePage);
        utassert(i.startZoom == 237.45f);
        utassert(i.startScroll.x == -21 && i.startScroll.y == -1);
    }

    {
        Flags i;
        ParseFlags(GetLifetimeArena(), L"SumatraPDF.exe -zoom 35%", i);
        utassert(0 == i.fileNames.Size());
        utassert(i.startZoom == 35.f);
    }

    {
        Flags i;
        ParseFlags(GetLifetimeArena(), L"SumatraPDF.exe -zoom fit-content", i);
        utassert(i.startZoom == kZoomFitContent);
        utassert(0 == i.fileNames.Size());
    }
}

static void BenchRangeTest() {
    utassert(IsBenchPagesInfo("1"));
    utassert(IsBenchPagesInfo("2-4"));
    utassert(IsBenchPagesInfo("5,7"));
    utassert(IsBenchPagesInfo("6,8,"));
    utassert(IsBenchPagesInfo("1-3,4,6-9,13"));
    utassert(IsBenchPagesInfo("2-"));
    utassert(IsBenchPagesInfo("loadonly"));

    utassert(!IsBenchPagesInfo(""));
    utassert(!IsBenchPagesInfo("-2"));
    utassert(!IsBenchPagesInfo("2--4"));
    utassert(!IsBenchPagesInfo("4-2"));
    utassert(!IsBenchPagesInfo("1-3,loadonly"));
    utassert(!IsBenchPagesInfo(nullptr));
}

// TODO: disabled because they bring too many dependencies
static void versioncheck_test() {
    utassert(IsValidProgramVersion("1"));
    utassert(IsValidProgramVersion("1.1"));
    utassert(IsValidProgramVersion("1.1.1\r\n"));
    utassert(IsValidProgramVersion("2662"));

    utassert(!IsValidProgramVersion("1.1b"));
    utassert(!IsValidProgramVersion("1..1"));
    utassert(!IsValidProgramVersion("1.1\r\n.1"));

    utassert(CompareProgramVersion("0.9.3.900", "0.9.3") > 0);
    utassert(CompareProgramVersion("1.09.300", "1.09.3") > 0);
    utassert(CompareProgramVersion("1.9.1", "1.09.3") < 0);
    utassert(CompareProgramVersion("1.2.0", "1.2") == 0);
    utassert(CompareProgramVersion("1.3.0", "2662") < 0);
}

static void hexstrTest() {
    u8 buf[6] = {1, 2, 33, 255, 0, 18};
    u8 buf2[6]{};
    AutoFreeStr s = _MemToHex(&buf);
    utassert(str::Eq(s, "010221ff0012"));
    bool ok = _HexToMem(s, &buf2);
    utassert(ok);
    utassert(memeq(buf, buf2, sizeof(buf)));

    FILETIME ft1, ft2;
    GetSystemTimeAsFileTime(&ft1);
    s.Set(_MemToHex(&ft1));
    _HexToMem(s, &ft2);
    DWORD diff = FileTimeDiffInSecs(ft1, ft2);
    utassert(0 == diff);
    utassert(FileTimeEq(ft1, ft2));

    s.Set(str::MemToHex(nullptr, 0));
    utassert(str::Eq(s, ""));
    ok = str::HexToMem(s, nullptr, 0);
    utassert(ok);
}

static void assertSerializedColor(COLORREF c, const char* s) {
    TempStr s2 = SerializeColorTemp(c);
    utassert(str::Eq(s2, s));
}

static void visualTabGroupsTest() {
    VisualTabGroupState state;

    VisualTabGroup* g1 = state.CreateDefaultGroup();
    utassert(g1 != nullptr);
    utassert(g1->id == 1);
    utassert(str::Eq(g1->name, "Group 1"));
    utassert(g1->color == MkColor(213, 90, 90));
    utassert(!g1->collapsed);
    utassert(state.FindGroup(g1->id) == g1);

    VisualTabGroup* g2 = state.CreateGroupWithId(7, "Research", MkColor(20, 30, 40));
    utassert(g2 != nullptr);
    utassert(g2->id == 7);
    utassert(g2->name != nullptr);
    utassert(str::Eq(g2->name, "Research"));
    utassert(g2->color == MkColor(20, 30, 40));
    utassert(state.FindGroup(g2->id) == g2);

    state.DeleteGroupById(g1->id);
    utassert(state.FindGroup(g1->id) == nullptr);
    utassert(state.FindGroup(g2->id) == g2);
    utassert(state.nextId == 8);
}

static void visualTabGroupCollapsedVisibilityTest() {
    VisualTabGroupState state;
    VisualTabGroup* group = state.CreateGroupWithId(3, "Research", MkColor(0x33, 0x66, 0x99));
    utassert(group != nullptr);

    group->collapsed = false;
    utassert(ShouldShowGroupedTab(state, group->id, false));
    utassert(ShouldShowGroupedTab(state, group->id, true));

    group->collapsed = true;
    utassert(!ShouldShowGroupedTab(state, group->id, false));
    utassert(ShouldShowGroupedTab(state, group->id, true));

    utassert(ShouldShowGroupedTab(state, 99, false));
    utassert(ShouldShowGroupedTab(state, -1, false));
}

static void visualTabGroupPersistenceTest() {
    auto saveRef = [](const VisualTabGroupState& state, int groupId, VisualTabGroupRef& ref) {
        ref.groupId = -1;
        str::ReplaceWithCopy(&ref.name, nullptr);
        str::ReplaceWithCopy(&ref.color, nullptr);
        ref.collapsed = false;
        VisualTabGroup* group = state.FindGroup(groupId);
        if (!group) {
            return;
        }
        ref.groupId = group->id;
        str::ReplaceWithCopy(&ref.name, group->name);
        str::ReplaceWithCopy(&ref.color, SerializeColorTemp(group->color));
        ref.collapsed = group->collapsed;
    };
    auto restoreGroup = [](VisualTabGroupState& state, int& groupId, const VisualTabGroupRef& ref) {
        groupId = -1;
        if (ref.groupId == -1) {
            return;
        }
        VisualTabGroup* group = state.FindGroup(ref.groupId);
        if (!group) {
            group = state.CreateGroupWithId(ref.groupId, ref.name, ParseColor(ref.color, kColorUnset));
        }
        if (group) {
            group->collapsed = ref.collapsed;
            groupId = group->id;
        }
    };

    VisualTabGroupState state;
    VisualTabGroup* group = state.CreateGroupWithId(7, "Research", MkColor(0x11, 0x22, 0x33));
    utassert(group != nullptr);
    group->collapsed = true;

    FileState fs{};
    TabState ts{};
    saveRef(state, 7, fs.visualTabGroup);
    saveRef(state, 7, ts.visualTabGroup);

    state.DeleteGroupById(7);
    utassert(state.FindGroup(7) == nullptr);

    utassert(fs.visualTabGroup.groupId == 7);
    utassert(str::Eq(fs.visualTabGroup.name, "Research"));
    utassert(ParseColor(fs.visualTabGroup.color, kColorUnset) == MkColor(0x11, 0x22, 0x33));
    utassert(fs.visualTabGroup.collapsed == true);

    utassert(ts.visualTabGroup.groupId == 7);
    utassert(str::Eq(ts.visualTabGroup.name, "Research"));
    utassert(ParseColor(ts.visualTabGroup.color, kColorUnset) == MkColor(0x11, 0x22, 0x33));
    utassert(ts.visualTabGroup.collapsed == true);

    VisualTabGroupState restoredTabState;
    int restoredTabGroupId = -1;
    restoreGroup(restoredTabState, restoredTabGroupId, ts.visualTabGroup);
    utassert(restoredTabGroupId == 7);
    VisualTabGroup* restoredGroup = restoredTabState.FindGroup(7);
    utassert(restoredGroup != nullptr);
    utassert(restoredGroup->id == 7);
    utassert(str::Eq(restoredGroup->name, "Research"));
    utassert(restoredGroup->color == MkColor(0x11, 0x22, 0x33));
    utassert(restoredGroup->collapsed == true);

    VisualTabGroupState restoredFileState;
    int reopenedTabGroupId = -1;
    restoreGroup(restoredFileState, reopenedTabGroupId, fs.visualTabGroup);
    utassert(reopenedTabGroupId == 7);
    VisualTabGroup* reopenedGroup = restoredFileState.FindGroup(7);
    utassert(reopenedGroup != nullptr);
    utassert(reopenedGroup->id == 7);
    utassert(str::Eq(reopenedGroup->name, "Research"));
    utassert(reopenedGroup->color == MkColor(0x11, 0x22, 0x33));
    utassert(reopenedGroup->collapsed == true);

    str::Free(fs.visualTabGroup.name);
    str::Free(fs.visualTabGroup.color);
    str::Free(ts.visualTabGroup.name);
    str::Free(ts.visualTabGroup.color);
}


static void colorTest() {
    COLORREF c = 0;
    bool ok = ParseColor(&c, "0x01020304");
    utassert(ok);
    assertSerializedColor(c, "#01020304");

    ok = ParseColor(&c, "#01020304");
    utassert(ok);
    assertSerializedColor(c, "#01020304");

    COLORREF c2 = MkColor(2, 3, 4, 1);
    assertSerializedColor(c2, "#01020304");
    utassert(c == c2);

    c2 = MkColor(5, 7, 6, 8);
    assertSerializedColor(c2, "#08050706");
    ok = ParseColor(&c, "#08050706");
    utassert(ok);
    utassert(c == c2);
}

static void assertGoToNextPage3(int cmdId) {
    auto cmd = FindCustomCommand(cmdId);
    utassert(cmd->origId == CmdGoToNextPage);
    auto arg = GetCommandArg(cmd, kCmdArgN);
    utassert(arg->intVal == 3);
}

void parseCommandsTest() {
    CommandArg* arg;

    {
        auto cmd = CreateCommandFromDefinition(" CmdCreateAnnotHighlight   #00ff00 openEdit copytoclipboard");
        utassert(cmd->origId == CmdCreateAnnotHighlight);

        arg = GetCommandArg(cmd, kCmdArgColor);
        utassert(arg != nullptr);
        arg = GetCommandArg(cmd, kCmdArgOpenEdit);
        utassert(arg != nullptr);
        utassert(GetCommandBoolArg(cmd, kCmdArgOpenEdit, false) == true);
        utassert(GetCommandBoolArg(cmd, kCmdArgCopyToClipboard, false) == true);
    }
    {
        auto cmd = CreateCommandFromDefinition(" CmdCreateAnnotHighlight   #00ff00 OpenEdit=yes");
        utassert(cmd->origId == CmdCreateAnnotHighlight);

        utassert(GetCommandArg(cmd, kCmdArgColor) != nullptr);
        utassert(GetCommandArg(cmd, kCmdArgOpenEdit) != nullptr);
        utassert(GetCommandBoolArg(cmd, kCmdArgOpenEdit, false) == true);
    }
    {
        {
            auto cmd = CreateCommandFromDefinition("CmdGoToNextPage 3");
            assertGoToNextPage3(cmd->id);
        }
        {
            auto cmd = CreateCommandFromDefinition("CmdGoToNextPage n 3");
            assertGoToNextPage3(cmd->id);
        }
        {
            auto cmd = CreateCommandFromDefinition("CmdGoToNextPage n: 3");
            assertGoToNextPage3(cmd->id);
        }
        {
            auto cmd = CreateCommandFromDefinition("CmdGoToNextPage n=3");
            assertGoToNextPage3(cmd->id);
        }
    }
    {
        const char* argStr = R"("C:\Program Files\FoxitReader\FoxitReader.exe" /A page=%p "%1)";
        const char* s = str::JoinTemp("CmdExec   ", argStr);
        auto cmd = CreateCommandFromDefinition(s);
        utassert(cmd->origId == CmdExec);
        auto cmd2 = FindCustomCommand(cmd->id);
        utassert(cmd == cmd2);
        arg = GetCommandArg(cmd, kCmdArgExe);
        utassert(str::Eq(arg->strVal, argStr));
    }
    {
        const char* argStr = R"("C:\Program Files\FoxitReader\FoxitReader.exe" /A page=%p "%1)";
        const char* s = str::JoinTemp("CmdExec  filter: *.jpeg ", argStr);
        auto cmd = CreateCommandFromDefinition(s);
        utassert(cmd->origId == CmdExec);
        auto cmd2 = FindCustomCommand(cmd->id);
        utassert(cmd == cmd2);
        arg = GetCommandArg(cmd, kCmdArgExe);
        utassert(str::Eq(arg->strVal, argStr));
        arg = GetCommandArg(cmd, kCmdArgFilter);
        utassert(str::Eq(arg->strVal, "*.jpeg"));
    }
}

void SumatraPDF_UnitTests() {
    parseCommandsTest();
    visualTabGroupsTest();
    visualTabGroupCollapsedVisibilityTest();
    visualTabGroupPersistenceTest();
    colorTest();
    BenchRangeTest();
    ParseCommandLineTest();
    versioncheck_test();
    hexstrTest();
}












