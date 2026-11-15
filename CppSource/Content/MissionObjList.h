// Author: Giperion.

#pragma once

// Content.dll TU40 (06f24670-06f28f60): the [ObjList] directives. The loader the routine
// reads an [ObjList] section into a list of directives; the factory the routine builds one
// directive per type; the TU50 ship (BehaviorCmds.h) runs them one after the other through
// slot 3 (Execute). The TU37 guides build them through the factory and the setters below.
// Owner: lane CNT-AI (moved from CNT-MS-TRIG, 2026-09-30).

// Every class derives from the abstract base of ObjListDirective.h (vtable 06fb8314) and
// embeds one pub::AI op at +0x10c; slots 0-2 (AddRef / Release / sdd 06f06eb0) and slot 6
// (Save 06f25f30) are the base's in every vtable unless stated. No RTTI in Content: the
// class names are the [ObjList] keys that create them.

// Uninitialised baseline members: the factory does not write every member (e.g. the goto int
// +0x17c, SetPriority's +0x10c). Parse writes them before any reader runs; the port starts
// them at 0 so no member is indeterminate.

#include "ObjListDirective.h"
#include "common_pub.h"       // pub::AI ops
#include "common_tstring.h"   // TString

#include <list>
#include <vector>

class INI_Reader;
class FContentShipBase;
struct FFormation;            // Encounters.h (TU31)

// (cdecl): *Out = a new directive of Type (0..0x16, 0x19; 0x17 / 0x18 and
// anything above 0x19 give null); returns *Out != null.
bool __cdecl CreateObjListDirective(int Type, FObjListDirective** Out);

// (cdecl; callers TU38 06f107b7, 06f18296): one [ObjList] section. *OutNickname
// = CreateID("") first, then the "nickname" key; every directive key appends a new directive
// (its Name = the whole line, then its Parse) to List, which holds one reference on it.
void __cdecl LoadObjList(INI_Reader* Reader, std::list<FObjListDirective*>* List,
                         unsigned int* OutNickname);

// Factory case 0, "Avoidance" (0x120, vtable 06fb9060).
class FObjListAvoidance : public FObjListDirective
{
public:
    bool Execute(FContentShipBase* Ship) override;
    void Parse(INI_Reader* Reader) override;
    int GetType() override;                          // 0
    // (RET 4; caller TU21 06eb8ddf): FlagId = 0, bValue = bAvoid.
    void SetAvoidance(bool bAvoid);

    pub::AI::SetFlagParams Op;                       // +0x10c
};

// Factory case 1, "Delay" (0x11c, vtable 06fb82f8).
class FObjListDelay : public FObjListDirective
{
public:
    bool Execute(FContentShipBase* Ship) override;
    void Parse(INI_Reader* Reader) override;
    int GetType() override;                          // 1
    // (RET 4): DelaySeconds.
    void SetDelay(float Seconds);

    pub::AI::DirectiveDelayOp Op;                    // +0x10c
};

// Factory case 2, "Idle" (0x118, vtable 06fb900c; Parse is the base's).
class FObjListIdle : public FObjListDirective
{
public:
    bool Execute(FContentShipBase* Ship) override;   // (ICF with Delay)
    int GetType() override;                          // 2

    pub::AI::DirectiveIdleOp Op;                     // +0x10c
};

// Factory case 3, "GotoShip" (0x1e8, vtable 06fb8f64).
class FObjListGotoShip : public FObjListDirective
{
public:
    FObjListGotoShip();
    bool Execute(FContentShipBase* Ship) override;
    void Parse(INI_Reader* Reader) override;
    int GetType() override;                          // 3
    // (RET 0xc; callers TU31 06efc3cf, TU37 06f0cb1c): Field_0C = 0 (target
    // object), Field_1C = *TargetId, Field_58 = 0, Field_50 = Range, Field_54 = Param3.
    void SetTarget(unsigned int const* TargetId, float Range, float Param3);

    pub::AI::DirectiveGotoOp Op;                     // +0x10c
    int Field_0x17c = 0;                             // +0x17c Parse: 1, value 9. TODO: meaning
    TString<0x30> TargetName;                        // +0x180 -> Op.Field_1C
    TString<0x30> ObjectName;                        // +0x1b4 -> Op.Field_5C
};

