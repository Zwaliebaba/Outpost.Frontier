// Author: Giperion.

// NeuroNetNavMap -- the view switches and the pieces of the frame that follow the view: the
// map meshes' pan / zoom, the grid labels, the zoom on a click, the universe / system rebuilds
// and the side panels' slide. See nn_navmap.h. baseline Client\Interface\NeuroNet\NavMap.cpp.

#include "nn_navmap.h"
#include "character_select_equip.h" // GetPlayerStarSystemIdbaseline (the routine)
#include "NeuronRuntime.h"                  // NeuronRuntime::Engine
#include "engine/sceneruntime.h"         // IEngine
#include "menu_ui_init.h"           // PlayOrRewindUiSoundSlot (the routine)
#include "rich_text_win.h"          // RichTextWin : WinControl
#include "ui_scroll.h"              // UiScroll : WinControl
#include "win3d.h"                  // kWin3DVisible
#include "common_universe.h"
#include "x86math.h"                // FTransform

#include <string.h>

namespace
{
    // 1/255 as the float 0x3b808081.
    float const kNavMapInv255 = 0.003921568859368563f;

    // FILD of the zero-extended byte, FMUL the field, FSTP dword.
    float NavMapByteToUnit(unsigned char Value)
    {
        return static_cast<float>(static_cast<double>(Value) * kNavMapInv255);
    }

    // The alpha the routine gives a map mesh: ftol(Alpha * 255.0f) (the field), its low
    // byte, back to a float through the field.
    float NavMapAlphaByte(float Alpha)
    {
        int const Whole = static_cast<int>(static_cast<double>(Alpha) * 255.0);
        return NavMapByteToUnit(static_cast<unsigned char>(Whole));
    }

    // the zoomed map mesh's scale per unit of zoom (0x3dee23b8, 5/43).
    float const kNavMapZoomedMeshScale = 0.11627906560897827f;
    // .
    float const kNavMapFlatMeshScale = 1.5f;

    // 's constants: the field / the field / the field the grid's offset
    // from the pan, the field the label step, the field / the field the first label,
    // / the field and the field / the field the frame's edges, the two
    // fixed rows / columns -0.294 / -0.309, and the zoomed scale 8.6.
    float const kNavMapGridOffsetX = 0.027000000700354576f;
    float const kNavMapGridOffsetY = 0.011500000022351742f;
    float const kNavMapGridOffsetNegY = -0.011500000022351742f;
    float const kNavMapGridStep = 0.07000000029802322f;
    float const kNavMapGridFirstLetter = 0.27900001406669617f;
    float const kNavMapGridFirstNumber = 0.25200000405311584f;
    float const kNavMapGridLetterMax = 0.23000000417232513f;
    float const kNavMapGridLetterMin = -0.3100000023841858f;
    float const kNavMapGridNumberMax = 0.27399998903274536f;
    float const kNavMapGridNumberMin = -0.25699999928474426f;
    float const kNavMapGridLetterRow = -0.29399999976158142f;
    float const kNavMapGridNumberColumn = -0.30899998545646667f;
    float const kNavMapZoomedScale = 8.6000003814697266f;

    // the field (double) -- the zoomed-in distance per unit of MapExtentX.
    double const kNavMapZoomedDistance = 0.11627906976744186;
    // the zoom's duration.
    float const kNavMapZoomSeconds = 1.5f;

    // / the field -- the two bases Create adds its right-panel offsets to (the
    // same constants as nn_navmap.cpp's; the routine derives the panels' off-screen x from
    // them).
    float const kNavMapPanelBaseA = -0.434f;
    float const kNavMapPanelBaseB = 0.421f;

    // One control the routine slides: off-screen x (the panel base plus an offset, computed
    // in the FPU and stored as a float), on-screen x, and the y both share (z 0).
    struct FNavMapSlide
    {
        WinControl* Control;
        float OffX;
        float OnX;
        float Y;
    };

    // The FPU sum the routine stores as a panel control's off-screen x.
    float NavMapPanelX(float Base, double Offset)
    {
        return static_cast<float>(static_cast<double>(Base) + Offset);
    }
}

// ----------------------------------------------------------------------------------------
// The map meshes and the grid
// ----------------------------------------------------------------------------------------

