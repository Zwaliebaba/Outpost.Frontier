// Author: Giperion.

#include "nn_mission_vendor.h"

#include "character_select_equip.h" // GetPlayerShipArchetypeIdbaseline (the routine)
#include "globals.h"                // GetClientFrameTimeCapped (the routine)
#include "hud_command_dispatch.h"   // HudPushCancelHandler (the routine)
#include "ids_rdl.h"                // AppendTextMaybeXmlRdl (the routine), RdlAppendNode
#include "knowledge_map.h"          // KnowledgeMapRevealFmtStr (the routine)
#include "login_mgr.h"              // G_Server (the field)
#include "menu_universe_base.h"     // GetClientLocationBaseMissionOffers (the routine),
                                    // GetClientCurrentLocationRuntime (the routine),
                                    // GetClientLocationCurrentBase (the field)
#include "mirror_property_bucket.h"
#include "nav_bar.h"                // NavBarCloseService (the routine)
#include "misc_util.h"              // FmtStrSprintf (the routine),
                                    // MiscUtilLoadMaterialLibrary (the routine)
#include "player_spawn.h"           // GetPlayerConnectionId (the field)
#include "rich_text_win.h"
#include "room_control.h"           // GetRoomRuntimeBaseId (+0x34)
#include "ui_scroll.h"
#include "win3d.h"                  // Win3DFindByName, Win3DFindByClassName, Win3DDeleteTree
#include "win_factory.h"
#include "wire_controls.h"          // WireControlsJoinColumn (the routine)

#include "NeuronRuntime.h"
#include "common_goods.h"           // MissionVendorProperties, MissionVendorAcceptance
#include "common_server.h"          // IServer::MissionResponse
#include "common_text.h"            // FmtStr, RenderDisplayList, StyleNode
#include "neuroncomponents.h"                  // FDUMP
#include "render/shading.h"         // IMaterialLibrary::DeleteGroup

#include <new>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

// WINFACTORY<NN_MissionVendor> static (the branch -> the routine;
// Create the routine; atexit the branch -> the routine).
WINFACTORY<NnMissionVendor> G_NnMissionVendorFactory("NN_MissionVendor");

int G_MissionVendorNoMissionsReason = 0; // (.data, zero)

namespace
{
    char const kMissionVendorCpp[] = "Client\\nn\\NN_MissionVendor.cpp";
    char const kNoMaterialInVendor[] = "%s(%d) : *** ERROR: Cannot find material %s in vendor\n";
    char const kVideoMaterialName[] = "MV_vidmaterial";
    char const kVideoTextureLibrary[] = "Interface\\NeuroNet\\MissionVendor\\MissionVendor.txm";
    char const kVideoScreenMesh[] = "Interface\\NeuroNet\\MissionVendor\\MV_videoscreen.3db";
    char const kHeadlineIconMesh[] = "Interface\\NeuroNet\\NewsVendor\\NEWS_headlineicon.3db";

    // the tooltip ids of the seven icons (MissionVendorProperties +0x10 indexes
    // it; read only at 0x0048c9cb and 0x0048d2f7).
    int const G_MissionVendorIconIds[7] = {0, 0x32fa, 0x32fb, 0x32fc, 0x32fd, 0x32fe, 0x32ff};

    // The seven paths Create stores into IconPaths (0x0048c36b..0x0048c3b8), in that order.
    char const* const kMissionIconMeshes[7] = {
        kHeadlineIconMesh,
        "Interface\\NeuroNet\\MissionVendor\\MV_missiondestroy.3db",
        "Interface\\NeuroNet\\MissionVendor\\MV_missiondestroyinstall.3db",
        "Interface\\NeuroNet\\MissionVendor\\MV_missionassassinate.3db",
        "Interface\\NeuroNet\\MissionVendor\\MV_missiondestroycontra.3db",
        "Interface\\NeuroNet\\MissionVendor\\MV_missionbounty.3db",
        "Interface\\NeuroNet\\MissionVendor\\MV_missionretrieve.3db",
    };

    // / the branch / the branch -- three float statics of this translation unit
    // (the field = the field = -0.6, the field = 0.6). Each has one XREF, its
    // initializer's write; nothing reads them.
    float G_MissionVendorStatic672030 = -0.6f;
    float G_MissionVendorStatic67202c = -0.6f;
    float G_MissionVendorStatic672028 = 0.6f;

