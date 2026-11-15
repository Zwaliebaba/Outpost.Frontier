// Author: Giperion.

#include "nn_player_info.h"

#include "character_select_equip.h" // GetPlayerSpawnSystemIdbaseline (the routine, the player's
                                    // vibe the field), GetPlayerVisitFlagsbaseline
                                    // (the routine), SetPlayerVisitFlagsbaseline (the routine)
#include "game_options.h"           // GetGameOption (the routine)
#include "globals.h"                // GetClientFrameTimeCapped (the routine)
#include "hud_target.h"             // HudReputationBand (the routine)
#include "ids_rdl.h"                // LoadIdsIntoRdl (the routine), RdlAppendNode
#include "ids_strings.h"            // GetIdsStringWWithRawFallback (the routine)
#include "login_mgr.h"              // G_Server (the field)
#include "misc_util.h"              // FormatGroupedNumber (the routine)
#include "nn_dealer.h"              // NnDealer::ApplyPlayerStatsFeeling (the routine)
#include "nn_modal.h"               // NnMainButtonOnModalOpen (the routine)
#include "nn_text_scratch.h"        // G_NnTextScratchB (the field), G_NnTextScratchCapacity
#include "nn_ui_pulse.h"            // G_HudChromeColorRgba (the field),
                                    // NnReputationColorArgb (the routine),
                                    // NnUiPulseBlendFromArgb (the routine)
#include "player_spawn.h"           // GetPlayerConnectionId (the field)
#include "rich_text_win.h"
#include "star_sphere.h"            // Win3DBindModelview (the routine)
#include "ui_scroll.h"
#include "win3d.h"                  // Win3DReparent (the routine), Win3DDeleteTree
                                    // (the routine), Win3DGetCamera (the field)
#include "win3d_immediate.h"        // Win3DBegin / Win3DVertex3f / Win3DEnd / ...
#include "wire_controls.h"          // WireControlsJoinColumn (the routine)

#include "NeuronRuntime.h"
#include "render/IRenderPipeline.h"
#include "render/rendcomp.h"

#include "common_archetype.h"       // Archetype::GetShip
#include "common_reputation.h"      // Reputation::get_id / get_name / get_info_card /
                                    // IsStoryFaction, Reputation::Vibe::GetAffiliation
#include "common_server.h"          // IServer::RequestPlayerStats
#include "common_misc.h"            // SinglePlayer
#include "common_text.h"            // RenderDisplayList, StyleNode
#include "common_tstring.h"         // TString

#include <stdio.h>
#include <string.h>
#include <wchar.h>

// WINFACTORY<NN_PlayerInfo> static (the branch -> the routine; Create
// ; atexit the branch -> the routine).
NnPlayerInfoFactory G_NnPlayerInfoFactory("NN_PlayerInfo");


NnPlayerInfo* G_NnPlayerInfo = nullptr;

// / the field (-0.6f / 0.6f, static inits the branch / the branch) are this
// TU's copies of a header float constant; nothing in the PE reads them (raw scan: each address
// occurs once, in its initializer's store), so the port has no counterpart.

namespace
{
    char const kInfoWindowMesh[] = "Interface\\NeuroNet\\Inventory\\CHAR_infowindow.cmp";
    char const kCloseButtonMesh[] = "Interface\\NeuroNet\\Inventory\\x.3db";
    char const kUnfriendlyBarMesh[] =
        "Interface\\NeuroNet\\Inventory\\PSTAT_grandienthate.3db";
    char const kFriendlyBarMesh[] =
        "Interface\\NeuroNet\\Inventory\\PSTAT_grandientlove.3db";

    wchar_t const kIntFormat[] = L"%d";
    wchar_t const kTimeFormat[] = L"%.02d:%.02d:%.02d";

    // Create's heading table (0x004a7e5a..0x004a7efe, a stack array): the ids of each
    // statistic, 1 for a gap of 0.015, 0 ends it.
    unsigned const kHeadingIds[16] = {0x61d, 0x61e, 0x61f, 1,     0x64b, 0x641, 0x646, 0x647,
                                      0x648, 0x649, 1,     0x64a, 0x64c, 0x64d, 0x64e, 0};

    // Not a baseline function: the "%d" line every statistic of the routine spells out -- swprintf
    // into the second scratch buffer, then the control's SetProperty(0x1d, text, 0).
    void SetScratchText(WinControl* Control)
    {
        Control->SetProperty(0x1d, reinterpret_cast<uintptr_t>(G_NnTextScratchB), 0);
    }

