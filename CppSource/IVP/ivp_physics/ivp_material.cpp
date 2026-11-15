// Author: Giperion.

#include "ivp_material.h"
#include "ivp_mindist.h"

// ============================================================================
// (thiscall, RET 8). baseline's store order is +0xc, +0x8, vtable,
// +0x10, +0x14, +0x18 (module storage-0634bede); +0x04 is not written. The
// vtable store is the compiler's, so the member order below is the observable
// one: SecondFrictionXEnabled = 0, FrictionFactor = arg1, the two zeroed floats
// and Elasticity = arg2.
// ============================================================================
IVP_Material_Simple::IVP_Material_Simple(float FrictionValue, float ElasticityValue)
{
    FrictionFactor = FrictionValue;
    SecondFrictionXEnabled = 0;
    SecondFrictionFactor = 0.0f;
    Elasticity = ElasticityValue;
    Adhesion = 0.0f;
}

// ============================================================================
// , IVP_Material_Manager slot 0 (RET 8, both arguments unread).
// A function-local static guarded by bit 0 of the field: on first use
// operator_new(0x1c), the routine(0.5f, 0.5f) (`PUSH 0x3f000000` twice,
// ), pointer stored in the field (null if
// operator_new returned null, module storage-0634be7d). No atexit is
// registered, so the material is never freed -- a pointer static reproduces
// that exactly.
// ============================================================================
IVP_Material* IVP_Material_Manager::get_material_by_index(const IVP_U_Point* WorldPosition, int Index)
{
    (void)WorldPosition;
    (void)Index;
    static IVP_Material_Simple* DefaultMaterial = new IVP_Material_Simple(0.5f, 0.5f);
    return DefaultMaterial;
}

// ============================================================================
// (RET 4). Contact+0x44 (the second side) is asked first and its
// result spilled to a float (`FSTP float ptr [ESP+0x8]`, module storage), then
// Contact+0x40's slot 0 is multiplied by that float on the FPU.
// ============================================================================
float IVP_Material_Manager::get_friction_factor(IVP_Impact_Solver_Long_Term* Contact)
{
    const float SecondSide = Contact->Materials[1]->get_friction_factor();
    return Contact->Materials[0]->get_friction_factor() * SecondSide;
}

// ============================================================================
// (RET 4). Same shape as slot 1 but slot 2 of each material, and
// the first side (+0x40) is called first (module storage-0634bd38).
// ============================================================================
float IVP_Material_Manager::get_elasticity(IVP_Impact_Solver_Long_Term* Contact)
{
    const float FirstSide = Contact->Materials[0]->get_elasticity();
    return Contact->Materials[1]->get_elasticity() * FirstSide;
}

// ============================================================================
// (RET 4). Slot 3 of each material, first side first, summed
// (`FADD float ptr [ESP+0x8]`, module storage).
// ============================================================================
float IVP_Material_Manager::get_adhesion(IVP_Impact_Solver_Long_Term* Contact)
{
    const float FirstSide = Contact->Materials[0]->get_adhesion();
    return Contact->Materials[1]->get_adhesion() + FirstSide;
}

// ============================================================================
// (RET 4). `MOV EAX,[ECX+4]; TEST; JZ; TEST ECX,ECX; JZ;
// CALL [vtable+0x10](1)` -- the scalar deleting destructor, slot 4. The
// `this != null` test is the compiler's delete-expression check.
// ============================================================================
void IVP_Material_Manager::environment_will_be_deleted(IVP_Environment* Environment)
{
    (void)Environment;
    if (DeleteOnEnvDelete != 0)
    {
        delete this;
    }
}