    uintptr_t PointerArg(void const* Pointer)
    {
        return reinterpret_cast<uintptr_t>(Pointer);
    }

    // The reward line of RefreshRows (0x0048ca54..0x0048cae2) and ShowSelected
    // (0x0048cf03..0x0048cf95): mad-lib 0x732 with the credits, or 0x741 without any.
    void FormatReward(MissionVendorProperties const& Offer, wchar_t* Buffer)
    {
        FmtStr Reward;
        if (Offer.IconTextureId != 0)
        {
            Reward.begin_mad_lib(0x732);
            Reward.append_int(Offer.IconTextureId);
        }
        else
        {
            Reward.begin_mad_lib(0x741);
        }
        Reward.end_mad_lib();
        FmtStrSprintf(&Reward, Buffer, 0x400);
    }

    // The full-text idiom of ShowSelected (0x0048cd79..0x0048ce6b / 0x0048cfd4..0x0048d0c6): a
    // style node 0x9ca4, the text (XML when it parses), then the pane cleared, refilled and
    // scrolled to the top.
    void SetFullText(RichTextWin* Pane, wchar_t const* Buffer)
    {
        RenderDisplayList Text;
        {
            StyleNode Style(0x9ca4);
            RdlAppendNode(Text, Style.Clone());
        }
        AppendTextMaybeXmlRdl(Buffer, static_cast<unsigned>(wcslen(Buffer)), Text, false);
        Pane->ClearContent();
        Pane->AppendContent(Text);
        Pane->SetScrollOrigin(0, 0);
    }
}

NnMissionVendor::NnMissionVendor()
    : ResultCallback(nullptr)
    , ResultContext(nullptr)
    , bCloseRequested(0)
    , ScrollOffset(0)
    , Field_0x374(0)
    , Selected(0)
    , Offers(nullptr)
    , OfferCount(0)
    , IconPaths(nullptr)
    , IconPathCount(0)
{
    // . The members above are 0x0048b390..0x0048b3ae / 0x0048b3c3..0x0048b3d5.
    // FmtStr::destroy, then its six dwords zeroed.
    StatusText.destroy();
    memset(static_cast<void*>(&StatusText), 0, sizeof(StatusText));
    // the string emptied (erase(0, npos) inlined), the routine.
    VideoTexture.clear();
    VideoMaterial.Reset();
    // . MaterialGroup (+0x348) is not written.
    Clicked = 0;
    bNoMissions = 0;
    Hovered = -1;
}

NnMissionVendor::~NnMissionVendor()
{
    // . 0x0048b54a..0x0048b593: the status text emptied, the offer array
    // (vector delete, MissionVendorProperties' dtor) and the path array freed.
    StatusText.destroy();
    delete[] Offers;
    ::operator delete(static_cast<void*>(IconPaths));

    ResetVideo();
    // MaterialLib slot 10.
    NeuronRuntime::MaterialLib->DeleteGroup(MaterialGroup);
    MaterialGroup = -1;
    // Then ~FmtStr, the routine (~FSwappedMaterial), the string, the routine and the routine
    // -- the members' and bases' destructors in that order.
}

uintptr_t NnMissionVendor::RowDword(size_t ArrayOffset, int Index) const
{
    uintptr_t Value = 0;
    unsigned char const* const Bytes = reinterpret_cast<unsigned char const*>(this);
    memcpy(&Value, Bytes + ArrayOffset + static_cast<ptrdiff_t>(Index) * 4, sizeof(Value));
    return Value;
}

void NnMissionVendor::DeleteSelf()
{
    Win3D* Child = ChildHead;
    while (Child != nullptr)
    {
        Win3D* const Next = Child->NextSibling;
        Win3DDeleteTree(Child);
        Child = Next;
    }
    Destroy();   // slot 10
    delete this; // slot 0 with 1
}

void NnMissionVendor::ResetVideo()
{
    VideoTexture.clear();
    VideoMaterial.Release();
    VideoTexture.clear();    // ?erase@ (0, npos)
    VideoMaterial.Reset();
}