    void SetIntText(WinControl* Control, int Value)
    {
        _swprintf(G_NnTextScratchB, kIntFormat, Value);
        SetScratchText(Control);
    }

    // The player's affiliation as the routine (0x004a8758..0x004a8785) and the Draw
    // (0x004a821f..0x004a824c) read it: -1 in single player, else Vibe::GetAffiliation of the
    // player's vibe (its result is not tested; Out stays -1 when it writes nothing).
    unsigned PlayerAffiliation()
    {
        unsigned Affiliation = 0xffffffffu;
        if (!SinglePlayer())
        {
            int const PlayerVibe = GetPlayerSpawnSystemIdbaseline();
            Reputation::Vibe::GetAffiliation(PlayerVibe, Affiliation, false);
        }
        return Affiliation;
    }

    // The quad block of the Draw (0x004a83cc..0x004a8519 and 0x004a852d..0x004a8687): the
    // modelview from Bar's orientation (Win3D +0x3c) and its offset plus position (+0xa8 +
    // +0x60), texturing off, blending on with (2, 4), colour (0, 0, 0, 0x0a), then a quad at
    // z = 0.001 (0x3a83126f) through the four corners in the order the caller gives.
    void BindBarFrame(WinControl const* Bar)
    {
        float Transform[12];
        for (int Index = 0; Index < 9; ++Index)
            Transform[Index] = Bar->Orientation[Index]; // REP MOVSD, 9 dwords
        Transform[9] = Bar->Offset[0] + Bar->Position[0];
        Transform[10] = Bar->Offset[1] + Bar->Position[1];
        Transform[11] = Bar->Offset[2] + Bar->Position[2];
        Win3DBindModelview(nullptr, Transform); // (0, &Transform)
        Win3DDisable(kWin3DTexture2D);          // (0xde1)
        Win3DEnable(kWin3DBlend);               // (0xbe2)
        Win3DSetBlendFunc(2, 4);
        Win3DColor4ub(0, 0, 0, 0x0a);
        Win3DBegin(kWin3DQuads);                // (8)
    }
}

Win3D* NnPlayerInfoFactory::Create(Win3D* Parent, void* Payload)
{
    // operator_new(0x428), the ctor (inline), +0x10 = the factory's name, slot 9;
    // on failure slot 0 with 1 and null.
    NnPlayerInfo* const Window =
        new NnPlayerInfo(static_cast<unsigned char>(reinterpret_cast<uintptr_t>(Payload)));
    Window->ClassName = ClassName;
    if (!Window->Create(Parent, Payload))
    {
        delete Window;
        return nullptr;
    }
    return Window;
}

NnPlayerInfo::NnPlayerInfo(unsigned char PayloadByte)
    : CloseButton(nullptr)
    , PlayerStatsTitle(nullptr)
    , Scroll(nullptr)
    , FactionInfoCard(nullptr)
    , CloseInfocardButton(nullptr)
    , CloseInfocardText(nullptr)
    , Field_0x40c(PayloadByte)
    , FactionCount(0)
    , FirstRow(0)
    , StatsRequestTimer(0.0f)
{
    // 0x004a9143..0x004a91d6. baseline leaves the control pointers, +0x41c and
    // +0x424 unwritten until Create; the port zeroes them.
    for (int Row = 0; Row < 6; ++Row)
    {
        FactionFrames[Row] = nullptr;
        FactionNames[Row] = nullptr;
        UnfriendlyBars[Row] = nullptr;
        FriendlyBars[Row] = nullptr;
    }
    for (int Stat = 0; Stat < 13; ++Stat)
    {
        StatValues[Stat] = nullptr;
        StatHeadings[Stat] = nullptr;
    }

    // the "he_p_grp" group (a TString<16> on the stack) gets visit flags
    // 0x41, told to the server.
    TString<16> GroupName;
    GroupName = "he_p_grp";
    SetPlayerVisitFlagsbaseline(Reputation::get_id(GroupName), 0x41, true);

    G_NnPlayerInfo = this;
}

NnPlayerInfo::~NnPlayerInfo()
{
    // the field = 0, then the vector (member dtor) and WinControl's dtor.
    G_NnPlayerInfo = nullptr;
}

