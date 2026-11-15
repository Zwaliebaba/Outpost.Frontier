// Author: Giperion.

// Animation mixer: linked leaf, blend and overlay nodes evaluate channel values.

#include "x86math.h"
#include "SceneRuntime.h"

#include <cmath>
#include <cstring>
#include <new>

namespace
{
static_assert(sizeof(FQuaternion) == 4 * sizeof(float), "quaternion transport is four floats");
static_assert(sizeof(FMatrix) == 9 * sizeof(float), "matrix transport is nine floats");

enum EMixerNodeType : unsigned long
{
    MixerLeafNode = 0,
    MixerBlendNode = 1,
    MixerOverlayNode = 2
};

// FDUMP arguments of 06615553..0661556d: flags 0x100000, line 0x3D0 and baseline's __FILE__
// (module storage, read from the PE bytes; analysis truncates it in the listing).
const int G_MixerFdumpFatal = 0x100000;
const char* const G_MixerFdumpFile =
    "SceneRuntime\\Anim\\AnimComponent.cpp";

// , double 0x3F893B352540C735 (pi / 255).
const double G_MixerEaseStep = 0x1.93B352540C735p-7;

// float[256] and its ready byte the field.
float G_MixerEaseTable[0x100];
unsigned char G_bMixerEaseReady = 0;

// MSVCRT _ftol: truncation to a 64-bit integer; the callers keep EAX only.
long MixerFtol(double Value)
{
    return (long)((long long)(Value));
}

// The joint-type table the routine (0661558e..066155c0) and the routine (0661d68e..0661d6c0)
// share: IEngine+0xC4's first dword to the number of floats the joint's parameters take.
unsigned long MixerJointValueCount(unsigned long JointType)
{
    switch (JointType)
    {
    case 1:
    case 2:
        return 1;
    case 3:
        return 2;
    case 4:
        return 4;
    case 5:
        return 3;
    case 6:
        return 7;
    default:
        return 0;
    }
}

// cdecl: the ease table, built on first use, indexed without a range check.
float MixerEase(long Index)
{
    if (G_bMixerEaseReady == 0)                                  // ..58
    {
        long Step = 0;
        while (Step < 0x100)                                     // ..8d
        {
            // FILD i; FMUL qword K; FCOS; FSUBR qword 1.0; FMUL qword 0.5; FSTP dword.
            G_MixerEaseTable[Step] = (float)(
                (1.0 - std::cos((double)(Step) * G_MixerEaseStep)) * 0.5);
            Step += 1;
        }
        G_bMixerEaseReady = 1;                                   
    }
    return G_MixerEaseTable[Index];                              // / 0661d0a7
}

// thiscall RET 4. No bound check in baseline.
void MixerStackPush(FMixerStack* Stack, FMixerNode* Node)
{
    Stack->Nodes[Stack->Count] = Node;
    Stack->Count += 1;
}

// 0 when empty.
FMixerNode* MixerStackPop(FMixerStack* Stack)
{
    if (Stack->Count == 0)
        return nullptr;
    Stack->Count -= 1;
    return Stack->Nodes[Stack->Count];
}

// thiscall RET 8 (Out, Kind): pop, then `JMP [EDX+4]` into the node's Evaluate
// after overwriting the Kind slot with the stack pointer (0661d0fb). Kind is never read.
void MixerStackEvaluate(FMixerStack* Stack, float* Out, unsigned long Kind)
{
    (void)Kind;
    FMixerNode* Node = MixerStackPop(Stack);
    if (Node == nullptr)                                         // TEST EAX,EAX / JZ
        return;
    Node->Evaluate(Out, Stack);
}

// cdecl (Out, A, B, T): vec3 lerp, then 3DMath +0x58 slerp of the quaternions at
// +0xC into a local, copied to Out+3.
void MixerBlendPosQuat(float* Out, const float* A, const float* B, float T)
{
    float Local[4];
    // FLD B; FSUB A; FMUL T; FADD A; FSTP dword -- per component (0661d474..0661d4ad).
    Local[0] = (float)(((double)(B[0]) - A[0]) * T + A[0]);
    Local[1] = (float)(((double)(B[1]) - A[1]) * T + A[1]);
    Local[2] = (float)(((double)(B[2]) - A[2]) * T + A[2]);
    Out[0] = Local[0];
    Out[1] = Local[1];
    Out[2] = Local[2];
    I3DMathEngine* Math = Acquire3DMathEngine();                 
    Math->QuatSlerp(*(FQuaternion*)(Local),
                    *(const FQuaternion*)(A + 3),
                    *(const FQuaternion*)(B + 3), T); // ..e0
    Out[3] = Local[0];                                           
    Out[4] = Local[1];
    Out[5] = Local[2];
    Out[6] = Local[3];
}

// cdecl (Out, A, B, T): copy both quaternions to locals, slerp into a third
// local, copy that to Out.
void MixerSlerpQuat(float* Out, const float* A, const float* B, float T)
{
    FQuaternion LocalB;
    FQuaternion LocalA;
    FQuaternion Result;
    std::memcpy(&LocalB, B, sizeof(LocalB));                      // ..32
    std::memcpy(&LocalA, A, sizeof(LocalA));                      // ..54
    I3DMathEngine* Math = Acquire3DMathEngine();                 
    Math->QuatSlerp(Result, LocalA, LocalB, T); // ..7b
    std::memcpy(Out, &Result, sizeof(Result));                    // ..9a
}

// Tail append, inline in the routine (0661623c..4a, 0661625a..7b, 066162dd..0661630) and
// (066154ad..d0).
void MixerAppend(FMixerList* List, FMixerNode* Node)
{
    FMixerNode* Tail = List->Tail;
    List->Tail = Node;
    if (Tail == nullptr)
        List->Head = Node;
    else
        Tail->Next = Node;
    Node->Next = nullptr;
    Node->Prev = Tail;
    List->Count += 1;
}

// Insert before Where, inline in the routine (06615495..aa) and the routine
// (06615f60..8a, 06615fee..0661601). Where may be null there: the node then becomes the tail.
void MixerInsertBefore(FMixerList* List, FMixerNode* Where, FMixerNode* Node)
{
    FMixerNode* Before = nullptr;
    if (Where == nullptr)
    {
        Before = List->Tail;
        List->Tail = Node;
    }
    else
    {
        Before = Where->Prev;
        Where->Prev = Node;
    }
    if (Before == nullptr)
        List->Head = Node;
    else
        Before->Next = Node;
    Node->Next = Where;
    Node->Prev = Before;
    List->Count += 1;
}

// `OR dword [typed+0x34],2` on a leaf that is about to be dropped (0661539e..a3 and siblings).
void MixerMarkDropped(FMixerNode* Node)
{
    if (Node->Type == MixerLeafNode)
        ((FMixerLeaf*)(Node))->Typed->PlayFlags |= 2;
}
} // namespace

