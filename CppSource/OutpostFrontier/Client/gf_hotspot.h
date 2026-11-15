// Author: Giperion.

#pragma once

// Hotspot -- a screen rectangle a room object answers the mouse through. baseline
// Client\supt\Hotspot.cpp (the leak warning at line 0x161 names it),
// OutpostFrontier.exe 0x00555d00..0x0055624f. Vftable 0x005e1218 has one slot, the deleting
// destructor the routine.

// Every hotspot lives in one global list (the field allocator byte, the field head,
// size): the constructor appends it, the destructor takes it out again. The mouse
// tracking in gameflow.cpp (the routine / the routine / the routine / the branch) asks
// FindHotspotAt which enabled hotspot the cursor is over and sends the owner enter (1),
// leave (2) and click (3).

// The only class derived from it is the Character's hotspot (vftable 0x005cb9ec, operator
// new(0x28) at 0x00444764, +0x20 the owning Character, +0x24 a byte), built at 0x00444783; its
// screen rectangle is refreshed by the routine from the Character's bounding sphere. Both are
// FCharacterObject / FCharacterHotspot in gf_character_object.h.

#include "common_text.h" // Rect

#include <list>

struct FRoomObject;

class FHotspot
{
public:
    // (thiscall, no arguments): the fields zeroed (Cookie -1), then this appended
    // to the list and the size (the field) incremented.
    FHotspot();

    // (non-deleting) / the routine (deleting, slot 0): the routine(this) -- drop
    // the tracker's hover when it points here -- then the first list node holding this is
    // unlinked and freed and the size decremented.
    virtual ~FHotspot();

    // (thiscall, RET 8). With an owner: Event 1 -> owner slot 10 (+0x28),
    // 2 -> slot 11 (+0x2c), 3 -> slot 12 (+0x30), each with (Cookie, Point); any other event
    // does nothing.
    void SendEvent(int Event, int const* Point);

    FRoomObject* Owner;     // +0x04
    unsigned long Cookie;   // +0x08 -1 from the ctor
    unsigned char bEnabled; // +0x0c FindHotspotAt skips a hotspot while it is clear
    unsigned char bHovered; // +0x0d set / cleared by the mouse tracking
    unsigned char Pad0E[2];
    Rect ScreenRect;        // +0x10 left, top, width, height in pixels
};

// every live hotspot, in construction order.
std::list<FHotspot*>& GetHotspotList();

// (cdecl): among the enabled hotspots whose rectangle holds Point (Point[0] x,
// Point[1] y), the one whose centre is nearest in rectangle-relative units
// ((dx / width)^2 + (dy / height)^2, or 1.0 when width or height is not positive); the first
// one wins a tie. Writes it (or null) to *OutHotspot and returns whether there was one.
bool FindHotspotAt(int const* Point, FHotspot** OutHotspot);

// (cdecl): a WARNING FDUMP (Hotspot.cpp line 0x161) for every hotspot still in
// the list, then whether the list is empty. Caller: the Gameflow shutdown the routine (a tail
// jump, so the result reaches GF slot 9's caller, which drops it).
bool ReportLeakedHotspots();