bool NnPlayerInfo::Create(Win3D* NewParent, void* Data)
{
    // (RET 8). 0x004a78cc..0x004a78e8: the window mesh (slot 38) and the hit box.
    SetMesh(kInfoWindowMesh, 0);
    WinControlHalfExtentsFromMesh(HalfExtents, &MeshHolder);


    {
        float const Position[3] = {0.405f, 0.239f, 0.0f}; // , 0x3e74bc6a
        float const Half[3] = {0.018f, 0.018f, 0.0f};
        CloseButton = CreateButtonControl("PlayerInfoCloseButton", kCloseButtonMesh, nullptr,
                                          "ui_select_item", nullptr, Position, nullptr, Half, 0, 0,
                                          0, 0, 0x409, 0, 0);
    }
    // ids 0x3a3 "PLAYER STATUS"
    {
        float const Position[3] = {-0.418f, 0.257f, 0.0f}; // , 0x3e839581
        CreateTextControl("PlayerInfoMainTitle", "PageHeader", Position, nullptr, 0.839f, 0.03f,
                          1.65f, 0x3a3, 1, 0, 1);
    }
    // ids 0x632 "REPUTATION"
    {
        float const Position[3] = {-0.405f, 0.21f, 0.0f}; // , 0x3e570a3d
        CreateTextControl("PlayerInfoTitle", "SubHeader", Position, nullptr, 0.35f, 0.03f, 1.65f,
                          0x632, 1, 0, 1);
    }
    // ids 0x640 "STATS"
    {
        float const Position[3] = {0.009f, 0.21f, 0.0f};
        PlayerStatsTitle = CreateTextControl("PlayerStatsTitle", "SubHeader", Position, nullptr,
                                             0.355f, 0.03f, 1.65f, 0x640, 1, 0, 1);
    }
    // the infocard, invisible (Flags &= 0xfd).
    {
        float const Position[3] = {0.01f, 0.172f, 0.0f}; // , 0x3e3020c5
        FactionInfoCard = CreateRichTextWin("FactionInfoCard", 0.384f, 0.34f, Position, true);
        FactionInfoCard->Flags = static_cast<unsigned char>(FactionInfoCard->Flags & 0xfd);
    }
    // ids 0x32b "Close the Infocard."
    {
        float const Position[3] = {0.2f, -0.21f, 0.0f};   // , 0xbe570a3d
        float const Half[3] = {0.133f, 0.026f, 0.0f};     // , 0x3cd4fdf4
        CloseInfocardButton = CreateWireButtonControl("NavMapCloseInfocardButton",
                                                      "ui_select_item", Position, Half, 0, 0,
                                                      0x32b, 0, 0);
    }
    // ids 0x3a1 "CLOSE"; both hidden (Flags &= 0xfc).
    {
        float const Position[3] = {0.07f, -0.19f, 0.0f}; // , 0xbe428f5c
        CloseInfocardText = CreateTextControl("NavMapCloseInfocardText", "ButtonText", Position,
                                              nullptr, 0.262f, 0.03f, 1.9f, 0x3a1, 1, 0, 1);
    }
    CloseInfocardButton->Flags = static_cast<unsigned char>(CloseInfocardButton->Flags & 0xfc);
    CloseInfocardText->Flags = static_cast<unsigned char>(CloseInfocardText->Flags & 0xfc);

    // six faction rows.
    char ControlName[0x80]; // [ESP+0xd0]
    for (int Row = 0; Row < 6; ++Row)
    {
        // FILD Row, FMUL 0.068f (the field), FSUBR -0.01f (the field): the row's base Y,
        // kept extended for the frame and stored as a float for the rest.
        double const RowY = static_cast<double>(-0.01f) -
                            static_cast<double>(Row) * static_cast<double>(0.068f);
        float const RowYf = static_cast<float>(RowY); // FST [ESP+0x30]

        sprintf(ControlName,"PlayerInfoFactionFrame%d", Row);
        {
            float const Position[3] = {-0.231f,
                                       static_cast<float>(RowY + static_cast<double>(0.149f)),
                                       0.0f};
            float const Half[3] = {0.173f, 0.034f, 0.0f}; // , 0x3d0b4396
            FactionFrames[Row] = CreateWireButtonControl(ControlName,"ui_item_select", Position, Half,
                                                         0, 0, 0, 0, 0);
        }

        sprintf(ControlName,"PlayerInfoFactionName%d", Row);
        {
            float const Position[3] = {
                NnFloatBits(0xbe666667), // -0.225 plus one ulp
                static_cast<float>(static_cast<double>(RowYf) + static_cast<double>(0.16f)),
                0.0f};
            FactionNames[Row] = CreateTextControl(ControlName,"ItemName", Position, nullptr, 0.358f,
                                                  0.04f, 1.5f, 0, 1, 0, 0);
        }

        // the bars' Y, RowYf + 0.134f (0x005d4918), shared by both.
        float const BarY =
            static_cast<float>(static_cast<double>(RowYf) + static_cast<double>(0.134f));
        float const Half[3] = {0.061f, 0.014f, 0.0f}; // , 0x3c656042
        float const Zero[3] = {0.0f, 0.0f, 0.0f};

        sprintf(ControlName,"PlayerInfoUnfriendlyBar%d", Row);
        {
            float const Position[3] = {-0.292f, BarY, 0.0f};
            UnfriendlyBars[Row] = CreateStaticControl(ControlName,kUnfriendlyBarMesh, Position, nullptr,
                                                      Half, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, Zero);
        }
        // the HUD chrome colour with a third of its alpha (the byte
        // divided by 3, IMUL 0x55555556), as both of property 0x18's arguments.
        unsigned char Colour[4];
        memcpy(Colour, G_HudChromeColorRgba, sizeof(Colour));
        Colour[3] = static_cast<unsigned char>(Colour[3] / 3);
        UnfriendlyBars[Row]->SetProperty(0x18, reinterpret_cast<uintptr_t>(Colour),
                                         reinterpret_cast<uintptr_t>(Colour));

        sprintf(ControlName,"PlayerInfofriendlyBar%d", Row);
        {
            float const Position[3] = {NnFloatBits(0xbe2e7d57), BarY, 0.0f}; // -0.1704 + 1 ulp
            FriendlyBars[Row] = CreateStaticControl(ControlName,kFriendlyBarMesh, Position, nullptr,
                                                    Half, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, Zero);
        }
        FriendlyBars[Row]->SetProperty(0x18, reinterpret_cast<uintptr_t>(Colour),
                                       reinterpret_cast<uintptr_t>(Colour));
    }


    {
        float const Position[3] = {-0.045f, 0.176f, 0.0f}; // , 0x3e343958
        Scroll = CreateUIScroll("PlayerInfoScroll", Position, 0.019f, 0.412f, 0, 5, 6, 0);
    }

    // the heading / value pairs, top down from 0.19; a 1 in the table
    // is a 0.015 gap. Outside single player the worth and next-level pairs (table entries 1 and
    // 2) are hidden and take no room.
    float Y = 0.0f;          // [ESP+0x10]
    int Pair = 0;            // [ESP+0x24]
    unsigned TableIndex = 0; // [ESP+0x20]
    unsigned Ids = kHeadingIds[0];
    while (Ids != 0)
    {
        if (Ids == 1)
        {
            Y = static_cast<float>(static_cast<double>(Y) + static_cast<double>(0.015f));
        }
        else
        {
            // FLD 0.19f (the field), FSUB Y, FSUB 0.03f (the field), stored as a float.
            float const PairY = static_cast<float>(
                static_cast<double>(0.19f) - static_cast<double>(Y) - static_cast<double>(0.03f));

            sprintf(ControlName,"PlayerInfoHeading%d", Pair);
            float const HeadingPosition[3] = {0.19f, PairY, 0.0f};
            WinControl* const Heading = CreateTextControl(ControlName,"ItemName", HeadingPosition,
                                                          nullptr, 0.358f, 0.04f, 1.5f, Ids, 0,
                                                          0, 0);
            StatHeadings[Pair] = Heading;

            sprintf(ControlName,"PlayerInfoStat%d", Pair);
            float const StatPosition[3] = {0.38f, PairY, 0.0f};
            StatValues[Pair] = CreateTextControl(ControlName,"ItemName", StatPosition, nullptr, 0.188f,
                                                 0.04f, 1.5f, 0, 0, 0, 0);

            if (!SinglePlayer() && (TableIndex == 1 || TableIndex == 2))
            {
                Heading->Flags = static_cast<unsigned char>(Heading->Flags & 0xfc);
                StatValues[Pair]->Flags = static_cast<unsigned char>(StatValues[Pair]->Flags & 0xfc);
            }
            else
            {
                Y = static_cast<float>(static_cast<double>(Y) + static_cast<double>(0.03f));
            }
            ++Pair;
        }
        ++TableIndex;
        Ids = kHeadingIds[TableIndex];
    }


    Scroll->SetProperty(0x37, 0, static_cast<uintptr_t>(-1));
    FactionCount = 0;
    SetOffset(0.0f, 0.0f, 0.0f); // slot 31
    SetPosition(Position);       // slot 5
    StatsRequestTimer = 1.0f;
    return Win3D::Create(NewParent, Data);
}