void NeuroNetNavMap::SetMapMeshTransform(int View, float Alpha)
{
    // .
    IEngine* const Engine = NeuronRuntime::Engine;
    double const Zoom = static_cast<double>(ZoomScale);
    double const PanX = static_cast<double>(PanOffset[0]) * Zoom;
    double const PanY = static_cast<double>(PanOffset[1]) * Zoom;

    if (View == 2)
    {
        // the zoomed mesh (+0x378), placed at +0x828 plus the scaled pan,
        // its rotation's [0][0] / [1][1] = ZoomScale * 5/43 (the rest of the rotation kept).
        float const Scale = static_cast<float>(Zoom * static_cast<double>(kNavMapZoomedMeshScale));
        FTransform Transform;
        memcpy(&Transform, Engine->GetInstanceTransform(ZoomedMapMesh2.EngineInstance),
               sizeof(Transform));
        Transform.Rotation.D[0][0] = Scale;
        Transform.Rotation.D[1][1] = Scale;
        Transform.Translation.X = static_cast<float>(static_cast<double>(Field_0x828[0]) + PanX);
        Transform.Translation.Y = static_cast<float>(static_cast<double>(Field_0x828[1]) + PanY);
        Transform.Translation.Z = Field_0x828[2];
        Engine->SetInstanceMatrix(ZoomedMapMesh2.EngineInstance, &Transform);

        // white, the alpha through a byte.
        ZoomedMapMesh2.UserFloats[0] = 1.0f;
        ZoomedMapMesh2.UserFloats[1] = 1.0f;
        ZoomedMapMesh2.UserFloats[2] = 1.0f;
        ZoomedMapMesh2.UserFloats[4] = NavMapAlphaByte(Alpha);
        return;
    }

    // the flat mesh (+0x350) at MeshPosition plus the scaled pan, scale
    // ZoomScale * 1.5.
    float const Scale = static_cast<float>(Zoom * static_cast<double>(kNavMapFlatMeshScale));
    FTransform Transform;
    memcpy(&Transform, Engine->GetInstanceTransform(ZoomedMapMesh.EngineInstance),
           sizeof(Transform));
    Transform.Rotation.D[0][0] = Scale;
    Transform.Rotation.D[1][1] = Scale;
    Transform.Translation.X = static_cast<float>(static_cast<double>(MeshPosition[0]) + PanX);
    Transform.Translation.Y = static_cast<float>(static_cast<double>(MeshPosition[1]) + PanY);
    Transform.Translation.Z = MeshPosition[2];
    Engine->SetInstanceMatrix(ZoomedMapMesh.EngineInstance, &Transform);


    ZoomedMapMesh.UserFloats[0] = 1.0f;
    ZoomedMapMesh.UserFloats[1] = 1.0f;
    ZoomedMapMesh.UserFloats[2] = 1.0f;
    ZoomedMapMesh.UserFloats[4] = NavMapAlphaByte(Alpha);
}