// ---------------------------------------------------------------------------------------------
// Node methods
// ---------------------------------------------------------------------------------------------

// `MOV ECX,[ECX+0x14]; JMP the routine`.
unsigned long FMixerLeaf::Tick(float Dt)
{
    return TypedTick(Typed, Dt);
}

// RET 8: typed slot 0x18 (Out). The stack argument is not read.
void FMixerLeaf::Evaluate(float* Out, FMixerStack* Stack)
{
    (void)Stack;
    Typed->CopySampled(Out);
}

// `MOV ECX,[ECX+0x14]; JMP [EAX+0x34]`.
void FMixerLeaf::UpdateDeltaRate(float Dt) // formerly Slot02
{
    Typed->UpdateDeltaRate(Dt);
}

// RET 8: typed slot 0x38 (Out, Dt), returns Out.
float* FMixerLeaf::SampleDisplacement(float* Out, float Dt) // formerly Slot03
{
    Typed->Step(Out, Dt);
    return Out;
}

// RET 4: FLD Dt; FADD [Time]; FST [Time]; FCOMP [Duration]; AND AX,0x100 (C0);
// JNZ -> 0. C0 is set for "less" and for unordered, so only Sum >= Duration answers 1.
unsigned long FMixerBlend::Tick(float Dt)
{
    const double Sum = (double)(Dt) + (double)(Time);
    Time = (float)(Sum);
    if (Sum >= (double)(Duration))
        return 1;
    return 0;
}