// Factory case 4, "GotoSpline" (0x1b4, vtable 06fb8f48).
class FObjListGotoSpline : public FObjListDirective
{
public:
    FObjListGotoSpline();
    bool Execute(FContentShipBase* Ship) override;   // (shared with GotoVec)
    void Parse(INI_Reader* Reader) override;
    int GetType() override;                          // 4

    pub::AI::DirectiveGotoOp Op;                     // +0x10c
    int Field_0x17c = 0;                             // +0x17c Parse: 1, value 20. TODO: meaning
    TString<0x30> ObjectName;                        // +0x180 -> Op.Field_5C
};

// Factory case 5, "GotoVec" (0x1b4, vtable 06fb8f2c).
class FObjListGotoVec : public FObjListDirective
{
public:
    FObjListGotoVec();
    bool Execute(FContentShipBase* Ship) override;
    void Parse(INI_Reader* Reader) override;
    int GetType() override;                          // 5
    // (RET 0x18; callers TU37 06f0b54b, 06f0b889, 06f0ba78, 06f0be29, TU43
    // , ...): Field_0C = 1 (a point), Destination, Field_50 / Field_54, Field_5C =
    // ObjectId when (ObjectId & 0x3fffffff) != 0, Field_58 = 0, Field_5A = bCruise, Field_5B
    // = bNoCruise.
    void SetDestination(Vector const* Destination, float Field50, float Field54,
                        unsigned int ObjectId, bool bNoCruise, bool bCruise);

    pub::AI::DirectiveGotoOp Op;                     // +0x10c
    int Field_0x17c = 0;                             // +0x17c Parse: 1, value 11. TODO: meaning
    TString<0x30> ObjectName;                        // +0x180 -> Op.Field_5C
};

// Factory case 6, "Trail" (0x15c, vtable 06fb8e30).
class FObjListTrail : public FObjListDirective
{
public:
    FObjListTrail();
    bool Execute(FContentShipBase* Ship) override;
    void Parse(INI_Reader* Reader) override;
    int GetType() override;                          // 6
    // (RET 8; caller TU37 06f0ce19).
    void SetTarget(unsigned int const* TargetId, float Distance);

    pub::AI::DirectiveTrailOp Op;                    // +0x10c
    TString<0x30> TargetName;                        // +0x128 -> Op.TargetId
};

// Factory case 7, "Follow" (0x168, vtable 06fb8fb8).
class FObjListFollow : public FObjListDirective
{
public:
    FObjListFollow();
    bool Execute(FContentShipBase* Ship) override;
    void Parse(INI_Reader* Reader) override;
    int GetType() override;                          // 7

    int Field_0x10c = 0;                             // +0x10c Parse: 1. TODO: meaning
    TString<0x30> TargetName;                        // +0x110 -> Op.TargetId
    pub::AI::DirectiveFollowOp Op;                   // +0x144
};

// Factory case 8, "Guide" (0x200, vtable 06fb8f9c).
class FObjListGuide : public FObjListDirective
{
public:
    FObjListGuide();
    bool Execute(FContentShipBase* Ship) override;
    void Parse(INI_Reader* Reader) override;
    int GetType() override;                          // 5
    // (RET 0x20; callers TU37 06f09ad6, 06f09baf, 06f09c7e, 06f09eb6).
    void SetGuide(unsigned int const* TargetId, Vector const* FollowOffset,
                  Vector const* Destination, float Field50, bool bNoCruise, bool bCruise,
                  bool FieldA0, bool Field58);

    pub::AI::DirectiveGuideOp Op;                    // +0x10c
    int Field_0x1c8 = 0;                             // +0x1c8 Parse: 1, value 19. TODO: meaning
    TString<0x30> TargetName;                        // +0x1cc -> GotoParams.Field_5C
};

// Factory case 9, "GuideShip" (0x234, vtable 06fb8f80).
class FObjListGuideShip : public FObjListDirective
{
public:
    FObjListGuideShip();
    bool Execute(FContentShipBase* Ship) override;
    void Parse(INI_Reader* Reader) override;
    int GetType() override;                          // 5

    pub::AI::DirectiveGuideOp Op;                    // +0x10c
    int Field_0x1c8 = 0;                             // +0x1c8 Parse: 1, value 17. TODO: meaning
    TString<0x30> ObjectName;                        // +0x1cc -> GotoParams.Field_1C
    TString<0x30> TargetName;                        // +0x200 -> GotoParams.Field_5C
};