void NeuroNetNavMap::PlaceGridLabels()
{
    // . A label's place is From + (To - From) * Ease, each end = (the grid's
    // offset + the label's step) * the end's scale.
    float ScaleFrom = 1.0f;
    float ScaleTo = 1.0f;
    float Ease = 0.0f;
    float FromX = 0.0f;
    float FromY = 0.0f;
    float ToX = 0.0f;
    float ToY = 0.0f;

    if (bTransition != 0)
    {
        Ease = TransitionEase;
        if (ViewMode == 2)
        {
            // / 0049fcee: zooming out of view 2.
            FromX = static_cast<float>(static_cast<double>(kNavMapGridOffsetX) -
                                       static_cast<double>(TransitionDelta[0]));
            ScaleTo = 1.0f;
            FromY = static_cast<float>(static_cast<double>(kNavMapGridOffsetNegY) -
                                       static_cast<double>(TransitionDelta[1]));
            ScaleFrom = kNavMapZoomedScale;
        }
        else
        {
            // zooming in.
            ScaleFrom = 1.0f;
            ScaleTo = kNavMapZoomedScale;
            ToX = static_cast<float>(static_cast<double>(TransitionDelta[0]) +
                                     static_cast<double>(kNavMapGridOffsetX));
            FromX = kNavMapGridOffsetX;
            FromY = kNavMapGridOffsetNegY;
            ToY = static_cast<float>(static_cast<double>(TransitionDelta[1]) -
                                     static_cast<double>(kNavMapGridOffsetY));
        }
    }
    else if (ViewMode == 2)
    {
        // zoomed in, following the pan.
        ScaleTo = kNavMapZoomedScale;
        FromX = static_cast<float>(static_cast<double>(PanOffset[0]) +
                                   static_cast<double>(kNavMapGridOffsetX));
        FromY = static_cast<float>(static_cast<double>(PanOffset[1]) -
                                   static_cast<double>(kNavMapGridOffsetY));
        ScaleFrom = kNavMapZoomedScale;
    }

    // the letters A..H along the bottom. TEST AH,0x41 + JE hides one right
    // of 0.23; TEST AH,5 + JNP hides one left of -0.31 (an unordered x stays shown).
    for (int Index = 0; Index < 8; ++Index)
    {
        double const Step = static_cast<double>(Index) * static_cast<double>(kNavMapGridStep);
        double const A = ((static_cast<double>(FromX) + Step) -
                          static_cast<double>(kNavMapGridFirstLetter)) *
                         static_cast<double>(ScaleFrom);
        double const B = ((Step + static_cast<double>(ToX)) -
                          static_cast<double>(kNavMapGridFirstLetter)) *
                         static_cast<double>(ScaleTo);
        float const X = static_cast<float>((B - A) * static_cast<double>(Ease) + A);
        WinControl* const Label = GridLetters[Index];
        if (X > kNavMapGridLetterMax || X < kNavMapGridLetterMin)
        {
            Label->SetProperty(1, 1, 0);
            continue;
        }
        Label->SetProperty(1, 0, 0);
        float const Pos[3] = {X, kNavMapGridLetterRow, 0.0f};
        Label->SetProperty(6, reinterpret_cast<uintptr_t>(Pos), 0);
    }

    // the numbers 1..8 down the left side.
    for (int Index = 0; Index < 8; ++Index)
    {
        double const Step = static_cast<double>(kNavMapGridFirstNumber) -
                            static_cast<double>(Index) * static_cast<double>(kNavMapGridStep);
        double const A = (static_cast<double>(FromY) + Step) * static_cast<double>(ScaleFrom);
        double const B = (Step + static_cast<double>(ToY)) * static_cast<double>(ScaleTo);
        float const Y = static_cast<float>((B - A) * static_cast<double>(Ease) + A);
        WinControl* const Label = GridNumbers[Index];
        if (Y > kNavMapGridNumberMax || Y < kNavMapGridNumberMin)
        {
            Label->SetProperty(1, 1, 0);
            continue;
        }
        Label->SetProperty(1, 0, 0);
        float const Pos[3] = {kNavMapGridNumberColumn, Y, 0.0f};
        Label->SetProperty(6, reinterpret_cast<uintptr_t>(Pos), 0);
    }
}

void NeuroNetNavMap::ZoomToClick()
{
    // .
    HideObjectTools();

    // the pan that puts the click at the centre, less the current pan.
    double const HalfX = static_cast<double>(MapSize[0]) * 0.5;
    double const HalfY = static_cast<double>(MapSize[1]) * 0.5;
    TransitionDelta[2] = 0.0f;
    TransitionDelta[0] = static_cast<float>(
        HalfX - static_cast<double>(ClickU) * static_cast<double>(MapSize[0]));
    TransitionDelta[1] = static_cast<float>(
        HalfY - (1.0 - static_cast<double>(ClickV)) * static_cast<double>(MapSize[1]));
    TransitionDelta[0] = static_cast<float>(static_cast<double>(TransitionDelta[0]) -
                                            static_cast<double>(PanOffset[0]));
    TransitionDelta[1] = static_cast<float>(static_cast<double>(TransitionDelta[1]) -
                                            static_cast<double>(PanOffset[1]));
    TransitionDelta[2] = static_cast<float>(static_cast<double>(TransitionDelta[2]) -
                                            static_cast<double>(PanOffset[2]));

    // a 1.5 s view-2 transition from the current pan; the distance goes
    // from MapExtentX / ZoomScale to MapExtentX * 5/43.
    TransitionTime = 0.0f;
    TransitionStart[0] = PanOffset[0];
    TransitionStart[1] = PanOffset[1];
    bTransition = 1;
    TransitionView = 2;
    TransitionDuration = kNavMapZoomSeconds;
    TransitionStart[2] = PanOffset[2];
    double const Distance = static_cast<double>(MapExtentX) / static_cast<double>(ZoomScale);
    TransitionDelta[2] = static_cast<float>(
        static_cast<double>(MapExtentX) * kNavMapZoomedDistance - Distance);
    ZoomDistance = static_cast<float>(Distance);

    PlayOrRewindUiSoundSlot(6); // (6)
}

// ----------------------------------------------------------------------------------------
// View rebuilds
// ----------------------------------------------------------------------------------------

