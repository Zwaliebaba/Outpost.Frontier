// Author: Giperion.

#pragma once

// UIWinDeform -- the PAGE_*.page character-preview widget (RTTI .?AUUIWinDeform@@ : Win3D, COL
// ; baseline UIWin.cpp). It shows one EngineCharacter body (a Costume's deformable plus
// its accessories) through a camera of its own laid over the widget's screen rectangle.

// baseline:
//   WINFACTORY<UIWinDeform> the field, static init the branch -> the routine("UIWinDeform"),
//   atexit the branch. Create 0x0059cac0: operator_new(0x3f4) + ctor the routine, then slot 9.
//   ctor the routine (Win3D's inlined), dtor the routine, scalar deleting the routine.
//   vftable 0x005e5804 overrides of Win3D 0x005d5544:
//     slot 0  the routine  scalar deleting destructor
//     slot 5  the branch  SetPosition (+ the body's pose)
//     slot 8  the branch  SetTransform (+ the camera viewport)
//     slot 9  the branch  Create (payload {Width, Height})
//     slot 11 the branch  Update (the body advanced in 0.2 s steps)
//     slot 13 the branch  Draw
//   Non-virtual: the routine SetCostume, the routine the body build (FEngineCharacterBody::Build,
//   engine_character.cpp), the routine the part add, the routine the hardpoint key.

// Layout past Win3D (+0x00..+0x7b):
//   +0x07c std::string -- constructed and destroyed only. Checked 2026-09-30 (W4-N3) outside
//          UIWin.cpp too: the only code holding a UIWinDeform* is PageCharacterCreate (+0x104,
//          CharCreate.cpp, whose +0x7c accesses are its own IUIPage face) and the Win3D manager /
//          WinFactoryCreate (the routine copies the nickname to Win3D +0x18), which know the
//          0x7c-byte Win3D only. No reader or writer exists in the exe.
//   +0x08c FEngineCharacterBody (0x298)
//   +0x324 the preview camera (the routine base camera, 0xc0)
//   +0x3e4 / +0x3e8 width / height in world units (slot 9 payload)
//   +0x3ec the body's pose Z (ctor -5.0f), +0x3f0 the body's height (ctor -1.0f)
//   size 0x3f4 (operator_new at 0x0059cad8)

#include "engine_character.h" // FEngineCharacterBody
#include "space_camera.h"     // FSpaceCamera
#include "win3d.h"

#include <string>

class UiWinDeform : public Win3D
{
public:
    // .
    UiWinDeform();
    // .
    ~UiWinDeform() override;

    // slot 5 (+0x14) -- the branch.
    void SetPosition(float const* NewPosition) override;
    // slot 8 (+0x20) -- the branch.
    void SetTransform(float const* Transform) override;
    // slot 9 (+0x24) -- the branch.
    bool Create(Win3D* NewParent, void* Payload) override;
    // slot 11 (+0x2c) -- the branch.
    void Update() override;
    // slot 13 (+0x34) -- the branch.
    void Draw() override;

    // (thiscall, RET 4) -- the body rebuilt from the costume called Name
    // (CostumeDescriptions::find_costume / get_costume), then posed; a failed build FDUMPs a
    // notice (UIWin.cpp line 0x210). Caller: PageCharacterCreate (0x00561387).
    void SetCostume(char const* Name);

    std::string Field_0x7c;       // +0x07c
    FEngineCharacterBody Body;    // +0x08c
    FSpaceCamera Camera;          // +0x324
    float Width;                  // +0x3e4
    float Height;                 // +0x3e8
    float PoseZ;                  // +0x3ec
    float PoseHeight;             // +0x3f0

private:
    // The body's pose both SetCostume (0x0059b12d..0x0059b20e) and SetPosition
    // (0x0059bb6b..0x0059bc1c) spell inline: a rotation of pi about Y (the routine, then
    // (0) / the routine(0)), IDeformable slot 24 with PoseHeight (only with a
    // deformable), slot 19 with {0, 0, PoseZ}, slot 20 with the rotation.
    void PoseBody();

    UiWinDeform(UiWinDeform const&) = delete;
    UiWinDeform& operator=(UiWinDeform const&) = delete;
};
