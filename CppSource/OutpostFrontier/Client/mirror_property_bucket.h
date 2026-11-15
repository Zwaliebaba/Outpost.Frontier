// Author: Giperion.

#pragma once

// A LocationController base runtime's per-type property cache (OutpostFrontier.exe Client\Gf):
// a MetaList<T> of heap-owned T plus a completion byte. The base runtime holds five of them in
// a row, each 0x18 bytes -- +0x00 MissionVendorProperties (MetaList vftable 0x005cb024), +0x18
// NewsBroadcastProperties (0x005cb02c), then CharacterProperties (0x005cb034), ScriptBehavior
// (0x005cb03c) and AmbientScriptProperties (0x005cb044). The MirrorDatabase listener
// (the branch ..) upserts into them through the LocationController (the routine /
// ), and a flag setter (the routine for +0x14) marks the list complete.

// The MetaList half is Common's template (common_metalist.h): the routine / the routine are
// its (scalar deleting) destructor, the routine free(), the routine traverse(), the routine
// free_heap(), the routine remove(), the routine free_node() -- the MissionVendorProperties
// instantiation, ICF-shared across the five types where the bodies match.

// Ported: the MissionVendorProperties bucket (the base runtime's +0x00, read by NN_MissionVendor)
// and the NewsBroadcastProperties bucket (+0x18, the routine / the routine / the routine,
// read by NN_News). The other three types use the same template once the runtime carries them.

#include "common_metalist.h"
#include "neuroncomponents.h" // FDUMP

template <class T>
struct TMirrorPropertyBucket
{
    // (thiscall, returns this): the MetaList vptr, Head / Tail / FreeList = 0,
    // bUseHeap = 0, the +0x14 byte = 0.
    TMirrorPropertyBucket()
        : List(false)
        , bComplete(0)
    {
    }

    // (the base runtime's inlined teardown at 0x00439cda..0x00439d64 has the same
    // body): Clear, then MetaList's destructor (free + free_heap).
    ~TMirrorPropertyBucket()
    {
        Clear();
    }

    // every element destroyed and freed and its slot nulled (a traverse
    // loop), then every node to the free list or deleted (MetaList::free), then +0x14 = 0.
    void Clear()
    {
        MetaNode<T>* Node = nullptr;
        while (List.traverse(Node, true))
        {
            delete Node->Data;
            Node->Data = nullptr;
        }
        List.free();
        bComplete = 0;
    }

    // (thiscall, RET 4): the element whose key (dword +0x00, get_key) equals
    // Value's is destroyed, freed and its node removed (the routine); then a new T is
    // copy-assigned from Value (operator new(sizeof(T)), T(), operator=) and appended at the tail.
    void Upsert(T const& Value)
    {
        unsigned long const Key = Value.get_key();
        MetaNode<T>* Found = nullptr;
        MetaNode<T>* Node = nullptr;
        while (List.traverse(Node, true))
        {
            if (Node->Data->get_key() == Key)
            {
                Found = Node;
                break;
            }
        }
        if (Found != nullptr)
        {
            delete Found->Data;
            List.remove(Found);
        }
        T* const Copy = new T();
        *Copy = Value;
        List.append(Copy);
    }

    // The erase the LocationController inlines (MirrorDatabase.h line 0xf6; the routine
    // for +0x00, the routine 0x0043a7b3..0x0043a848 for +0x18): the
    // first node whose element's key (dword +0x00) equals Key -- its element destroyed and freed
    // when non-null, the node removed (the routine). No such node: a WARNING FDUMP.
    void Erase(unsigned long Key)
    {
        MetaNode<T>* Found = nullptr;
        MetaNode<T>* Node = nullptr;
        while (List.traverse(Node, true))
        {
            if (Node->Data->get_key() == Key)
            {
                Found = Node;
                break;
            }
        }
        if (Found == nullptr)
        {
            FDUMP(0x100002, "%s(%d) : *** WARNING: %s",
                  "Client\\Gf\\MirrorDatabase.h", 0xf6,
                  "Cannot find record to remove\n");
            return;
        }
        if (Found->Data != nullptr)
            delete Found->Data;
        List.remove(Found);
    }

    MetaList<T> List;          // +0x00 (vptr, Head +0x04, Tail +0x08, bUseHeap +0x0c, FreeList +0x10)
    unsigned char bComplete;   // +0x14 set by the LocationController (the routine for +0x00's)
    unsigned char Pad15[3];
};