void NeuroNetNavMap::EnterSystemView()
{
    // .
    if (G_NavMapFilterMode == 6)
        SetFilterMode(FilterModeBeforeInfoCard);
    ViewMode = 1;
    CachedObjectId = 0xffffffffu;

    // the jump links, their records freed (the routine / the routine).
    while (!JumpLinks.empty())
    {
        FNavMapJumpLink* const Link = JumpLinks.front();
        operator delete(Link);
        JumpLinks.pop_front();
    }

    // every item (the routine + operator delete on a non-null one).
    while (!Items.empty())
    {
        FNavMapItem* const Item = Items.front();
        if (Item != nullptr)
            delete Item;
        Items.pop_front();
    }


    for (int Index = 0; Index < 0x63; ++Index)
        WaypointItems[Index] = nullptr;
    for (int Index = 0; Index < 0x63; ++Index)
        BestPathItems[Index] = nullptr;

    BuildSystemItems();
    AddPlayerMarker();
    UniverseButton->SetProperty(1, 0, 0);

    // the row hides with no system or one of the four hidden ones.
    unsigned char bHidden = 1;
    if (CurrentSystem != nullptr)
    {
        unsigned const Id = CurrentSystem->GetId();
        bHidden = 0;
        for (int Index = 0; Index < 4; ++Index)
        {
            if (SkipSystemIds[Index] == Id)
            {
                bHidden = 1;
                break;
            }
        }
    }

    LabelButton->SetProperty(1, bHidden, 0);
    PhysicalFilter->SetProperty(1, bHidden, 0);
    PoliticalFilter->SetProperty(1, bHidden, 0);
    PatrolFilter->SetProperty(1, bHidden, 0);
    MineableFilter->SetProperty(1, bHidden, 0);

    // the mission filter needs mission zones (a null vector counts as
    // none).
    unsigned const bNoMission = (MissionZoneIds.empty() || bHidden != 0) ? 1u : 0u;
    MissionFilter->SetProperty(1, bNoMission, 0);

    // the political sub-filters only in modes 1 / 2.
    if (G_NavMapFilterMode == 1 || G_NavMapFilterMode == 2)
    {
        AllFilter->SetProperty(1, bHidden, 0);
        HostileFilter->SetProperty(1, bHidden, 0);
        FriendlyFilter->SetProperty(1, bHidden, 0);
    }
    else
    {
        AllFilter->SetProperty(1, 1, 0);
        HostileFilter->SetProperty(1, 1, 0);
        FriendlyFilter->SetProperty(1, 1, 0);
    }

    FilterTitle->SetProperty(1, bHidden, 0);
    LegendFilter->SetProperty(1, bHidden, 0);
    BaseListFilter->SetProperty(1, bHidden, 0);
    for (int Index = 0; Index < 4; ++Index)
        FactionLabels[Index]->SetProperty(1, 1, 0);

    // no pan, the flat mesh at full alpha, the items and the grid placed,
    // then every grid label shown.
    PanOffset[0] = 0.0f;
    PanOffset[1] = 0.0f;
    PanOffset[2] = 0.0f;
    SetMapMeshTransform(ViewMode, 1.0f);
    PlaceItems();
    PlaceGridLabels();
    for (int Index = 0; Index < 8; ++Index)
        GridLetters[Index]->SetProperty(1, 0, 0);
    for (int Index = 0; Index < 8; ++Index)
        GridNumbers[Index]->SetProperty(1, 0, 0);
}