// RET 8.
void FMixerBlend::Evaluate(float* Out, FMixerStack* Stack)
{
    FMixerNode* Right = MixerStackPop(Stack);                    // -> EBP
    FMixerNode* Left = MixerStackPop(Stack);                     // -> EDI
    float LeftValue[7];
    float RightValue[7];
    Left->Evaluate(LeftValue, Stack);                            // ..87
    Right->Evaluate(RightValue, Stack);                          // ..95

    // FLD [Duration]; FCOMP 0.0f; AND AX,0x4100; JNZ -> 0xFF. The mask is C3|C0, the jump is
    // taken for "equal", "less" and unordered, so only Duration > 0 computes an index.
    long WeightIndex = 0xFF;                                     
    if (Duration > 0.0f)
    {
        // FLD Inv; FMUL Time; FMUL 255.0f; FSTP qword; floor; _ftol (0661d1aa..c5).
        WeightIndex = MixerFtol(std::floor((double)(InvDuration) *
                                           (double)(Time) * 255.0));
    }
    const float Weight = MixerEase(WeightIndex);                 // ..dd FSTP dword

    // ..f2: the right-hand node if it is a leaf, else the left-hand one if that is.
    FMixerNode* Leaf = nullptr;
    if (Right->Type == MixerLeafNode)
        Leaf = Right;
    else if (Left->Type == MixerLeafNode)
        Leaf = Left;

    if (Leaf != nullptr)
    {
        FTypedChannel* Typed = ((FMixerLeaf*)(Leaf))->Typed;
        // typed slot 0x0C (Out, Left, Right, (double)Weight) -- 0661d1fc..1a.
        Typed->InterpolatePose(Out, LeftValue, RightValue,
                                             (double)(Weight));
        if ((Flags & 2) == 0)                                    
            return;

        // ..47: one axis of the result survives the pose curve, picked by the
        // channel's LoopFlags bits 3/4/5.
        const unsigned long Axis = Typed->LoopFlags;
        float Kept = 0.0f;
        if ((Axis & 0x8) != 0)
            Kept = Out[0];
        else if ((Axis & 0x10) != 0)
            Kept = Out[1];
        else if ((Axis & 0x20) != 0)
            Kept = Out[2];

        // , the rounding points as the listing has them.
        const double Inv = (double)(InvDuration);
        const double Now = (double)(Time);
        const double Ctrl0 = (double)(PoseCtrl[0]) * 0.5;                  // ext
        const double Ctrl1 = (double)(PoseCtrl[1]) * 0.5;                  // ext
        const float Ctrl2 = (float)((double)(PoseCtrl[2]) * 0.5); // [0x18]
        const float Ctrl0Inv = (float)(Ctrl0 * Inv);                       // [0x1c]
        const double Ctrl1Inv = Ctrl1 * Inv;                                          // ext
        const double Ctrl2InvNow = (double)(Ctrl2) * Inv * Now;            // ext
        const float Term0 = (float)((double)(Ctrl0Inv) * Now);  // [0x10]
        const float Term1 = (float)(Ctrl1Inv * Now);                       // [0x14]
        const double Rate0 = (double)(Term0) + PoseRate[0];                // ext
        const float Rate1 = (float)((double)(Term1) + PoseRate[1]); // [0x14]
        const float Rate2 = (float)(Ctrl2InvNow + PoseRate[2]);            // [0x18]
        const double Pos0 = Rate0 * Now;                                              // ext
        const double Pos1 = (double)(Rate1) * Now;                         // ext
        const float Pos2 = (float)((double)(Rate2) * Now);      // [0x24]
        Out[0] = (float)(Pos0 + PoseStart[0]);
        Out[1] = (float)(Pos1 + PoseStart[1]);
        Out[2] = (float)((double)(Pos2) + PoseStart[2]);

        // put the kept axis back. With none of the three bits set baseline
        // pops the value it loaded (0661d42a FSTP ST0).
        if ((Axis & 0x8) != 0)
            Out[0] = Kept;
        else if ((Axis & 0x10) != 0)
            Out[1] = Kept;
        else if ((Axis & 0x20) != 0)
            Out[2] = Kept;
        return;
    }

    // ..63: two overlays, switched on the left one's value count.
    if (Left->Type != MixerOverlayNode || Right->Type != MixerOverlayNode)
        return;
    const FMixerOverlay* LeftOverlay = (const FMixerOverlay*)(Left);
    const FMixerOverlay* RightOverlay = (const FMixerOverlay*)(Right);
    const double W = (double)(Weight);
    switch (LeftOverlay->Count)
    {
    case 1:                                                      
        Out[0] = (float)(
            ((double)(RightOverlay->Values[0]) - LeftOverlay->Values[0]) * W +
            LeftOverlay->Values[0]);
        break;
    case 3:                                                      
    {
        float Lerped[3];
        unsigned long Index = 0;
        while (Index < 3)
        {
            Lerped[Index] = (float)(
                ((double)(RightOverlay->Values[Index]) - LeftOverlay->Values[Index]) *
                    W +
                LeftOverlay->Values[Index]);
            Index += 1;
        }
        Out[0] = Lerped[0];
        Out[1] = Lerped[1];
        Out[2] = Lerped[2];
        break;
    }
    case 4:                                                      
        MixerSlerpQuat(Out, LeftOverlay->Values, RightOverlay->Values, Weight);
        break;
    case 7:                                                      
        MixerBlendPosQuat(Out, LeftOverlay->Values, RightOverlay->Values, Weight);
        break;
    default:                                                     // 2, 5, 6 and out of range
        break;
    }
}

// `RET 4`.
void FMixerBlend::UpdateDeltaRate(float Dt) // formerly Slot02
{
    (void)Dt;
}

// RET 8: the predecessor's slot 3 with the same arguments, returns Out.
float* FMixerBlend::SampleDisplacement(float* Out, float Dt) // formerly Slot03
{
    Prev->SampleDisplacement(Out, Dt);
    return Out;
}

// thiscall RET 4.
FMixerOverlay::FMixerOverlay(const FAnimBindOut* Desc)
{
    Next = nullptr;                                              
    Prev = nullptr;                                              
    bRemove = 0;                                                 
    Type = MixerOverlayNode;                                                    
    if (Desc->Kind == 0)                                         // -> 0661d678
    {
        const unsigned long* Relative = G_AnimEngine->GetInstanceRelative(Desc->Instance);
        if (Relative != nullptr)                                 
        {
            Count = MixerJointValueCount(Relative[0]);           // ..c5
            G_AnimEngine->GetJointParams(Desc->Instance, 0, Values); // ..d6
        }
        // Relative == null leaves Count as operator new handed it over (0661d68c -> 0661d6dc);
        // baseline writes nothing there either.
        return;
    }
    if (Desc->Kind == 1)                                         // -> 0661d5ef
    {
        Count = 7;
        const float* Position = G_AnimEngine->GetInstancePosition(Desc->Instance); // IEngine+0x90
        Values[0] = Position[0];
        Values[1] = Position[1];
        Values[2] = Position[2];
        const float* Orientation = G_AnimEngine->GetInstanceOrientation(Desc->Instance); // +0x98
        I3DMathEngine* Math = Acquire3DMathEngine();             
        FQuaternion Quat;
        Math->MatrixToQuaternion(Quat,
                                 *(const FMatrix*)(Orientation)); // +0x50
        Values[3] = Quat.W;
        Values[4] = Quat.X;
        Values[5] = Quat.Y;
        Values[6] = Quat.Z;
        return;
    }
    Count = 0;                                                   
}