intptr_t NnPlayerInfo::SetProperty(int Msg, uintptr_t A, uintptr_t B)
{
    // (RET 0xc).
    if (Msg == 0x3e)
    {
        // the scroll moved.
        if (A != reinterpret_cast<uintptr_t>(Scroll))
            return 0;
        FirstRow = static_cast<int>(B);
        return 1;
    }
    if (Msg != 0x26)
        return 0;

    if (A == reinterpret_cast<uintptr_t>(CloseButton))
    {
        NnMainButtonOnModalOpen();
        return 1;
    }

    if (A == reinterpret_cast<uintptr_t>(CloseInfocardButton))
    {
        // the card invisible, its close button and text hidden, every
        // statistic shown again (outside single player not the worth and next-level pairs), the
        // title back to ids 0x640.
        FactionInfoCard->Flags = static_cast<unsigned char>(FactionInfoCard->Flags & 0xfd);
        CloseInfocardButton->Flags = static_cast<unsigned char>(CloseInfocardButton->Flags & 0xfc);
        CloseInfocardText->Flags = static_cast<unsigned char>(CloseInfocardText->Flags & 0xfc);
        for (int Stat = 0; Stat < 13; ++Stat)
        {
            if (SinglePlayer() || (Stat != 1 && Stat != 2))
            {
                StatValues[Stat]->Flags = static_cast<unsigned char>(StatValues[Stat]->Flags | 3);
                StatHeadings[Stat]->Flags =
                    static_cast<unsigned char>(StatHeadings[Stat]->Flags | 3);
            }
        }
        PlayerStatsTitle->SetProperty(0x1e, 0x640, 0);
        return 1;
    }

    // a faction frame.
    for (int Row = 0; Row < 6; ++Row)
    {
        if (A == reinterpret_cast<uintptr_t>(FactionFrames[Row]))
        {
            ShowFactionInfocard(FirstRow + Row);
            return 1;
        }
    }
    return 0;
}