void NnMissionVendor::RevealSelectedOffer()
{
    // e.g. 0x0048c3e2..0x0048c40b: title, description, briefing.
    MissionVendorProperties const& Offer = Offers[Selected];
    // on each (the KnowledgeMap reveal).
    KnowledgeMapRevealFmtStr(&Offer.Title);
    KnowledgeMapRevealFmtStr(&Offer.Description);
    KnowledgeMapRevealFmtStr(&Offer.Briefing);
}

bool NnMissionVendor::Create(Win3D* NewParent, void* Data)
{
    // (RET 8). 0x0048b6eb..0x0048b781: extents, hit offset and the base position.
    HalfExtents[0] = 0.43f; // +0xb4
    HalfExtents[1] = 0.3f;
    HalfExtents[2] = 0.0f;
    HitOffset[0] = 0.0f;    // +0xc0
    HitOffset[1] = 0.033f;
    HitOffset[2] = -1.0f;
    BasePosition[0] = 0.0f;
    BasePosition[1] = -0.1f;
    BasePosition[2] = -1.498f;
    float const X = BasePosition[0];
    float const Y = BasePosition[1];
    float const Z = BasePosition[2];
    float const Zero[3] = {0.0f, 0.0f, 0.0f};


    {
        float const Position[3] = {X, Y, Z};
        Background = CreateStaticControl("MissionVendorBackground",
                                         "Interface\\NeuroNet\\NewsVendor\\NEWS_background.cmp",
                                         Position, nullptr, Zero, 0, 0, 0, 1, 1, 0, 1, 0, 0, 0, Zero);
    }

    {
        float const Position[3] = {X + 0.584f, Y + 0.455f, Z};
        float const Half[3] = {0.025f, 0.025f, 0.0f};
        CloseButton = CreateButtonControl("MissionVendorCloseButton",
                                          "Interface\\NeuroNet\\Inventory\\x.3db", nullptr,
                                          "ui_select_item", nullptr, Position, nullptr, Half, 0, 0,
                                          0, 0, 0x72e, 0, 0);
    }

    {
        float const Position[3] = {X - 0.6f, Y + 0.486f, Z};
        Title = CreateTextControl("MissionVendorTitle", "PageHeader", Position, nullptr, 1.2f, 0.03f,
                                  2.0f, 0x72b, 1, 0, 1);
    }

    // the offers of the base of the current room (the routine's +0x34),
    // counted with the routine.
    TMirrorPropertyBucket<MissionVendorProperties>* const Bucket = GetClientLocationBaseMissionOffers(
        GetRoomRuntimeBaseId(static_cast<FRoomRuntime*>(GetClientCurrentLocationRuntime())));
    int Count = 0;
    MetaNode<MissionVendorProperties>* Node = nullptr;
    while (Bucket->List.traverse(Node, true))
        ++Count;

    if (Count == 0)
    {
        // why there is nothing to offer.
        bNoMissions = 1;
        unsigned Ids = 0;
        if (GetPlayerShipArchetypeIdbaseline() == 0)
        {
            Ids = 0x3b0;
        }
        else
        {
            // Jump table 0x0048c444; above 4 keeps 0.
            switch (G_MissionVendorNoMissionsReason)
            {
            case 0: Ids = 0x3d0; break;
            case 1: Ids = 0x384; break;
            case 2: Ids = 0x385; break;
            case 3: Ids = 0x386; break;
            case 4: Ids = 0x3d0; break;
            default: break;
            }
        }
        float const Position[3] = {-0.54f, -0.01f, BasePosition[2]};
        CreateTextControl("MissionVendorNoMissions", "SubHeader", Position, nullptr, 1.055f, 0.03f,
                          2.0f, Ids, 1, 0, 1);
        HudPushCancelHandler(AsHudCmdHandler(), 0);
        return Win3D::Create(NewParent, Data);
    }

    // the three frames.
    {
        float const Position[3] = {X - 0.215f, Y + 0.217f, Z};
        CreateStaticControl("MissionVendorHeadlinesFrame",
                            "Interface\\NeuroNet\\NewsVendor\\NEWS_headlineframe.3db", Position,
                            nullptr, Zero, 0, 0, 0, 1, 1, 0, 1, 0, 0, 0, Zero);
    }
    {
        float const Position[3] = {X - 0.006f, Y - 0.149f, Z};
        CreateStaticControl("MissionVendorStoryFrame",
                            "Interface\\NeuroNet\\NewsVendor\\NEWS_textframe.3db", Position, nullptr,
                            Zero, 0, 0, 0, 1, 1, 0, 1, 0, 0, 0, Zero);
    }
    {
        float const Position[3] = {X + 0.364f, Y + 0.288f, Z};
        CreateStaticControl("MissionVendorVideoFrame",
                            "Interface\\NeuroNet\\NewsVendor\\NEWS_videowindow.3db", Position,
                            nullptr, Zero, 0, 0, 0, 1, 1, 0, 1, 0, 0, 0, Zero);
    }
    // the video screen's material group, then the screen itself.
    MaterialGroup = MiscUtilLoadMaterialLibrary(kVideoScreenMesh);
    {
        float const Position[3] = {X + 0.364f, Y + 0.288f, Z};
        CreateStaticControl("MissionVendorVideoScreen", kVideoScreenMesh, Position, nullptr, Zero, 0,
                            0, 0, 1, 1, 0, 1, 0, 0, 0, Zero);
    }
    // the accept caption and button.
    {
        float const Position[3] = {X + 0.237f, Y + 0.105f, Z};
        CreateTextControl("MissionVendorAcceptText", "ButtonText", Position, nullptr, 0.277f, 0.03f,
                          2.0f, 0x3e9, 1, 0, 1);
    }
    {
        float const Position[3] = {X + 0.37f, Y + 0.078f, Z};
        float const Half[3] = {0.133f, 0.026f, 0.0f};
        AcceptButton = CreateWireButtonControl("MissionVendorAcceptButton", "ui_execute_transaction",
                                               Position, Half, 0, 0, 0x331, 0, 0);
    }

    // five rows, 0.0615 apart from {x - 0.39, y + 0.36}.
    float const Top[3] = {X - 0.39f, Y + 0.36f, Z};
    char Name[0x100];
    for (int Row = 0; Row < 5; ++Row)
    {
        // FILD Row, FMUL 0.0615f, subtracted at extended precision and
        // stored once as float.
        float const RowPos[3] = {
            Top[0],
            static_cast<float>(static_cast<double>(Top[1]) -
                               static_cast<double>(Row) * static_cast<double>(0.0615f)),
            Top[2]};

        sprintf(Name, "MissionVendorSummaryIcon%d", Row);
        {
            float const Position[3] = {RowPos[0] - 0.125f, RowPos[1], RowPos[2]};
            SummaryIcons[Row] = CreateStaticControl(Name, kHeadlineIconMesh, Position, nullptr, Zero, 0,
                                                    0, 0, 1, 0, 0, 1, 0, 0, 0, Zero);
        }
        sprintf(Name, "MissionVendorIconFrame%d", Row);
        {
            float const Position[3] = {RowPos[0] - 0.125f, RowPos[1], RowPos[2]};
            float const Half[3] = {0.03f, 0.03f, 0.0f};
            IconFrames[Row] = CreateWireButtonControl(Name, "ui_select_item", Position, Half, 0, 0,
                                                      0x72c, 0, 0);
        }
        sprintf(Name, "MissionVendorSummaryBox%d", Row);
        {
            float const Position[3] = {RowPos[0] + 0.185f, RowPos[1], RowPos[2]};
            float const Half[3] = {0.245f, 0.031f, 0.0f};
            SummaryBoxes[Row] = CreateWireButtonControl(Name, nullptr, Position, Half, 0, 0, 0x72c, 0,
                                                        0);
        }
        sprintf(Name, "MissionVendorSummaryDest%d", Row);
        {
            float const Position[3] = {RowPos[0] + 0.195f, RowPos[1] + 0.017f, RowPos[2]};
            SummaryDests[Row] = CreateTextControl(Name, "NewsMission", Position, nullptr, 0.49f, 0.03f,
                                                  1.5f, 0, 0, 0, 0);
        }
        sprintf(Name, "MissionVendorSummaryTitle%d", Row);
        {
            float const Position[3] = {RowPos[0] + 0.195f, RowPos[1] - 0.009f, RowPos[2]};
            SummaryTitles[Row] = CreateTextControl(Name, "NewsMission", Position, nullptr, 0.49f,
                                                   0.03f, 1.5f, 0, 0, 0, 0);
        }
        sprintf(Name, "MissionVendorSummaryReward%d", Row);
        {
            float const Position[3] = {RowPos[0] + 0.293f, RowPos[1] + 0.004f, RowPos[2]};
            SummaryRewards[Row] = CreateTextControl(Name, "NewsMission", Position, nullptr, 0.27f,
                                                    0.03f, 1.5f, 0, 2, 0, 0);
        }
    }

    // the scroll bar (0..5, page 5).
    {
        float const Position[3] = {X + 0.09f, Y + 0.397f, Z};
        SummaryScroll = CreateUIScroll("MissionVendorSummaryScroll", Position, 0.019f, 0.317f, 0, 5,
                                       5, 0);
    }
    // the selected offer's lines (each name goes through sprintf into
    // the name buffer first; none has a conversion).
    {
        float const Position[3] = {X - 0.493f, Y + 0.0085f, Z};
        MissionReject = CreateTextControl("MissionVendorMissionReject", "Normal", Position, nullptr,
                                          0.7f, 0.03f, 1.8f, 0x72f, 0, 0, 1);
    }
    {
        float const Position[3] = {X - 0.494f, Y + 0.022f, Z};
        MissionDest = CreateTextControl("MissionVendorMissionDest", "NewsMission", Position, nullptr,
                                        0.7f, 0.03f, 1.5f, 0, 0, 0, 1);
    }
    {
        float const Position[3] = {X - 0.494f, Y - 0.006f, Z};
        MissionTitle = CreateTextControl("MissionVendorMissionTitle", "NewsMission", Position,
                                         nullptr, 0.7f, 0.03f, 1.5f, 0, 0, 0, 1);
    }
    {
        float const Position[3] = {X + 0.348f, Y + 0.009f, Z};
        MissionReward = CreateTextControl("MissionVendorMissionReward", "NewsMission", Position,
                                          nullptr, 0.2f, 0.03f, 1.5f, 0, 2, 0, 1);
    }

    {
        float const Position[3] = {X - 0.556f, Y - 0.051f, Z};
        FullText = CreateRichTextWin("MissionVendorMissionFullText", 1.08f, 0.27f, Position, true);
    }
    // the selected offer's icon (fixed x / y, the base's z).
    {
        float const Position[3] = {-0.535f, -0.111f, BasePosition[2]};
        float const Half[3] = {0.03f, 0.03f, 0.0f};
        MissionIcon = CreateStaticControl("MissionVendorIcon", kHeadlineIconMesh, Position, nullptr,
                                          Half, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, Zero);
    }


    DrawLayer = 0x64;
    IconPathCount = 7;
    IconPaths = static_cast<char const**>(::operator new(0x1c));
    for (int Index = 0; Index < 7; ++Index)
        IconPaths[Index] = kMissionIconMeshes[Index];


    LoadOffers();
    RefreshRows();
    ShowSelected();
    if (Selected != -1)
        RevealSelectedOffer();


    HudPushCancelHandler(AsHudCmdHandler(), 0);
    return Win3D::Create(NewParent, Data);
}