// `XOR EAX,EAX; RET 4`.
unsigned long FMixerOverlay::Tick(float Dt)
{
    (void)Dt;
    return 0;
}

// RET 8: REP MOVSD of Count dwords from +0x18 to Out.
void FMixerOverlay::Evaluate(float* Out, FMixerStack* Stack)
{
    (void)Stack;
    if (Count != 0)
        std::memcpy(Out, Values, Count * sizeof(float));
}

// `RET 4`.
void FMixerOverlay::UpdateDeltaRate(float Dt) // formerly Slot02
{
    (void)Dt;
}

// RET 8: zero three floats, return Out.
float* FMixerOverlay::SampleDisplacement(float* Out, float Dt) // formerly Slot03
{
    (void)Dt;
    Out[0] = 0.0f;
    Out[1] = 0.0f;
    Out[2] = 0.0f;
    return Out;
}

// thiscall RET 4 (this = Left).
bool FAnimBindLess::operator()(const FAnimBindOut& Left, const FAnimBindOut& Right) const
{
    if (Left.Kind != Right.Kind)                                 // ..3b SETL: signed
        return (long)(Left.Kind) < (long)(Right.Kind);
    if (Left.Kind == 0 || Left.Kind == 1)                        // / 06612b49: SETL
        return (ptrdiff_t)(Left.Instance) < (ptrdiff_t)(Right.Instance);
    if (Left.Kind == 2)                                          // ..6a: SBB, unsigned
        return (size_t)(Left.Extra) < (size_t)(Right.Extra);
    return false;                                                
}

// ---------------------------------------------------------------------------------------------
// List primitives
// ---------------------------------------------------------------------------------------------

// operator delete. The node classes have no destructor in baseline's vtables.
void MixerFreeNode(FMixerNode* Node)
{
    // Nodes have no virtual destructor; free storage without base destruction.
    ::operator delete(Node);
}

// thiscall RET 4.
void MixerUnlink(FMixerList* List, FMixerNode* Node)
{
    if (Node->Prev == nullptr)
        List->Head = Node->Next;
    else
        Node->Prev->Next = Node->Next;
    if (Node->Next == nullptr)
        List->Tail = Node->Prev;
    else
        Node->Next->Prev = Node->Prev;
    List->Count -= 1;
}

// thiscall RET 4.
void MixerUnlinkAndFree(FMixerList* List, FMixerNode* Node)
{
    MixerUnlink(List, Node);
    MixerFreeNode(Node);
}

// the head is read once, then each successor is taken before its node goes.
void MixerFreeList(FMixerList* List)
{
    FMixerNode* Node = List->Head;
    while (List->Count != 0)
    {
        FMixerNode* Next = Node->Next;
        MixerUnlink(List, Node);
        MixerFreeNode(Node);
        Node = Next;
    }
}

// thiscall RET 8 (Blend, bOuter): drop the blend, what it was fading from and,
// with bOuter, what it was fading to. A blend two back is unwound the same way, recursively.
void MixerUnlinkParentBlend(FMixerList* List, FMixerNode* Node, unsigned char bOuter)
{
    FMixerNode* Inner = Node->Prev;                              // ESI
    FMixerNode* Outer = Inner->Prev;                             // EAX
    if (Outer->Type == MixerBlendNode)                                        
        MixerUnlinkParentBlend(List, Outer, 1);
    else
    {
        MixerMarkDropped(Outer);                                 // ..19
        MixerUnlink(List, Outer);                                // ..4b
        MixerFreeNode(Outer);                                    
    }
    if (bOuter != 0)                                             
    {
        MixerMarkDropped(Inner);                                 // ..68
        MixerUnlink(List, Inner);
        MixerFreeNode(Inner);                                    
    }
    MixerUnlink(List, Node);                                     // ..d3
    MixerFreeNode(Node);                                         
}

// ---------------------------------------------------------------------------------------------
// Maps
// ---------------------------------------------------------------------------------------------

// lower_bound on the signed key plus the equality check; null on a miss.
FMixerDescMap* MixerFindKey(FAnimationObject* Animation, long Key)
{
    FMixerKeyMap::iterator It = Animation->Mixers->find(Key);
    if (It == Animation->Mixers->end())
        return nullptr;
    return &It->second;
}

// find-or-insert, returns the inner map.
FMixerDescMap* MixerFindOrAddKey(FAnimationObject* Animation, long Key)
{
    return &(*Animation->Mixers)[Key];
}

// The +0x3C half of the routine: the routine erases every outer node, which runs every
// list's destructor the routine; the map object itself goes with the Animation object.
void MixerDestroyAll(FAnimationObject* Animation)
{
    FMixerKeyMap::iterator Outer = Animation->Mixers->begin();
    while (Outer != Animation->Mixers->end())
    {
        FMixerDescMap::iterator Entry = Outer->second.begin();
        while (Entry != Outer->second.end())
        {
            MixerFreeList(&Entry->second);
            ++Entry;
        }
        ++Outer;
    }
    delete Animation->Mixers;
    Animation->Mixers = nullptr;
}

