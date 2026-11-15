// Author: Giperion.

#include "ivp_template_polygon.h"

#include <cstdlib>
#include <new>

// (module storage-0636d955): every field zeroed.
IVP_Template_Polygon::IVP_Template_Polygon()
    : n_points(0)
    , points(nullptr)
    , n_lines(0)
    , lines(nullptr)
    , n_surfaces(0)
    , surfaces(nullptr)
{
}

// (module storage-0636d9b2): operator delete(lines), lines = 0;
// operator delete(points), points = 0; when surfaces is set, the eh vector
// destructor iterator (the routine: element 0x20, count at surfaces[-4],
// element dtor 0636da20 -> 0636d9e0) and operator delete(surfaces - 4) --
// delete[] -- then surfaces = 0 (on both paths).
IVP_Template_Polygon::~IVP_Template_Polygon()
{
    ::operator delete(lines);
    lines = nullptr;
    ::operator delete(points);
    points = nullptr;
    if (surfaces != nullptr)
    {
        delete[] surfaces;
    }
    surfaces = nullptr;
}

// (module storage-0636d9d1): REP STOSD of 8 zero dwords.
IVP_Template_Surface::IVP_Template_Surface()
    : templ(nullptr)
    , n_lines(0)
    , lines(nullptr)
    , revert_line(nullptr)
{
    normal[0] = 0.0f;
    normal[1] = 0.0f;
    normal[2] = 0.0f;
    normal[3] = 0.0f;
}

// (module storage-0636da0e): when lines is set, the routine
// (free) and lines = 0; then operator delete(revert_line) unconditionally and
// revert_line = 0.
IVP_Template_Surface::~IVP_Template_Surface()
{
    if (lines != nullptr)
    {
        free(lines);
        lines = nullptr;
    }
    ::operator delete(revert_line);
    revert_line = nullptr;
}

// (module storage-0636da1b): (this - templ->surfaces) >> 5,
// arithmetic shift.
int32_t IVP_Template_Surface::get_surface_index() const
{
    return static_cast<int32_t>(reinterpret_cast<intptr_t>(this) - reinterpret_cast<intptr_t>(templ->surfaces)) >> 5;
}