void NnMissionVendor::LoadOffers()
{
    // . 0x0048c788..0x0048c7c0: the offers of the base the player is in
    // (the field), counted by walking Head / Next.
    TMirrorPropertyBucket<MissionVendorProperties>* const Bucket =
        GetClientLocationBaseMissionOffers(GetClientLocationCurrentBase());
    int Count = 0;
    MetaNode<MissionVendorProperties>* Node = nullptr;
    while (Bucket->List.traverse(Node, true))
        ++Count;

    // new[] (count prefix, MissionVendorProperties' ctor).
    Offers = new MissionVendorProperties[Count];
    OfferCount = Count;

    // element i is assigned from the list's i-th node (baseline walks
    // from the head for each i; one walk visits the same nodes in the same order).
    Node = nullptr;
    for (int Index = 0; Index < Count; ++Index)
    {
        Bucket->List.traverse(Node, true);
        Offers[Index] = *Node->Data; // ??4MissionVendorProperties@@
    }

    // the rows from the top, offer 0 selected, the scroll range.
    ScrollOffset = 0;
    Field_0x374 = 0;
    Selected = 0;
    SummaryScroll->SetProperty(0x37, 0, static_cast<uintptr_t>(OfferCount - 1));
}

void NnMissionVendor::RefreshRows()
{
    // .
    wchar_t Buffer[0x400];
    int Row = 0;
    for (int Index = ScrollOffset; Index < OfferCount; ++Index)
    {
        MissionVendorProperties const& Offer = Offers[Index];
        WinControl* const Controls[6] = {SummaryIcons[Row], IconFrames[Row], SummaryBoxes[Row],
                                         SummaryDests[Row], SummaryTitles[Row], SummaryRewards[Row]};

        // the row's six controls, property 2 then property 1, with 0.
        for (int Slot = 0; Slot < 6; ++Slot)
            Controls[Slot]->SetProperty(2, 0, 0);
        for (int Slot = 0; Slot < 6; ++Slot)
            Controls[Slot]->SetProperty(1, 0, 0);

        // the icon mesh and its tooltip; an unknown class keeps path 0
        // and ids 0 (unsigned compare, JAE).
        char const* Path = IconPaths[0];
        int Ids = 0;
        if (Offer.MissionClass < IconPathCount)
        {
            Path = IconPaths[Offer.MissionClass];
            Ids = G_MissionVendorIconIds[Offer.MissionClass];
        }
        SummaryIcons[Row]->SetProperty(4, PointerArg(Path), 0);
        IconFrames[Row]->SetProperty(3, static_cast<uintptr_t>(Ids), 0);
        SummaryBoxes[Row]->SetProperty(3, static_cast<uintptr_t>(Ids), 0);

        // the offer's +0x14 and +0x2c texts.
        FmtStrSprintf(&Offer.Title, Buffer, 0x400);
        SummaryDests[Row]->SetProperty(0x1d, PointerArg(Buffer), 0);
        FmtStrSprintf(&Offer.Description, Buffer, 0x400);
        SummaryTitles[Row]->SetProperty(0x1d, PointerArg(Buffer), 0);


        FormatReward(Offer, Buffer);
        SummaryRewards[Row]->SetProperty(0x1d, PointerArg(Buffer), 0);

        // the selected offer's frame and box lit (0xc with 1).
        uintptr_t const bSelected = (Index == Selected) ? 1u : 0u;
        IconFrames[Row]->SetProperty(0xc, bSelected, 0);
        SummaryBoxes[Row]->SetProperty(0xc, bSelected, 0);


        ++Row;
        if (Row >= 5)
            break;
    }

    // every unused row, property 2 then property 1 with 1, the default
    // icon mesh, the four text-bearing controls cleared (0x1e with 0).
    for (; Row < 5; ++Row)
    {
        WinControl* const Controls[6] = {SummaryIcons[Row], IconFrames[Row], SummaryBoxes[Row],
                                         SummaryDests[Row], SummaryTitles[Row], SummaryRewards[Row]};
        for (int Slot = 0; Slot < 6; ++Slot)
            Controls[Slot]->SetProperty(2, 1, 0);
        for (int Slot = 0; Slot < 6; ++Slot)
            Controls[Slot]->SetProperty(1, 1, 0);
        SummaryIcons[Row]->SetProperty(4, PointerArg(IconPaths[0]), 0);
        SummaryBoxes[Row]->SetProperty(0x1e, 0, 0);
        SummaryDests[Row]->SetProperty(0x1e, 0, 0);
        SummaryTitles[Row]->SetProperty(0x1e, 0, 0);
        SummaryRewards[Row]->SetProperty(0x1e, 0, 0);
    }
}