void MixerWalkPlayback(FAnimationObject* Animation, const FAnimBindOut* Desc, FMixerList* List,
                       float Dt)
{
    (void)Animation;

    FMixerNode* Snapshot[0x140];
    long SnapshotCount = 0;
    FMixerNode* Cursor = List->Head;
    while (Cursor != nullptr)
    {
        Snapshot[SnapshotCount] = Cursor;
        SnapshotCount += 1;
        Cursor = Cursor->Next;
    }

    long Index = 0;
    while (Index < SnapshotCount)                                
    {
        FMixerNode* Node = Snapshot[Index];
        if (Node->Tick(Dt) == 1)                                 // ..2f
        {
            if (Node->Type == MixerBlendNode)                                 // ..3a
            {
                FMixerBlend* Blend = (FMixerBlend*)(Node);
                if ((Blend->Flags & 2) != 0)                     // ..6b
                {
                    FTypedChannel* PrevTyped = ((FMixerLeaf*)(Blend->Prev))->Typed;
                    const float* Position = G_AnimEngine->GetInstancePosition(Desc->Instance);
                    PrevTyped->SetOverlayPos(Position);
                }
                if ((Blend->Flags & 1) != 0)                     
                {
                    FMixerNode* Outer = Blend->Prev->Prev;       // ..7b
                    if (Outer->Type == MixerBlendNode)                        // ..97
                    {
                        MixerUnlinkParentBlend(List, Outer, 1);
                        MixerUnlinkAndFree(List, Node);
                    }
                    else                                         // ..c0
                    {
                        MixerMarkDropped(Outer);
                        MixerUnlink(List, Outer);
                        MixerFreeNode(Outer);
                        MixerUnlinkAndFree(List, Node);
                    }
                }
                else                                             // ..f8
                {
                    MixerUnlink(List, Node);
                    MixerFreeNode(Node);
                }
            }
            else if (Node->Type == MixerLeafNode)                            // ..c7
            {
                FMixerLeaf* Leaf = (FMixerLeaf*)(Node);
                FMixerNode* Next = Node->Next;
                if (Next != nullptr)                             // ..e5
                {
                    if (Next->Type == MixerBlendNode || (Next->Next != nullptr && Next->Next->Type == MixerBlendNode))
                        TypedApplyFromArchetype(Leaf->Typed);    
                    else
                    {
                        MixerUnlink(List, Node);                 // ..f5
                        MixerFreeNode(Node);
                    }
                }
                else
                {
                    FMixerNode* Before = Node->Prev;             
                    if (Before == nullptr)                       // ..dd
                    {
                        TypedApplyFromArchetype(Leaf->Typed);
                        Node->bRemove = 1;
                    }
                    else
                    {
                        // ..59: a blend back from this leaf to what precedes it.
                        FMixerBlend* Back = new FMixerBlend;
                        Back->Next = nullptr;
                        Back->Duration = List->BlendDuration;
                        Back->Prev = nullptr;
                        Back->Time = 0.0f;
                        Back->bRemove = 0;
                        Back->Type = MixerBlendNode;
                        // (byte & 0xFD) | 1 on memory operator new did not clear: bit 1 off,
                        // bit 0 on; bits 2..7 are never read, so the port stores them as zero.
                        Back->Flags = 1;
                        Back->InvDuration = (float)(1.0 / (double)(Back->Duration));
                        TypedApplyFromArchetype(Leaf->Typed);    // ..5f
                        MixerUnlink(List, Node);                 // ..92
                        MixerInsertBefore(List, Before, Node);   // ..b9
                        MixerAppend(List, Back);                 // ..d0
                    }
                }
            }
        }
        Index += 1;
    }

    // every node, head first, onto a zeroed stack.
    FMixerStack Stack;
    std::memset(&Stack, 0, sizeof(Stack));
    Cursor = List->Head;
    while (Cursor != nullptr)
    {
        MixerStackPush(&Stack, Cursor);
        Cursor = Cursor->Next;
    }

    if (Stack.Count != 0)                                        // ..33
    {
        unsigned long Kind = 0;
        if (Desc->Kind == 0)                                     // ..94
        {
            const unsigned long* Relative = G_AnimEngine->GetInstanceRelative(Desc->Instance);
            if (Relative != nullptr)
                Kind = MixerJointValueCount(Relative[0]);
        }
        else if (Desc->Kind == 1)                                // -> 066155b9
            Kind = 7;
        else if (Desc->Kind == 2)                                // -> 066155c0
            Kind = 0;
        else                                                     // ..76
        {
            SceneRuntimeFdump(G_MixerFdumpFatal, G_MixerFdumpFile, 0x3D0, "FATAL",
                         "Unknown data type!\n");
            Kind = (unsigned long)(Index);            // the loop counter
        }

        float Out[7];
        MixerStackEvaluate(&Stack, Out, Kind);                   // ..cf

        if (Desc->Kind == 0)                                     // ..44
            G_AnimEngine->SetJointParams(Desc->Instance, 0, Out);
        else if (Desc->Kind == 1)                                // ..2f
        {
            G_AnimEngine->SetInstancePosition(Desc->Instance, Out);        // IEngine+0x8C
            FMatrix Matrix;
            I3DMathEngine* Math = Acquire3DMathEngine();                   
            Math->QuaternionToMatrix(Matrix,
                                     *(const FQuaternion*)(Out + 3)); // +0x54
            G_AnimEngine->SetInstanceOrientation(Desc->Instance, &Matrix.D[0][0]); // IEngine+0x94
        }
    }

    // drop the nodes flagged in the first pass.
    FMixerNode* Walk = List->Head;
    while (Walk != nullptr)
    {
        FMixerNode* Next = Walk->Next;
        if (Walk->bRemove != 0)
        {
            MixerUnlink(List, Walk);
            MixerFreeNode(Walk);
        }
        Walk = Next;
    }
}

