// Author: Giperion.

// supt\Hotspot.cpp -- OutpostFrontier.exe 0x00555d00..0x0055624f. See gf_hotspot.h.

#include "gf_hotspot.h"

#include "gameflow.h"     // ClearHoveredHotspotIf (the routine)
#include "room_control.h" // FRoomObject slots 10..12

#include "neuroncomponents.h"        // FDUMP

namespace
{
    char const kHotspotCpp[] = "Client\\supt\\Hotspot.cpp";

    // the branch (static initializer) stores .rdata the field (0.6f) here;
    // nothing in the PE reads it.
    float G_Hotspot6799a4 = 0.6f;

    // / the field / the field -- the branch builds the empty list and
    // registers the branch with atexit, which frees every node (not the hotspots) and the
    // head. The std::list's own construction and destruction are that pair.
    std::list<FHotspot*> G_Hotspots;
}

std::list<FHotspot*>& GetHotspotList()
{
    return G_Hotspots;
}

FHotspot::FHotspot()
    : Owner(nullptr)
    , Cookie(0xffffffffu)
    , bEnabled(0)
    , bHovered(0)
{
    // +0x10..+0x1c are zeroed twice (0x00555db7..0x00555dc0 and again
    // ), +0x04 = 0, +0x08 = -1, +0x0c = +0x0d = 0; then a node is
    // appended at the tail (0x00555de6..0x00555e13) and the field incremented.
    Pad0E[0] = 0;
    Pad0E[1] = 0;
    ScreenRect.Left = 0;
    ScreenRect.Top = 0;
    ScreenRect.Width = 0;
    ScreenRect.Height = 0;
    G_Hotspots.push_back(this);
}

FHotspot::~FHotspot()
{
    // the tracker lets go of this hotspot first (the routine).
    ClearHoveredHotspotIf(this);

    // the first node whose value is this is unlinked and freed, the
    // size decremented; no node, nothing happens.
    for (std::list<FHotspot*>::iterator It = G_Hotspots.begin(); It != G_Hotspots.end(); ++It)
    {
        if (*It == this)
        {
            G_Hotspots.erase(It);
            break;
        }
    }
}

void FHotspot::SendEvent(int Event, int const* Point)
{
    // .
    if (Owner == nullptr)
        return;
    if (Event == 1)
        Owner->Slot10(Cookie, Point);
    else if (Event == 2)
        Owner->Slot11(Cookie, Point);
    else if (Event == 3)
        Owner->Slot12(Cookie, Point);
}

bool FindHotspotAt(int const* Point, FHotspot** OutHotspot)
{
    // . Best / BestScore start at 0 (0x0055609d / 0x005560a5).
    FHotspot* Best = nullptr;
    float BestScore = 0.0f;

    for (std::list<FHotspot*>::iterator It = G_Hotspots.begin(); It != G_Hotspots.end(); ++It)
    {
        FHotspot* const Hotspot = *It;
        // +0x0c clear -> next.
        if (!Hotspot->bEnabled)
            continue;
        // Rect::point_is_inside(x, y) on +0x10.
        if (!Hotspot->ScreenRect.point_is_inside(Point[0], Point[1]))
            continue;

        // Rect::get_center on a copy of the rectangle.
        Rect const Copy = Hotspot->ScreenRect;
        int CenterX = 0;
        int CenterY = 0;
        Copy.get_center(&CenterX, &CenterY);

        // the score starts as 1.0f (the field); 0x0055612d..0x0055615d replace it
        // only when Width > 0 and Height > 0 (both JLE): ((x - cx) / w)^2 + ((y - cy) / h)^2,
        // with FIDIV on the integer width / height (x87 extended precision).
        double Score = 1.0;
        if (Copy.Width > 0 && Copy.Height > 0)
        {
            double const Dx = static_cast<double>(Point[0] - CenterX) /
                              static_cast<double>(Copy.Width);
            double const Dy = static_cast<double>(Point[1] - CenterY) /
                              static_cast<double>(Copy.Height);
            Score = Dy * Dy + Dx * Dx;
        }

        // no best yet -> take it; otherwise FCOM against the stored
        // float with TEST AH,0x05 / JP -- the jump (keep the old best) is taken unless C0 alone
        // is set, i.e. the new one is taken only when Score < BestScore (not on equal, not on
        // an unordered compare).
        if (Best == nullptr || Score < static_cast<double>(BestScore))
        {
            BestScore = static_cast<float>(Score); // FSTP float [ESP+0x10]
            Best = Hotspot;
        }
    }

    // .
    if (Best != nullptr)
    {
        *OutHotspot = Best;
        return true;
    }
    *OutHotspot = nullptr;
    return false;
}

bool ReportLeakedHotspots()
{
    // .
    for (std::list<FHotspot*>::iterator It = G_Hotspots.begin(); It != G_Hotspots.end(); ++It)
    {
        FHotspot* const Hotspot = *It;
        FDUMP(0x100002, "%s(%d) : *** WARNING: hotspot = %p id = %d not freed\n", kHotspotCpp,
              0x161, Hotspot, Hotspot->Cookie);
    }
    // the field == 0.
    return G_Hotspots.size() == 0;
}