void NnMissionVendor::ShowSelected()
{
    // .
    wchar_t Buffer[0x400];
    if (Selected == -1)
    {
        // the reject line's ids, the three lines cleared, the status
        // text in the full-text pane. The video is left as it is.
        MissionReject->SetProperty(0x1e, 0x72f, 0);
        MissionDest->SetProperty(0x1e, 0, 0);
        MissionTitle->SetProperty(0x1e, 0, 0);
        MissionReward->SetProperty(0x1e, 0, 0);
        FmtStrSprintf(&StatusText, Buffer, 0x400);
        SetFullText(FullText, Buffer);
        return;
    }

    // . Selected is not range-checked (a click below the last offer
    // selects past the array, as in baseline).
    MissionVendorProperties const& Offer = Offers[Selected];
    FmtStrSprintf(&Offer.Title, Buffer, 0x400);
    MissionDest->SetProperty(0x1d, PointerArg(Buffer), 0);
    FmtStrSprintf(&Offer.Description, Buffer, 0x400);
    MissionTitle->SetProperty(0x1d, PointerArg(Buffer), 0);
    FormatReward(Offer, Buffer);
    MissionReward->SetProperty(0x1d, PointerArg(Buffer), 0);
    MissionReject->SetProperty(0x1e, 0, 0);
    FmtStrSprintf(&Offer.Briefing, Buffer, 0x400);
    SetFullText(FullText, Buffer);
    UpdateVideo();
}