void MixerBlendAttach(FAnimationObject* Animation, FTypedChannel* Typed, float Duration,
                      unsigned long Flags)
{
    FMixerDescMap* Inner = MixerFindOrAddKey(Animation, TypedMixerKey(Typed)); // ..9f
    if (Inner == nullptr)                                        // baseline tests it
        return;

    FAnimBindOut* Desc = TypedBindDesc(Typed);
    FMixerDescMap::iterator Entry = Inner->find(*Desc);          // ..bb the routine
    if (Entry == Inner->end())                                   // ..c9
    {
        // insert {desc, empty list}. The list's +0xC word is copied from
        // an uninitialised stack slot (06616104 local_24); the port starts it at 0.
        FMixerList Empty;
        Empty.Head = nullptr;
        Empty.Tail = nullptr;
        Empty.Count = 0;
        Empty.BlendDuration = 0.0f;
        Entry = Inner->insert(FMixerDescMap::value_type(*Desc, Empty)).first;
    }
    FMixerList* List = &Entry->second;

    FMixerNode* Walk = List->Head;                               // ..5e
    while (Walk != nullptr)
    {
        if (Walk->Type == MixerLeafNode && ((FMixerLeaf*)(Walk))->Typed == Typed)
            return;
        Walk = Walk->Next;
    }

    FMixerNode* Anchor = nullptr;                                // local_48
    unsigned char bBlend = 0;                                    // local_4d
    // FLD Duration; FCOMP 0.0f; AND AX,0x4100 (C3|C0); JNZ skips the blend for "equal",
    // "less" and unordered -- only Duration > 0 falls through.
    if (Duration > 0.0f)
    {
        bBlend = 1;                                              
        if (List->Count == 0)                                    // ..81
        {
            Flags &= ~0x80UL;                                    // ..8f AND AL,0x7F
            FMixerOverlay* Overlay = new FMixerOverlay(&Entry->first); // ..ad
            MixerAppend(List, Overlay);                          // ..4a
            Anchor = Overlay;
        }
        else
            Anchor = List->Tail;                                 
    }
    else
    {
        if (List->Count != 0)                                    // ..bd
        {
            Walk = List->Head;                                   // ..d6
            while (Walk != nullptr)
            {
                MixerMarkDropped(Walk);
                Walk = Walk->Next;
            }
            Walk = List->Head;                                   // ..fc
            while (List->Count != 0)
            {
                FMixerNode* Next = Walk->Next;
                MixerUnlink(List, Walk);
                MixerFreeNode(Walk);
                Walk = Next;
            }
        }
    }

    FMixerLeaf* Leaf = new FMixerLeaf;                           // ..25
    Leaf->Typed = Typed;
    Leaf->Next = nullptr;
    Leaf->Prev = nullptr;
    Leaf->bRemove = 0;
    Leaf->Type = MixerLeafNode;
    MixerAppend(List, Leaf);                                     // ..7b
    if (bBlend == 0)                                             // ..7e
        return;

    FMixerBlend* Blend = new FMixerBlend;                        // ..ca
    Blend->InvDuration = (float)(1.0 / (double)(Duration));
    Blend->Next = nullptr;
    Blend->Prev = nullptr;
    Blend->bRemove = 0;
    Blend->Type = MixerBlendNode;
    Blend->Time = 0.0f;
    Blend->Duration = Duration;
    // (byte & 0xFD) | 1 on uncleared memory; bits 2..7 are never read, stored as zero here.
    Blend->Flags = 1;
    if ((signed char)(Flags & 0xFF) < 0)              // ..d9 TEST CL,CL / JNS
        Blend->Flags &= 0xFE;
    MixerAppend(List, Blend);                                    
    List->BlendDuration = Duration;                              

    if (Typed->Kind != 1)                                        // ..13
        return;
    if ((Flags & 0x38) == 0)                                     // TEST CL,0x38
        return;

    float Predicted[3];
    Leaf->SampleDisplacement(Predicted, Duration);
    Blend->Flags |= 2;                                           // ..3d
    const float* Start = G_AnimEngine->GetInstancePosition((void*)(Typed->Field14));
    Blend->PoseStart[0] = Start[0];                              // ..67
    Blend->PoseStart[1] = Start[1];
    Blend->PoseStart[2] = Start[2];
    float AnchorOut[3];
    const float* Rate = Anchor->SampleDisplacement(AnchorOut, Duration);
    Blend->PoseRate[0] = Rate[0];                                // ..8e
    Blend->PoseRate[1] = Rate[1];
    Blend->PoseRate[2] = Rate[2];

    // .
    const double Inv = (double)(Blend->InvDuration);
    Blend->PoseRate[0] = (float)(Inv * Blend->PoseRate[0]);
    Blend->PoseRate[1] = (float)(Inv * Blend->PoseRate[1]);
    Blend->PoseRate[2] = (float)(Inv * Blend->PoseRate[2]);
    const float Scaled1 = (float)((double)(Predicted[1]) * Inv); // [0x34]
    const float Scaled2 = (float)((double)(Predicted[2]) * Inv); // [0x38]
    Blend->PoseCtrl[0] =
        (float)((double)(Predicted[0]) * Inv - Blend->PoseRate[0]);
    Blend->PoseCtrl[1] = (float)((double)(Scaled1) - Blend->PoseRate[1]);
    Blend->PoseCtrl[2] = (float)((double)(Scaled2) - Blend->PoseRate[2]);
}