void NeuroNetNavMap::EnterUniverseView()
{
    // .
    ViewMode = 0;
    if (G_NavMapFilterMode == 4 || G_NavMapFilterMode == 7)
        SetFilterMode(0);
    else if (G_NavMapFilterMode == 6)
        SetFilterMode(FilterModeBeforeInfoCard);

    // / 0049e5d7..0049e61b: the jump links (records freed) and the items.
    while (!JumpLinks.empty())
    {
        FNavMapJumpLink* const Link = JumpLinks.front();
        operator delete(Link);
        JumpLinks.pop_front();
    }
    while (!Items.empty())
    {
        FNavMapItem* const Item = Items.front();
        if (Item != nullptr)
            delete Item;
        Items.pop_front();
    }
    for (int Index = 0; Index < 0x63; ++Index)
        WaypointItems[Index] = nullptr;
    for (int Index = 0; Index < 0x63; ++Index)
        BestPathItems[Index] = nullptr;

    BuildUniverseItems();
    AddPlayerMarker();

    // the player marker onto the player's system's star (no null test of
    // PlayerItem or of the star's system, as in baseline).
    unsigned const PlayerSystem = GetPlayerStarSystemIdbaseline();
    for (std::list<FNavMapItem*>::iterator It = Items.begin(); It != Items.end(); ++It)
    {
        FNavMapItem* const Item = *It;
        if (Item->Kind != 0)
            continue;
        if (Item->System->GetId() != PlayerSystem)
            continue;
        PlayerItem->Position[0] = Item->Position[0];
        PlayerItem->Position[1] = Item->Position[1];
        PlayerItem->Position[2] = Item->Position[2];
        break;
    }

    // the filter row hidden, the faction labels shown.
    LabelButton->SetProperty(1, 1, 0);
    UniverseButton->SetProperty(1, 1, 0);
    PhysicalFilter->SetProperty(1, 1, 0);
    PoliticalFilter->SetProperty(1, 1, 0);
    PatrolFilter->SetProperty(1, 1, 0);
    MissionFilter->SetProperty(1, 1, 0);
    MineableFilter->SetProperty(1, 1, 0);
    LegendFilter->SetProperty(1, 1, 0);
    BaseListFilter->SetProperty(1, 1, 0);
    AllFilter->SetProperty(1, 1, 0);
    HostileFilter->SetProperty(1, 1, 0);
    FriendlyFilter->SetProperty(1, 1, 0);
    FilterTitle->SetProperty(1, 1, 0);
    for (int Index = 0; Index < 4; ++Index)
        FactionLabels[Index]->SetProperty(1, 0, 0);

    // no pan, the items placed, the grid hidden, the title "Universe"
    // (ids 0x505).
    PanOffset[0] = 0.0f;
    PanOffset[1] = 0.0f;
    PanOffset[2] = 0.0f;
    PlaceItems();
    for (int Index = 0; Index < 8; ++Index)
        GridLetters[Index]->SetProperty(1, 1, 0);
    for (int Index = 0; Index < 8; ++Index)
        GridNumbers[Index]->SetProperty(1, 1, 0);
    SystemNameText->SetProperty(0x1e, 0x505, 0);
}

// ----------------------------------------------------------------------------------------
// Side panels
// ----------------------------------------------------------------------------------------

void NeuroNetNavMap::SyncSidePanels()
{
    // not while a slide runs.
    if (ButtonMover::IsActive())
        return;

    // the eight waypoint / abort controls, scanned up to the first null.
    WinControl* const Controls[9] = {ClearAllWaypointsButton, ClearAllWaypointsText,
                                     AbortMissionButton,      AbortMissionText,
                                     AbortYesButton,          AbortYesText,
                                     AbortNoButton,           AbortNoText,
                                     nullptr};
    unsigned char bAnyShown = 0;
    for (int Index = 0; Controls[Index] != nullptr; ++Index)
    {
        if ((Controls[Index]->Flags & kWin3DVisible) != 0)
        {
            bAnyShown = 1;
            break;
        }
    }

    // the objective rows (each null-tested), then the info text (not).
    if (bAnyShown == 0)
    {
        for (int Index = 0; Index < 3; ++Index)
        {
            WinControl* const Objective = MissionObjective[Index];
            if (Objective != nullptr && (Objective->Flags & kWin3DVisible) != 0)
            {
                bAnyShown = 1;
                break;
            }
            WinControl* const Text = ObjectiveText[Index];
            if (Text != nullptr && (Text->Flags & kWin3DVisible) != 0)
            {
                bAnyShown = 1;
                break;
            }
        }
    }
    if (bAnyShown == 0 && (static_cast<WinControl*>(InfoText)->Flags & kWin3DVisible) != 0)
        bAnyShown = 1;

    if (bAnyShown != bSidePanelsOpen)
        SlideSidePanels(bAnyShown);
}