void NnMissionVendor::UpdateVideo()
{
    // . 0x0048d343: without a selection the icon is hidden.
    if (Selected < 0)
    {
        MissionIcon->SetProperty(1, 1, 0);
        return;
    }

    // the offer's texture name (Common's get_icon_texture), the previous
    // swap undone, the name kept in VideoTexture. baseline runs strlen on the result unchecked.
    MissionVendorProperties const& Offer = Offers[Selected];
    char const* const Texture = Offer.get_icon_texture();
    ResetVideo();
    VideoTexture.assign(Texture, strlen(Texture));


    if (!VideoMaterial.Init(kVideoMaterialName))
        FDUMP(0x100001, kNoMaterialInVendor, kMissionVendorCpp, 0x3af, kVideoMaterialName);
    VideoMaterial.Swap(VideoTexture.c_str(), kVideoTextureLibrary);

    // the icon mesh, its tooltip, shown.
    char const* Path = IconPaths[0];
    int Ids = 0;
    if (Offer.MissionClass < IconPathCount)
    {
        Path = IconPaths[Offer.MissionClass];
        Ids = G_MissionVendorIconIds[Offer.MissionClass];
    }
    MissionIcon->SetProperty(4, PointerArg(Path), 0);
    MissionIcon->SetProperty(3, static_cast<uintptr_t>(Ids), 0);
    MissionIcon->SetProperty(1, 0, 0);
}