bool NnPlayerInfo::Slot23(unsigned Msg, unsigned A, unsigned B)
{
    // (RET 0xc) -- the body NN_Help's slot 23 (nn_help.cpp) is folded with.
    if (Msg != 0xef02)
        return WinControl::Slot23(Msg, A, B);

    // out of the parent (no parent test), then every child deleted (the
    // next sibling read first), slot 10 and slot 0 with 1.
    Win3DReparent(this, nullptr);
    Win3D* Child = ChildHead;
    while (Child != nullptr)
    {
        Win3D* const Next = Child->NextSibling;
        Win3DDeleteTree(Child);
        Child = Next;
    }
    Destroy();
    delete this;
    return true;
}

void NnPlayerInfo::Draw()
{
    // (slot 13).
    if (GetGameOption(kGameOptHud) == 0) // (10)
        return;

    unsigned const Affiliation = PlayerAffiliation(); // [ESP+0x10]

    // the six rows from FirstRow; a row past the count hides its frame,
    // name and bars.
    for (int Row = 0; Row < 6; ++Row)
    {
        int const Index = FirstRow + Row;
        if (Index >= FactionCount)
        {
            FactionNames[Row]->Flags = static_cast<unsigned char>(FactionNames[Row]->Flags & 0xfc);
            FactionFrames[Row]->Flags =
                static_cast<unsigned char>(FactionFrames[Row]->Flags & 0xfc);
            UnfriendlyBars[Row]->Flags =
                static_cast<unsigned char>(UnfriendlyBars[Row]->Flags & 0xfc);
            FriendlyBars[Row]->Flags = static_cast<unsigned char>(FriendlyBars[Row]->Flags & 0xfc);
            continue;
        }
        FactionNames[Row]->Flags = static_cast<unsigned char>(FactionNames[Row]->Flags | 3);
        FactionFrames[Row]->Flags = static_cast<unsigned char>(FactionFrames[Row]->Flags | 3);
        UnfriendlyBars[Row]->Flags = static_cast<unsigned char>(UnfriendlyBars[Row]->Flags | 3);
        FriendlyBars[Row]->Flags = static_cast<unsigned char>(FriendlyBars[Row]->Flags | 3);

        FNnPlayerInfoFaction const& Faction = Factions.data()[Index];

        // the name, ids of Reputation::get_name.
        FactionNames[Row]->SetProperty(0x1e, Reputation::get_name(Faction.GroupId), 0);

        // its colour by attitude, pulsed for the player's own group.
        unsigned long Colour = 0; // [ESP+0x14]
        NnReputationColorArgb(&Colour, Faction.Attitude);
        if (Affiliation == Faction.GroupId)
        {
            unsigned long Pulsed = 0; // [ESP+0x10]
            NnUiPulseBlendFromArgb(&Pulsed, Colour);
            Colour = Pulsed;
        }
        FactionNames[Row]->SetProperty(0x33, reinterpret_cast<uintptr_t>(&Colour), 1);

        // the frame's caption, ids 0x634 "FRIENDLY" for band 4, 0x636
        // "HOSTILE" for band 0, otherwise 0x635 "NEUTRAL".
        int const Band = HudReputationBand(Faction.Attitude); // at 0x004a830a
        unsigned CaptionIds = 0x635;
        if (Band == 4)
            CaptionIds = 0x634;
        else if (Band == 0)
            CaptionIds = 0x636;
        FactionFrames[Row]->SetProperty(3, CaptionIds, 0);
    }


    WireControlsJoinColumn(FactionFrames, 6);
    WinControl::Draw();

    // each visible row's bar is darkened past its attitude.
    IRenderPipeline* const Pipe = NeuronRuntime::Pipeline;
    for (int Row = 0; Row < 6; ++Row)
    {
        int const Index = FirstRow + Row;
        if (Index >= FactionCount)
            continue;
        FNnPlayerInfoFaction const& Faction = Factions.data()[Index];

        // FCOMP 0.0f (the field), TEST AH,5, JP -- the unfriendly
        // bar only when the attitude is less than 0 (C0 alone); at or above 0, or unordered,
        // the friendly one.
        WinControl* Bar = nullptr;
        if (Faction.Attitude < 0.0f)
        {
            // X = HalfX - (-(2 HalfX * Attitude)); the quad from X to
            // the left end.
            Bar = UnfriendlyBars[Row];
            float const HalfX = Bar->HalfExtents[0];
            float const HalfY = Bar->HalfExtents[1];
            float const X = static_cast<float>(
                static_cast<double>(HalfX) +
                static_cast<double>(HalfX + HalfX) * static_cast<double>(Faction.Attitude));
            Pipe->SetRenderState(D3DRS_ZENABLE, 1);
            Pipe->SetRenderState(D3DRS_ZFUNC, 8);
            Pipe->SetRenderState(D3DRS_ZWRITEENABLE, 1);
            BindBarFrame(Bar);
            Win3DVertex3f(X, HalfY, 0.001f);
            Win3DVertex3f(X, -HalfY, 0.001f);
            Win3DVertex3f(-HalfX, -HalfY, 0.001f);
            Win3DVertex3f(-HalfX, HalfY, 0.001f);
            Win3DEnd();
        }
        else
        {
            // X = HalfX - 2 HalfX * Attitude; the quad from -X to the
            // right end.
            Bar = FriendlyBars[Row];
            float const HalfX = Bar->HalfExtents[0];
            float const HalfY = Bar->HalfExtents[1];
            float const X = static_cast<float>(
                static_cast<double>(HalfX) -
                static_cast<double>(HalfX + HalfX) * static_cast<double>(Faction.Attitude));
            Pipe->SetRenderState(D3DRS_ZENABLE, 1);
            Pipe->SetRenderState(D3DRS_ZFUNC, 8);
            Pipe->SetRenderState(D3DRS_ZWRITEENABLE, 1);
            BindBarFrame(Bar);
            Win3DVertex3f(-X, HalfY, 0.001f);
            Win3DVertex3f(-X, -HalfY, 0.001f);
            Win3DVertex3f(HalfX, -HalfY, 0.001f);
            Win3DVertex3f(HalfX, HalfY, 0.001f);
            Win3DEnd();
        }

        // the bar's mesh over the quad.
        Pipe->SetRenderState(D3DRS_ZFUNC, 4);
        NeuronRuntime::Renderer->RenderInstance(Win3DGetCamera(), Bar->MeshHolder.EngineInstance, 1.0f, 0,
                               nullptr, nullptr); // IRenderer2 +0x1c, &the field
    }

    // the timer gets the frame time (the sum is stored as a float, the
    // compare uses the extended sum); past 1.0 it loses 1.0 and the server is asked for the
    // stats (IServer slot 73 with a zero dword, 4 bytes).
    double const Timer =
        GetClientFrameTimeCapped() + static_cast<double>(StatsRequestTimer);
    StatsRequestTimer = static_cast<float>(Timer);
    if (Timer > 1.0) // TEST AH,0x41 / JNZ: skipped when not greater
    {
        StatsRequestTimer = static_cast<float>(Timer - 1.0);
        unsigned Request = 0; // [ESP+0x4]
        G_Server->RequestPlayerStats(GetPlayerConnectionId(),
                                     reinterpret_cast<unsigned char*>(&Request), 4);
    }
}