void MixerUnbindTyped(FAnimationObject* Animation, FTypedChannel* Typed)
{
    FMixerDescMap* Inner = MixerFindKey(Animation, TypedMixerKey(Typed)); // ..d8
    if (Inner == nullptr)
        return;
    FMixerDescMap::iterator Entry = Inner->find(*TypedBindDesc(Typed));  // ..f7
    if (Entry == Inner->end())
        return;
    FMixerList* List = &Entry->second;

    FMixerNode* Leaf = List->Head;                               // ..1e
    while (Leaf != nullptr)
    {
        if (Leaf->Type == MixerLeafNode && ((FMixerLeaf*)(Leaf))->Typed == Typed)
            break;
        Leaf = Leaf->Next;
    }

    if (Leaf != nullptr)
    {
        FMixerNode* After = Leaf->Next;                          // EBP
        if (After != nullptr && After->Next != nullptr && After->Next->Type == MixerBlendNode)
        {
            // ..9f: the leaf is what a later blend fades FROM. Freeze its value in an
            // overlay where the leaf was and let that blend drop the overlay when done.
            FMixerOverlay* Overlay = new FMixerOverlay(&Entry->first);
            MixerInsertBefore(List, Leaf->Next, Overlay);
            if (Overlay->Next != nullptr)                        // ..92
                ((FMixerBlend*)(Overlay->Next->Next))->Flags |= 1; // ..9b
        }
        else if (After != nullptr && After->Type == MixerBlendNode)           // ..b4
        {
            // the leaf is what After fades TO.
            FMixerNode* AfterNext = After->Next;
            if (AfterNext != nullptr && AfterNext->Next != nullptr && AfterNext->Next->Type == MixerBlendNode)
            {
                FMixerOverlay* Overlay = new FMixerOverlay(&Entry->first);
                MixerInsertBefore(List, After->Next, Overlay);
            }
            FMixerNode* Outer = After->Prev->Prev;               // ..1a (no null check)
            if (Outer->Type == MixerBlendNode)                                // ..2e
                MixerUnlinkParentBlend(List, Outer, 1);
            else                                                 // ..49
            {
                MixerMarkDropped(Outer);
                MixerUnlink(List, Outer);
                MixerFreeNode(Outer);
            }
            MixerUnlinkAndFree(List, After);                     // ..4f
        }
        MixerUnlink(List, Leaf);                                 // ..57
        MixerFreeNode(Leaf);                                     
    }

    if (List->Count == 0)                                        // ..7e the routine
        Inner->erase(Entry);
}

// 06616928..066169ac, after the channel key has been resolved.
unsigned char MixerHasLeaf(FAnimationObject* Animation, FTypedChannel* Typed)
{
    FMixerDescMap* Inner = MixerFindKey(Animation, TypedMixerKey(Typed));
    if (Inner == nullptr)
        return 0;
    FMixerDescMap::iterator Entry = Inner->find(*TypedBindDesc(Typed)); // ..81
    if (Entry == Inner->end())
        return 0;
    FMixerNode* Node = Entry->second.Head;                       // ..9b
    while (Node != nullptr)
    {
        if (Node->Type == MixerLeafNode && ((FMixerLeaf*)(Node))->Typed == Typed)
            return 1;
        Node = Node->Next;
    }
    return 0;
}

// RET 0xC (this, Key, Dt), after the receiver.
void MixerWalkKey(FAnimationObject* Animation, long Key, float Dt)
{
    if (Key == -1)                                               
        return;
    FMixerDescMap* Inner = MixerFindKey(Animation, Key);         // ..20
    if (Inner == nullptr)
        return;
    FMixerDescMap::iterator Entry = Inner->begin();              // ..8f
    while (Entry != Inner->end())
    {
        MixerWalkPlayback(Animation, &Entry->first, &Entry->second, Dt);
        if (Entry->second.Count == 0)
            Entry = Inner->erase(Entry);                         // ..58 the routine
        else
            ++Entry;
    }
    if (Inner->empty())                                          // ..dd
        Animation->Mixers->erase(Key);
}