void NnMissionVendor::Draw()
{
    // .
    if (bNoMissions == 0)
    {
        // every child at 0x64; the hovered row's frame or box 0x65, the
        // selected row's 0x66 (gated on Clicked, indexed by Selected; the second test wins). The
        // rows are Hovered / Selected minus the scroll offset and are not range-checked.
        int const SelectedRow = Selected - ScrollOffset;
        int const HoveredRow = Hovered - ScrollOffset;
        size_t const FramesOffset = offsetof(NnMissionVendor, IconFrames);
        size_t const BoxesOffset = offsetof(NnMissionVendor, SummaryBoxes);
        for (Win3D* Child = ChildHead; Child != nullptr; Child = Child->NextSibling)
        {
            Child->DrawLayer = 0x64;
            if (Hovered >= 0)
            {
                uintptr_t const Control = PointerArg(Child->AsWinControl()); // slot 2
                if (Control == RowDword(FramesOffset, HoveredRow) ||
                    Control == RowDword(BoxesOffset, HoveredRow))
                    Child->DrawLayer = 0x65;
            }
            if (Clicked >= 0)
            {
                uintptr_t const Control = PointerArg(Child->AsWinControl());
                if (Control == RowDword(FramesOffset, SelectedRow) ||
                    Control == RowDword(BoxesOffset, SelectedRow))
                    Child->DrawLayer = 0x66;
            }
        }


        WireControlsJoinColumn(IconFrames, 5);
        WireControlsJoinColumn(SummaryBoxes, 5);
        Background->DrawLayer = 0x63;
        FullText->DrawLayer = 0x67;
    }
    WinControl::Draw(); // (tail jump)
}

void NnMissionVendor::Update()
{
    // the frame time is fetched and dropped (FSTP ST0), Win3D's Update, then a
    // close OnAcceptResult asked for.
    (void)GetClientFrameTimeCapped();
    Win3D::Update();
    if (bCloseRequested != 0)
    {
        bCloseRequested = 0;
        NavBarCloseService(false); // (0)
        DeleteSelf();
    }
}