// Factory case 0xa, "Face" (0x188, vtable 06fb8f10).
class FObjListFace : public FObjListDirective
{
public:
    FObjListFace();
    bool Execute(FContentShipBase* Ship) override;
    void Parse(INI_Reader* Reader) override;
    int GetType() override;                          // 0xa

    pub::AI::DirectiveFaceOp Op;                     // +0x10c
    TString<0x30> TargetName;                        // +0x154 -> Op.Field_10
};

// Factory case 0xb, "SetLifeTime" (0x110, vtable 06fb8e84).
class FObjListSetLifeTime : public FObjListDirective
{
public:
    bool Execute(FContentShipBase* Ship) override;
    void Parse(INI_Reader* Reader) override;
    int GetType() override;                          // 0xb

    int LifeTime = 0;                                // +0x10c
};

// Factory case 0xc, "SetPriority" (0x110, vtable 06fb8ea0).
class FObjListSetPriority : public FObjListDirective
{
public:
    bool Execute(FContentShipBase* Ship) override;
    void Parse(INI_Reader* Reader) override;
    int GetType() override;                          // 0xc

    int Priority = 0;                                // +0x10c NORMAL 1, ALWAYS_EXECUTE 0
};

// Factory cases 0xd "StayInRange" (vtable 06fb8e68) and 0xe "StayOutOfRange" (06fb8e4c),
// both 0x180 with SetZoneBehaviorParams at +0x10c and a TString at +0x14c. Their Execute
// (06f27cb0 / 06f27fa0) and Parse (06f27df0 / 06f280a0) bodies differ only in the value
// Parse gives Op.Field_10 first (0 / 1).
class FObjListStayInRange : public FObjListDirective
{
public:
    FObjListStayInRange();
    bool Execute(FContentShipBase* Ship) override;
    void Parse(INI_Reader* Reader) override;
    int GetType() override;                          // 0xd
    // (RET 8; callers TU61 06f8e491, 06f91e4d): Mode = InvertMode = 0,
    // TargetPoint = *Center, Radius = Radius.
    void SetSphere(Vector const* Center, float Radius);

    pub::AI::SetZoneBehaviorParams Op;               // +0x10c
    TString<0x30> ObjectName;                        // +0x14c -> Op.TargetId
};

class FObjListStayOutOfRange : public FObjListDirective
{
public:
    FObjListStayOutOfRange();
    bool Execute(FContentShipBase* Ship) override;
    void Parse(INI_Reader* Reader) override;
    int GetType() override;                          // 0xe

    pub::AI::SetZoneBehaviorParams Op;               // +0x10c
    TString<0x30> ObjectName;                        // +0x14c -> Op.TargetId
};

// Factory case 0xf, "MakeNewFormation" (0x124, vtable 06fb8ed8, dtor 06f25210).
class FObjListMakeNewFormation : public FObjListDirective
{
public:
    bool Execute(FContentShipBase* Ship) override;
    void Parse(INI_Reader* Reader) override;
    int GetType() override;                          // 0xf

    FFormation* Formation = nullptr;                 // +0x10c
    std::vector<TString<0x30>> MemberNames;          // +0x110 (alloc byte, +0x114..+0x11c)
    bool bAddFollowers = false;                      // +0x120 "ADD_FOLLOWERS"
};

// Factory case 0x10, "FollowPlayer" (0x120, vtable 06fb8ebc, dtor 06f25210).
class FObjListFollowPlayer : public FObjListDirective
{
public:
    bool Execute(FContentShipBase* Ship) override;
    void Parse(INI_Reader* Reader) override;
    int GetType() override;                          // 0xf

    FFormation* Formation = nullptr;                 // +0x10c
    std::vector<TString<0x30>> MemberNames;          // +0x110
};

// Factory case 0x11, "BreakFormation" (0x10c, vtable 06fb9044; Parse is the base's).
class FObjListBreakFormation : public FObjListDirective
{
public:
    bool Execute(FContentShipBase* Ship) override;
    int GetType() override;                          // 0x11
};

// Factory case 0x12, "CancelObjective" (0x10c, vtable 06fb9028; Parse is the base's).
class FObjListCancelObjective : public FObjListDirective
{
public:
    bool Execute(FContentShipBase* Ship) override;
    int GetType() override;                          // 0x12
};

