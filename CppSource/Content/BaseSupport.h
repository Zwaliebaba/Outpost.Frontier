// Author: Giperion.

#pragma once

// Content.dll TU19 (06eb3ec0-06eb6c00): the BaseSupport controller and the news table
// (missions\news.ini, [NewsItem]).

#include "BaseDB.h"             // FBarMissionOfferData
#include "common_archetype.h"   // CacheString
#include "common_goods.h"       // NewsBroadcastDescription

#include <list>
#include <map>
#include <vector>

class INI_Reader;
struct FContentPlayerState;     // ContentPlayerState.h

// One [NewsItem] record, 0x80 bytes (element of vector the field..the field).
// Default state is set by the loader the routine (06eb451a-06eb4597); copy is the
// member-wise the routine, destruction the routine.
struct FNewsItem
{
    FNewsItem();
    FNewsItem(FNewsItem const& Other) = default;
    FNewsItem& operator=(FNewsItem const& Other) = default;
    ~FNewsItem();

    std::vector<unsigned int> Bases;                       // +0x00 "base", unique ids
    int FromStage;                                         // +0x10 "rank"[0], default 0
    int ToStage;                                           // +0x14 "rank"[1], default 0x2a
    CacheString Audio;                                     // +0x18 "audio" (StringAlloc)
    CacheString Logo;                                      // +0x1c "logo" (StringAlloc)
    // +0x20. Common's field names do not follow the ini keys: "category" goes to
    // Desc.Headline (+0x2c), "headline" to Desc.Text (+0x44), "text" to Desc.Audio
    // (+0x5c); "breaking" -> bFlag1, "autoselect" -> bFlag2, "icon" -> Category,
    // "logo" -> LogoTexture (+0x74), "audio" -> AudioName (+0x78).
    NewsBroadcastDescription Desc;
};

// constructed by the branch, destroyed by the atexit the routine.
extern std::vector<FNewsItem> GNewsItems;

// one record per [NewsItem] section, appended to GNewsItems.
void __cdecl LoadNewsItems(INI_Reader* Reader);

// BaseSupport message 0x2c (mission offer accepted), 0x10 bytes. Read by the branch
// ; sent with Kind 0 by FBarOffer::Accept (06ea8ec2) and re-sent as is to the
// new mission controller (06eb51dc).
struct FMissionOfferAcceptMsg
{
    unsigned int OfferId;           // +0x00 vendor offer id / bar character id / controller
    unsigned int Kind;              // +0x04 2 vendor offer, 1 bar character
    unsigned char bAccepted;        // +0x08 raw message byte, forwarded unchanged
    unsigned int PlayerId;          // +0x0c
};

// A mission vendor offer of a docked player (list element, node +8). Built by TU35
// from pub::GF::MissionVendorOfferCreate and the generated mission (copied
// with the routine, the loader reference moves in); released by the routine or by an
// accepted offer (BaseSupport msg 0x2c).
struct FBaseVendorOffer
{
    unsigned long VendorHandle;             // +0x00 MissionVendorOfferCreate
    unsigned int OfferId;                   // +0x04 the description's id after Create;
                                            // matched with BaseSupport msg 0x2c +0
    FBarMissionOfferData Data;              // +0x08 loader, mission, type
};

// The per-player record of a base visit (map value, node +0x10, 0x1c bytes): built by
// BaseSupport msg 1, dropped by msg 2.
struct FBaseVisit
{
    std::vector<unsigned long> NewsHandles;         // +0x00 pub::GF::NewsBroadcastCreate
    std::list<FBaseVendorOffer> VendorOffers;       // +0x10
};

// player id -> visit (map; find the routine, insert the routine, erase
// ).
extern std::map<unsigned int, FBaseVisit> GBaseVisits;

// (cdecl; caller FBarOffer::Accept the routine 06ea8f55, bribe): rebuild the
// player's mission vendor offers -- release them (the routine), then the routine for the
// player's base.
void __cdecl RefreshMissionVendorOffers(FContentPlayerState* State);