void NnPlayerInfo::SortFactions()
{
    // (thiscall).
    unsigned const Affiliation = PlayerAffiliation(); // [ESP+0xc]

    // passes over the FactionCount - 1 neighbour pairs (none when that
    // is <= 0) until one swaps nothing. Each attitude, 10.0f (the field) higher for the
    // affiliation, compared on the x87 stack: FCOMP, TEST AH,0x41, JNZ -- the pair swaps only
    // when the first is greater (not when equal, less or unordered).
    bool bSorted = false;
    while (!bSorted)
    {
        bSorted = true;
        for (int Pair = 0; Pair < FactionCount - 1; ++Pair)
        {
            FNnPlayerInfoFaction* const First = &Factions.data()[Pair];
            FNnPlayerInfoFaction* const Second = &Factions.data()[Pair + 1];
            double FirstKey = static_cast<double>(First->Attitude);
            double SecondKey = static_cast<double>(Second->Attitude);
            if (First->GroupId == Affiliation)
                FirstKey += static_cast<double>(10.0f);
            if (Second->GroupId == Affiliation)
                SecondKey += static_cast<double>(10.0f);
            if (FirstKey > SecondKey)
            {
                FNnPlayerInfoFaction const Saved = *First;
                *First = *Second;
                *Second = Saved;
                bSorted = false;
            }
        }
    }
}