void NeuroNetNavMap::SlideSidePanels(unsigned char bOpen)
{
    // .
    if (bOpen == bSidePanelsOpen)
        return;
    PlayOrRewindUiSoundSlot(5);         // (5)
    ButtonMover::Start(0.4f);           // (0.4)
    bSidePanelsOpen = bOpen;

    // / 004a105f..004a10b4: the two side windows (z -0.22). Opening, the
    // left one comes in from 1.045 to 0.657 and the right one goes out from 0.19 to 0.585;
    // closing reverses both.
    float const LeftOut[3] = {1.045f, -0.23f, -0.22f};
    float const LeftIn[3] = {0.657f, -0.23f, -0.22f};
    float const RightIn[3] = {0.19f, -0.23f, -0.22f};
    float const RightOut[3] = {0.585f, -0.23f, -0.22f};
    if (bOpen != 0)
    {
        ButtonMover::AddButton(LeftWindow, LeftOut, LeftIn);
        ButtonMover::AddButton(RightWindow, RightIn, RightOut);
    }
    else
    {
        ButtonMover::AddButton(LeftWindow, LeftIn, LeftOut);
        ButtonMover::AddButton(RightWindow, RightOut, RightIn);
    }

    // (open) / 004a10b9..004a1604 (close): every panel control, in this
    // order, between its off-screen x (a panel base plus an offset, rounded to a float) and its
    // place; opening moves it in, closing out.
    // The offsets are the float constants FSUB / FADD read: the field, the field,
    // , the field, the field, the field, the field, the field,
    // , the field, the field, the field, the field.
    // The on-screen places are MOV immediates; six of them are one ulp away from the nearest
    // float to their decimal (0x3ea76c8c, 0x3efe76c8, 0x3eba5e36, 0x3eb22d0f, 0x3e808313,
    // ), so those are spelled with their exact value.
    FNavMapSlide const Slides[17] = {
        {ClearAllWaypointsButton, NavMapPanelX(kNavMapPanelBaseB, -static_cast<double>(0.516f)),
         -0.516f, -0.237f},
        {ClearAllWaypointsText, NavMapPanelX(kNavMapPanelBaseB, -static_cast<double>(0.659f)),
         -0.659f, -0.209f},
        {InfoText, NavMapPanelX(kNavMapPanelBaseA, static_cast<double>(0.93f)), 0.897f, -0.06f},
        {InfoTitle, NavMapPanelX(kNavMapPanelBaseA, static_cast<double>(0.36f)),
         0.3270000219345093f, 0.256f},
        {AbortMissionButton, NavMapPanelX(kNavMapPanelBaseA, static_cast<double>(0.53f)),
         0.49699997901916504f, -0.23f},
        {AbortMissionText, NavMapPanelX(kNavMapPanelBaseA, static_cast<double>(0.397f)),
         0.36400002241134644f, -0.206f},
        {AbortYesButton, NavMapPanelX(kNavMapPanelBaseA, static_cast<double>(0.447f)), 0.414f,
         -0.23f},
        {AbortYesText, NavMapPanelX(kNavMapPanelBaseA, static_cast<double>(0.381f)),
         0.34800001978874207f, -0.206f},
        {AbortNoButton, NavMapPanelX(kNavMapPanelBaseA, static_cast<double>(0.617f)), 0.584f,
         -0.23f},
        {AbortNoText, NavMapPanelX(kNavMapPanelBaseA, static_cast<double>(0.55f)), 0.517f,
         -0.206f},
        {MissionObjective[0], NavMapPanelX(kNavMapPanelBaseB, -static_cast<double>(0.547f)),
         -0.517f, 0.181f},
        {MissionObjective[1], NavMapPanelX(kNavMapPanelBaseB, -static_cast<double>(0.547f)),
         -0.517f, 0.046f},
        {MissionObjective[2], NavMapPanelX(kNavMapPanelBaseB, -static_cast<double>(0.547f)),
         -0.517f, -0.089f},
        {ObjectiveScroll, NavMapPanelX(kNavMapPanelBaseB, static_cast<double>(0.3261f)), 0.3261f,
         -0.058f},
        {ObjectiveText[0], NavMapPanelX(kNavMapPanelBaseB, -static_cast<double>(0.686f)),
         -0.656f, 0.2510000169277191f},
        {ObjectiveText[1], NavMapPanelX(kNavMapPanelBaseB, -static_cast<double>(0.686f)),
         -0.656f, 0.116f},
        {ObjectiveText[2], NavMapPanelX(kNavMapPanelBaseB, -static_cast<double>(0.686f)),
         -0.656f, -0.01900000125169754f},
    };

    for (int Index = 0; Index < 17; ++Index)
    {
        FNavMapSlide const& Slide = Slides[Index];
        float const Off[3] = {Slide.OffX, Slide.Y, 0.0f};
        float const On[3] = {Slide.OnX, Slide.Y, 0.0f};
        if (bOpen != 0)
            ButtonMover::AddButton(Slide.Control, Off, On);
        else
            ButtonMover::AddButton(Slide.Control, On, Off);
    }
}
