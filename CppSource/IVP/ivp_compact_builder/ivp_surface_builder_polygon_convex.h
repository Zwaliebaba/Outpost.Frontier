// Author: Giperion.

#pragma once

#include "ivp_compact_ledge.h"
#include "ivp_point_hash.h"
#include "../ivp_utility/ivp_u_vector.h"
#include "../ivp_utility/ivp_vhash.h"

// ============================================================================
// IVP_SurfaceBuilder_Polygon_Convex -- builds one IVP_Compact_Ledge from an
// IVP_Template_Polygon (through IVP_Object_Polygon_Tetra) or from a single
// triangle (Common.dll module storage-0636e0f5).
// (2026-10-08, claude-code:cmn-ivp4b)
// ============================================================================

class IVP_Object_Polygon_Tetra;
class IVP_Template_Polygon;
class IVP_Triangle;

// Point hash of the polygon builder and of IVP_Compact_Ledge_Generator,
// vtable module storage {0636cfe0 compare, 0636dae0 scalar deleting dtor}.
// Created as operator new(0x10) + the routine(size) + vtable store
// (0636da48, 0636db1f, 0637a37d). The baseline class name is not in the binary.
class FPolygonPointHash : public IVP_VHash
{
public:
    explicit FPolygonPointHash(int Size)
        : IVP_VHash(Size)
    {
    }

    // .
    int Compare(void* Elem0, void* Elem1) override
    {
        return IVP_PointHashCompare(Elem0, Elem1);
    }

    // -> the routine (vtable store, JMP the routine base
    // dtor), then operator delete when bit 0 of the flag is set.
    ~FPolygonPointHash() override
    {
    }

    // The {hash index, element} pair array behind IVP_VHash::Elems.
    struct FEntry
    {
        unsigned HashIndex;
        void* Elem;
    };
};

// The triangle argument of the second constructor (0636db00): +0x08 is a
// pointer to three 16-byte points; the two dwords before it are not read
// there (the single-triangle path 0636c800 passes {0, 2, points}).
struct IVP_Template_Triangle_Points
{
    int32_t Field_0x00;            // +0x00 0 at the only caller (TODO name)
    int32_t Field_0x04;            // +0x04 2 at the only caller (TODO name)
    IVP_U_Float_Point* points;     // +0x08
};

// 0xc bytes, built on the stack by 0636e080 / 0636e0c0.
class IVP_SurfaceBuilder_Polygon_Convex
{
public:
    // (thiscall, RET 4).
    explicit IVP_SurfaceBuilder_Polygon_Convex(IVP_Template_Polygon* Templ);
    // (thiscall, RET 4).
    explicit IVP_SurfaceBuilder_Polygon_Convex(IVP_Template_Triangle_Points* Triangle);
    // .
    ~IVP_SurfaceBuilder_Polygon_Convex();

    // (cdecl): null template -> null.
    static IVP_Compact_Ledge* convert_template_to_ledge(IVP_Template_Polygon* Templ);
    // (cdecl): null triangle -> null.
    static IVP_Compact_Ledge* convert_triangle_to_ledge(IVP_Template_Triangle_Points* Triangle);

    IVP_Object_Polygon_Tetra* tetras;   // +0x00
    IVP_Compact_Ledge* c_ledge;         // +0x04 aligned block (the routine)
    FPolygonPointHash* point_hash;      // +0x08 the deduplicated triangle points (second ctor)

private:
    // .
    void init_surface_manager_polygon();
    // (thiscall, RET 4): appends every front triangle (is_back_side == 0).
    void add_triangles_to_vector(IVP_U_Vector<IVP_Triangle>* Triangles);
};
static_assert(sizeof(IVP_SurfaceBuilder_Polygon_Convex) == 0xc, "IVP_SurfaceBuilder_Polygon_Convex is 0xc bytes");