void NnPlayerInfo::OnPlayerStats(FNnPlayerStatsBlock const* Stats,
                                 FNnPlayerStatsShipKills const* ShipKills,
                                 FPlayerStatsRepEntry const* Reputations)
{
    // (thiscall, RET 0xc). 0x004a8834..0x004a88ae: systems, bases, jump holes.
    SetIntText(StatValues[6], Stats->SystemsVisited);  // +0x3b0
    SetIntText(StatValues[7], Stats->BasesVisited);    // +0x3b4
    SetIntText(StatValues[8], Stats->JumpHolesFound);  // +0x3b8

    // the play time, ftol of the seconds, as hours:minutes:seconds.
    {
        int const TotalSeconds = static_cast<int>(Stats->TotalTimeSeconds);
        int const Seconds = TotalSeconds % 60;
        int const TotalMinutes = TotalSeconds / 60;
        int const Minutes = TotalMinutes % 60;
        int const Hours = TotalMinutes / 60;
        _swprintf(G_NnTextScratchB, kTimeFormat, Hours, Minutes, Seconds);
        SetScratchText(StatValues[5]); // +0x3ac
    }

    // kills by the ship's type bits (Archetype +0x10); a ship
    // archetype that does not resolve counts nowhere.
    int Freighters = 0;   // [ESP+0x18], type & 0x20000
    int Transports = 0;   // [ESP+0x10], type & 0x100000
    int Battleships = 0;  // [ESP+0x24], type & 0x280000
    int Fighters = 0;     // EBP, every other type
    for (int Entry = 0; Entry < Stats->ShipKillCount; ++Entry)
    {
        FNnPlayerStatsShipKills const& Kills = ShipKills[Entry];
        Archetype::Ship const* const ShipArch = Archetype::GetShip(Kills.ShipArchId);
        if (ShipArch == nullptr)
            continue;
        unsigned const Type = ShipArch->ObjectType;
        if ((Type & 0x20000u) != 0)
            Freighters += Kills.Count;
        else if ((Type & 0x100000u) != 0)
            Transports += Kills.Count;
        else if ((Type & 0x280000u) != 0)
            Battleships += Kills.Count;
        else
            Fighters += Kills.Count;
    }


    SetIntText(StatValues[9], Fighters);     // +0x3bc
    SetIntText(StatValues[10], Freighters);  // +0x3c0
    SetIntText(StatValues[11], Transports);  // +0x3c4
    SetIntText(StatValues[12], Battleships); // +0x3c8
    SetIntText(StatValues[4], Battleships + Transports + Freighters + Fighters); // +0x3a8

    // missions, level, worth (grouped digits).
    SetIntText(StatValues[3], Stats->TotalMissions); // +0x3a4
    SetIntText(StatValues[0], Stats->Level);         // +0x398
    FormatGroupedNumber(G_NnTextScratchB, Stats->Worth, -1);
    SetScratchText(StatValues[1]);                   // +0x39c

    // the next level -- ids 0x3e4 "Max Level" for 0x7fffffff, the
    // grouped number when positive, otherwise ids 0x620 "Mission".
    int const Requirement = Stats->NextLevelRequirement;
    if (Requirement == 0x7fffffff)
        GetIdsStringWWithRawFallback(0x3e4, G_NnTextScratchB, G_NnTextScratchCapacity);
    else if (Requirement > 0)
        FormatGroupedNumber(G_NnTextScratchB, Requirement, -1);
    else
        GetIdsStringWWithRawFallback(0x620, G_NnTextScratchB, G_NnTextScratchCapacity);
    SetScratchText(StatValues[2]); // +0x3a0

    // the faction list rebuilt -- each group with an entry in the visit
    // map (the routine with (0, 1)) whose flags have bit 0, and that is no story faction.
    Factions.clear(); // the tail copied onto the head, end = begin
    FactionCount = 0;
    for (int Entry = 0; Entry < Stats->ReputationCount; ++Entry)
    {
        FPlayerStatsRepEntry const& Rep = Reputations[Entry];
        unsigned char VisitFlags = 0; // [ESP+0x28]
        if (!GetPlayerVisitFlagsbaseline(Rep.GroupId, &VisitFlags, 0, 1))
            continue;
        if ((VisitFlags & 1) == 0)
            continue;
        if (Reputation::IsStoryFaction(Rep.GroupId))
            continue;
        FNnPlayerInfoFaction Faction;
        Faction.GroupId = Rep.GroupId;
        Faction.Attitude = Rep.Feeling;
        Factions.push_back(Faction); // the vector insert the routine / operator_new inline
        ++FactionCount;
    }


    SortFactions();
    Scroll->SetProperty(0x37, 0, static_cast<uintptr_t>(FactionCount - 1));
}