intptr_t NnMissionVendor::SetProperty(int Msg, uintptr_t A, uintptr_t B)
{
    // (RET 0xc): byte table 0x0048c6f0 over Msg - 0x26 (0..0x28) into the five
    // cases at 0x0048c6dc. Every path returns 0.
    switch (Msg)
    {
    case 0x4e:
    {
        // closed from outside -- no callback, no NavBar. (baseline tests
        // `this` first.)
        DeleteSelf();
        return 0;
    }
    case 0x3e:
    {
        // the scroll bar moved.
        if (A == PointerArg(SummaryScroll) && static_cast<int>(B) != ScrollOffset)
        {
            ScrollOffset = static_cast<int>(B);
            RefreshRows();
        }
        return 0;
    }
    case 0x2c:
    case 0x2d:
    {
        // the hovered row, written (0x2c) or cleared (0x2d) on every
        // pass until the control is found; 0x2d compares against Row - ScrollOffset as baseline
        // does (0x0048c515..0x0048c51f). Then the row's other wire button gets the same message.
        int Row = 0;
        for (; Row < 5; ++Row)
        {
            if (Msg == 0x2c)
                Hovered = ScrollOffset + Row;
            else if (Hovered == Row - ScrollOffset)
                Hovered = -1;
            if (A == PointerArg(IconFrames[Row]) || A == PointerArg(SummaryBoxes[Row]))
                break;
        }
        if (Row == 5)
            return 0;
        if (A != PointerArg(IconFrames[Row]))
            IconFrames[Row]->SetProperty(Msg, 0, 0);
        if (A != PointerArg(SummaryBoxes[Row]))
            SummaryBoxes[Row]->SetProperty(Msg, 0, 0);
        return 0;
    }
    case 0x26:
    {
        if (A == PointerArg(CloseButton))
        {

            if (ResultCallback != nullptr)
                ResultCallback(ResultContext, 0);
            NavBarCloseService(false);
            DeleteSelf();
            return 0;
        }
        if (A == PointerArg(AcceptButton))
        {
            // accept the selected offer -- the callback with 1, then
            // IServer slot 43 (+0xac) with the offer's +0x08 and +0x0c. The window stays open
            // until OnAcceptResult.
            if (Selected < 0)
                return 0;
            if (ResultCallback != nullptr)
                ResultCallback(ResultContext, 1);
            MissionVendorProperties const& Offer = Offers[Selected];
            G_Server->MissionResponse(Offer.TargetId, Offer.SystemId, true, GetPlayerConnectionId());
            return 0;
        }

        // a row clicked. No match leaves Clicked at -1 and selects
        // ScrollOffset - 1, as baseline does.
        Clicked = -1;
        for (int Row = 0; Row < 5; ++Row)
        {
            if (A == PointerArg(IconFrames[Row]) || A == PointerArg(SummaryBoxes[Row]))
            {
                Clicked = Row;
                break;
            }
        }
        Selected = ScrollOffset + Clicked;
        RefreshRows();
        ShowSelected();
        if (Selected != -1)
            RevealSelectedOffer();
        return 0;
    }
    default:
        return 0;
    }
}

bool NnMissionVendor::OnEvent(unsigned MsgId)
{
    // (RET 4), on the +0x32c face: USER_CANCEL (0x10) closes the window like the
    // close button. (baseline tests the adjusted `this` for null.)
    if (MsgId != 0x10)
        return false;
    if (ResultCallback != nullptr)
        ResultCallback(ResultContext, 0);
    NavBarCloseService(false);
    DeleteSelf();
    return true;
}

void NnMissionVendor::CloseWindow()
{
    // . The NavBar goes first; baseline then tests `this` (its caller already has).
    NavBarCloseService(false);
    DeleteSelf();
}

void NnMissionVendor::OnAcceptResult(MissionVendorAcceptance const& Result)
{
    // the open NN_MissionVendor (the routine by class name).
    NnMissionVendor* const Window =
        static_cast<NnMissionVendor*>(Win3DFindByClassName("NN_MissionVendor"));
    if (Window == nullptr)
        return;

    // accepted (byte +0x08 == 1) -- close at the next Update.
    if (Result.bAccepted)
    {
        Window->bCloseRequested = 1;
        return;
    }

    // refused -- no selection, the reason (+0x0c) as the status text.
    Window->Selected = -1;
    Window->StatusText.destroy();
    Window->StatusText = Result.Reason;
    Window->RefreshRows();
    Window->ShowSelected();
    if (Window->Selected != -1)
        Window->RevealSelectedOffer();
}