// Factory case 0x13, "Dock" (0x16c, vtable 06fb8ff0).
class FObjListDock : public FObjListDirective
{
public:
    FObjListDock();
    bool Execute(FContentShipBase* Ship) override;
    void Parse(INI_Reader* Reader) override;
    int GetType() override;                          // 0x13
    // (RET 8; callers TU37 06f06965, 06f09d90, 06f0a8cf, TU53 06f60148).
    void SetTarget(unsigned int const* DockObjectId, unsigned int const* Field10);

    pub::AI::DirectiveDockOp Op;                     // +0x10c
    TString<0x30> ObjectName;                        // +0x138 -> Op.Field_1C
};

// Factory case 0x14, "InstantTradelane" (0x124, vtable 06fb8fd4).
class FObjListInstantTradelane : public FObjListDirective
{
public:
    bool Execute(FContentShipBase* Ship) override;
    void Parse(INI_Reader* Reader) override;
    int GetType() override;                          // 0x13 (same body as Dock's)
    // (RET 0xc; caller TU53 06f65e7a).
    void SetLane(unsigned int const* TradeLaneId, unsigned int Field14,
                 unsigned int const* Field10);

    pub::AI::DirectiveInstantTradelaneOp Op;         // +0x10c
};

// Factory case 0x15, "Launch" (0x124, vtable 06fb8ef4).
class FObjListLaunch : public FObjListDirective
{
public:
    bool Execute(FContentShipBase* Ship) override;
    void Parse(INI_Reader* Reader) override;
    int GetType() override;                          // 0x15
    // (RET 0xc; caller TU53 06f60a48).
    void SetLaunch(unsigned int const* LaunchObjectId, int Field10, int Field14);

    pub::AI::DirectiveLaunchOp Op;                   // +0x10c
};

// Factory case 0x16, "WaitForPlayerManeuver" (0x13c, ctor the routine, vtable 06fb907c).
class FObjListWaitForPlayerManeuver : public FObjListDirective
{
public:
    FObjListWaitForPlayerManeuver();
    bool Execute(FContentShipBase* Ship) override;
    void Parse(INI_Reader* Reader) override;
    int GetType() override;                          // 0x16

    pub::AI::DirectiveWaitForPlayerManeuverOp Op;    // +0x10c (BehaviorName -> +0x11c)
    char BehaviorName[0x20];                         // +0x11c
};

// Factory case 0x19 (0x120, ctor the routine, vtable 06fb9098; no [ObjList] key, Parse is
// the base's).
class FObjListTractorBeam : public FObjListDirective
{
public:
    bool Execute(FContentShipBase* Ship) override;
    int GetType() override;                          // 0x19
    // (RET 8; caller TU37 06f0ca4d).
    void SetTarget(unsigned int TargetId, bool Field10);

    pub::AI::DirectiveTractorBeamOp Op;              // +0x10c
};

// baseline reads some directive members by raw offset, choosing the offset only by the
// directive's GetType() code (the NPC ship's comm-line-done the routine: 3/4/5 -> +0x17c,
// 7 -> +0x10c, 8/9 -> +0x1c8). GetType 5 is shared by GotoVec, Guide and GuideShip (all
// three vtables' slot +0x14 are 06ee99a0), so the +0x17c read also lands on the Guide ops'
// GotoParams.Field_64. These asserts pin the offsets that read depends on.
static_assert(offsetof(FObjListGotoShip, Field_0x17c) == 0x17c, "GotoShip +0x17c");
static_assert(offsetof(FObjListGotoSpline, Field_0x17c) == 0x17c, "GotoSpline +0x17c");
static_assert(offsetof(FObjListGotoVec, Field_0x17c) == 0x17c, "GotoVec +0x17c");
static_assert(offsetof(FObjListFollow, Field_0x10c) == 0x10c, "Follow +0x10c");
static_assert(offsetof(FObjListGuide, Op) + offsetof(pub::AI::DirectiveGuideOp, GotoParams) +
                  offsetof(pub::AI::DirectiveGotoOp, Field_64) == 0x17c,
              "Guide +0x17c is GotoParams.Field_64");
static_assert(offsetof(FObjListGuideShip, Op) + offsetof(pub::AI::DirectiveGuideOp, GotoParams) +
                  offsetof(pub::AI::DirectiveGotoOp, Field_64) == 0x17c,
              "GuideShip +0x17c is GotoParams.Field_64");
static_assert(offsetof(FObjListGuide, Field_0x1c8) == 0x1c8, "Guide +0x1c8");
static_assert(offsetof(FObjListGuideShip, Field_0x1c8) == 0x1c8, "GuideShip +0x1c8");