void NnPlayerInfo::ShowFactionInfocard(int Row)
{
    // (thiscall, RET 4). 0x004a8e9d..0x004a8eda: nothing without rows (a null or
    // empty vector) or when the group has no infocard.
    if (Factions.empty())
        return;
    // Row is not tested against the count (0x004a8ec1..0x004a8ec7).
    unsigned const GroupId = Factions.data()[Row].GroupId;
    unsigned const InfoCard = Reputation::get_info_card(GroupId);
    if (InfoCard == 0)
        return;

    // the title becomes the group's name.
    PlayerStatsTitle->SetProperty(0x1e, Reputation::get_name(GroupId), 0);

    // every statistic hidden, the card visible, its close button and
    // text shown.
    for (int Stat = 0; Stat < 13; ++Stat)
    {
        StatValues[Stat]->Flags = static_cast<unsigned char>(StatValues[Stat]->Flags & 0xfc);
        StatHeadings[Stat]->Flags = static_cast<unsigned char>(StatHeadings[Stat]->Flags & 0xfc);
    }
    FactionInfoCard->Flags = static_cast<unsigned char>(FactionInfoCard->Flags | 2);
    CloseInfocardButton->Flags = static_cast<unsigned char>(CloseInfocardButton->Flags | 3);
    CloseInfocardText->Flags = static_cast<unsigned char>(CloseInfocardText->Flags | 3);

    // a card of style 0x9ca4 and the infocard's ids, put into the text
    // (cleared first) and scrolled to the origin; the card destroyed.
    RenderDisplayList Card; // [ESP+0x1c]
    {
        StyleNode Style(0x9ca4);
        RdlAppendNode(Card, Style.Clone()); // vt+0x1c, the routine
    }
    LoadIdsIntoRdl(InfoCard, Card);
    FactionInfoCard->ClearContent();
    FactionInfoCard->AppendContent(Card);
    FactionInfoCard->SetScrollOrigin(0, 0);
}

void NnPlayerInfoOnPlayerStats(unsigned char const* Stats, void const* ShipKills,
                               void const* Reputations)
{
    // .
    FNnPlayerStatsBlock const* const Block = reinterpret_cast<FNnPlayerStatsBlock const*>(Stats);
    FPlayerStatsRepEntry const* const RepTable =
        static_cast<FPlayerStatsRepEntry const*>(Reputations);

    // an open NN_PlayerInfo gets all three through the routine.
    if (G_NnPlayerInfo != nullptr)
    {
        G_NnPlayerInfo->OnPlayerStats(Block,
                                      static_cast<FNnPlayerStatsShipKills const*>(ShipKills),
                                      RepTable);
        return;
    }
    // the routine([Stats+0x30], the reputation table -- the third
    // argument, [ESP+0xc]).
    NnDealer::ApplyPlayerStatsFeeling(Block->ReputationCount, RepTable);
}