// RET 8 (this, Dt), after the receiver.
void MixerWalkAll(FAnimationObject* Animation, float Dt)
{
    // ..30: clear bit 2 ("ticked this frame") on every channel.
    FTypedChannelSet::iterator Channel = Animation->Channels->begin();
    while (Channel != Animation->Channels->end())
    {
        (*Channel)->PlayFlags &= 0xFFFFFFFBu;
        ++Channel;
    }

    // ..dd: walk every list; drop lists that emptied, then roots with no lists left.
    FMixerKeyMap::iterator Outer = Animation->Mixers->begin();
    while (Outer != Animation->Mixers->end())
    {
        FMixerDescMap& Inner = Outer->second;
        FMixerDescMap::iterator Entry = Inner.begin();
        while (Entry != Inner.end())
        {
            MixerWalkPlayback(Animation, &Entry->first, &Entry->second, Dt);
            if (Entry->second.Count == 0)
                Entry = Inner.erase(Entry);                      // ..93 the routine
            else
                ++Entry;
        }
        if (Inner.empty())
            Outer = Animation->Mixers->erase(Outer);             // ..cb the routine
        else
            ++Outer;
    }

    // ..42: tick every playing channel no mixer leaf ticked; a channel that runs out
    // stops playing and leaves its mixer.
    Channel = Animation->Channels->begin();
    while (Channel != Animation->Channels->end())
    {
        FTypedChannel* Typed = *Channel;
        if ((Typed->PlayState & 1) != 0 && ((Typed->PlayFlags >> 2) & 1) == 0)
        {
            if (TypedTick(Typed, Dt) == 1)                       // ..0f
            {
                Typed->PlayState &= 0xFE;                        // ..1e
                MixerUnbindTyped(Animation, Typed);              // ..2b
            }
        }
        ++Channel;
    }
}


// thiscall RET 8 (Pos, Inner).
void PlaybackWriteOverlayPos(FAnimPlayback* Playback, const float* Pos, FMixerDescMap* Inner)
{
    unsigned long Index = 0;
    while (Index < Playback->Typed.size())                    
    {
        FTypedChannel* Typed = Playback->Typed[Index];
        Typed->SetOverlayPos(Pos);          // ..96
        if (Typed->Kind == 1)                                    // ..a2
        {
            FMixerDescMap::iterator Entry = Inner->find(*TypedBindDesc(Typed)); // ..de
            if (Entry != Inner->end())
            {
                FMixerNode* Node = Entry->second.Head;           // ..03
                while (Node != nullptr)
                {
                    if (Node->Type == MixerOverlayNode)
                    {
                        FMixerOverlay* Overlay = (FMixerOverlay*)(Node);
                        Overlay->Values[0] = Pos[0];
                        Overlay->Values[1] = Pos[1];
                        Overlay->Values[2] = Pos[2];
                    }
                    Node = Node->Next;
                }
            }
        }
        Index += 1;
    }
}

// thiscall RET 8 (Matrix, Inner): the orientation goes to the channels as the
// matrix it came in as and to the overlays as a quaternion, converted once up front.
void PlaybackWriteOverlayOrient(FAnimPlayback* Playback, const float* Matrix, FMixerDescMap* Inner)
{
    I3DMathEngine* Math = Acquire3DMathEngine();                 
    FQuaternion Quat;
    Math->MatrixToQuaternion(Quat,
                             *(const FMatrix*)(Matrix)); // ..af
    unsigned long Index = 0;
    while (Index < Playback->Typed.size())                    
    {
        FTypedChannel* Typed = Playback->Typed[Index];
        Typed->SetOverlayExtra(Matrix);       // ..cb
        if (Typed->Kind == 1)
        {
            FMixerDescMap::iterator Entry = Inner->find(*TypedBindDesc(Typed));
            if (Entry != Inner->end())
            {
                FMixerNode* Node = Entry->second.Head;           // ..3d
                while (Node != nullptr)
                {
                    if (Node->Type == MixerOverlayNode)
                    {
                        FMixerOverlay* Overlay = (FMixerOverlay*)(Node);
                        Overlay->Values[3] = Quat.W;
                        Overlay->Values[4] = Quat.X;
                        Overlay->Values[5] = Quat.Y;
                        Overlay->Values[6] = Quat.Z;
                    }
                    Node = Node->Next;
                }
            }
        }
        Index += 1;
    }
}

// thiscall RET 8 (Delta, Inner).
void PlaybackAddOverlayPos(FAnimPlayback* Playback, const float* Delta, FMixerDescMap* Inner)
{
    unsigned long Index = 0;
    while (Index < Playback->Typed.size())                    
    {
        FTypedChannel* Typed = Playback->Typed[Index];
        Typed->AddOverlayPos(Delta);        // ..85
        if (Typed->Kind == 1)
        {
            FMixerDescMap::iterator Entry = Inner->find(*TypedBindDesc(Typed));
            if (Entry != Inner->end())
            {
                FMixerNode* Node = Entry->second.Head;           // ..f9
                while (Node != nullptr)
                {
                    if (Node->Type == MixerOverlayNode)
                    {
                        FMixerOverlay* Overlay = (FMixerOverlay*)(Node);
                        Overlay->Values[0] = (float)((double)(Delta[0]) + Overlay->Values[0]);
                        Overlay->Values[1] = (float)((double)(Delta[1]) + Overlay->Values[1]);
                        Overlay->Values[2] = (float)((double)(Delta[2]) + Overlay->Values[2]);
                    }
                    Node = Node->Next;
                }
            }
        }
        Index += 1;
    }
}
