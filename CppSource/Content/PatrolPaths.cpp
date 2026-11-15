// Author: Giperion.

// Content.dll: zone paths (PatrolPaths.h), the COMDAT run 06f37110-06f38e00 at the head of
// TU45. The STL instantiations of the run (06f38120..06f38cf0: find / destroy / insert of
// the 0x14 object vector, the map ctor / insert / erase, Vector vector insert) are the std
// containers used here. Lane CNT-POP.

#include "PatrolPaths.h"

#include "ContentInternal.h"    // SubtractVectors (the routine)
#include "SystemInfo.h"
#include "common_server.h"

namespace
{
    // (cdecl): Out = A + B, each component stored as a float.
    Vector AddVectors(Vector const& A, Vector const& B)
    {
        Vector Result;
        Result.z = A.z + B.z;
        Result.y = A.y + B.y;
        Result.x = A.x + B.x;
        return Result;
    }

    // (cdecl): Out = V * (1.0f / Divisor). FST float stores the rounded
    // reciprocal but keeps the unrounded one on the x87 stack: z is scaled by the unrounded
    // value, y and x by the reloaded float.
    Vector DivideVector(Vector const& V, float Divisor)
    {
        double const ExactReciprocal = 1.0 / static_cast<double>(Divisor);
        float const Reciprocal = static_cast<float>(ExactReciprocal);
        Vector Result;
        Result.z = static_cast<float>(ExactReciprocal * V.z);
        Result.y = static_cast<float>(static_cast<double>(Reciprocal) * V.y);
        Result.x = static_cast<float>(static_cast<double>(Reciprocal) * V.x);
        return Result;
    }

    // Squared length summed x, z, y (the x87 order of 06f3741b-06f37466 and 06f3757d-06f37597).
    double LengthSquaredXZY(Vector const& V)
    {
        return static_cast<double>(V.x) * V.x + static_cast<double>(V.z) * V.z
             + static_cast<double>(V.y) * V.y;
    }

    // Linear find of the entry with ObjectId (inline in 06f379a0 and friends, the routine).
    std::vector<FPatrolPathObject>::iterator FindPathObject(std::vector<FPatrolPathObject>* Objects,
                                                            unsigned int ObjectId)
    {
        std::vector<FPatrolPathObject>::iterator It = Objects->begin();
        while (It != Objects->end() && It->ObjectId != ObjectId)
            ++It;
        return It;
    }

    // != end.
    bool ContainsIndex(std::vector<int> const& Indices, int Value)
    {
        for (std::vector<int>::const_iterator It = Indices.begin(); It != Indices.end(); ++It)
        {
            if (*It == Value)
                return true;
        }
        return false;
    }
}

// (06f37110-06f377e9, RET 0xc).
FPatrolPath::FPatrolPath(FZonePathLabel* const* First, FZonePathLabel* const* Last,
                         FSystemInfo* InSystem)
{

    System = InSystem;
    bLoop = false;
    Label.Id = (*First)->Label.Id;
    Usage = (*First)->Usage;
    bMissionEligible = (*First)->bMissionEligible;

    // the number of path indices over the whole label range.
    int IndexCount = 0;
    for (FZonePathLabel* It = *First; It != *Last; ++It)
        IndexCount += static_cast<int>(It->Indices.size());

    // the labels in path-index order; each gets its zone's two ends. A
    // label listing several indices is taken once per index. baseline does not stop at a
    // missing index: the scan ends on *Last and that record is used (06f37281 falls through
    // with EAX = the range end), exactly as here.
    std::vector<FZonePathLabel*> Steps;
    for (int Index = 1; Index <= IndexCount; ++Index)
    {
        FZonePathLabel* Step = *First;
        while (Step != *Last && !ContainsIndex(Step->Indices, Index))
            ++Step;
        FSystemZone* Zone = System->FindZone(Step->ZoneId);
        Zone->GetPathEnds(&Step->Start, &Step->End);
        Steps.insert(Steps.end(), Step);
    }

    // one step -> its two ends (the branch).
    if (Steps.begin() + 1 == Steps.end())
    {
        FZonePathLabel* Step = Steps.front();
        bLoop = false;
        Points.insert(Points.end(), 1, Step->Start);
        Points.insert(Points.end(), 1, Step->End);
    }
    else
    {
        // which end of the first step lies nearer the second step's
        // segment. FCOMP of the stored float against the x87 value, TEST AH,5 / JNP: taken
        // (start) only when less.
        FZonePathLabel* FirstStep = Steps[0];
        FZonePathLabel* SecondStep = Steps[1];
        Vector StartClosest;
        Vector EndClosest;
        float const StartDistance = static_cast<float>(DistanceSquaredToSegment(
            &FirstStep->Start, &SecondStep->Start, &SecondStep->End, &StartClosest));
        double const EndDistance = DistanceSquaredToSegment(
            &FirstStep->End, &SecondStep->Start, &SecondStep->End, &EndClosest);
        Vector const Near = (static_cast<double>(StartDistance) < EndDistance)
            ? FirstStep->Start : FirstStep->End;

        // Near equals Start when every component differs by at most 1
        // (squared, TEST AH,0x41 / JP: greater or unordered leaves). The far end is the first
        // point; Current is the near end, not yet added.
        double const DeltaX = static_cast<double>(Near.x) - FirstStep->Start.x;
        double const DeltaY = static_cast<double>(Near.y) - FirstStep->Start.y;
        double const DeltaZ = static_cast<double>(Near.z) - FirstStep->Start.z;
        Vector Current;
        if (DeltaX * DeltaX <= 1.0 && DeltaY * DeltaY <= 1.0 && DeltaZ * DeltaZ <= 1.0)
        {
            Points.insert(Points.end(), FirstStep->End);
            Current = FirstStep->Start;
        }
        else
        {
            Points.insert(Points.end(), FirstStep->Start);
            Current = FirstStep->End;
        }

        // every further step adds the midpoint between Current and its
        // nearer end; its other end becomes Current. FCOMPP |ToEnd|^2 against |ToStart|^2,
        // TEST AH,0x41 / JNZ: the start is taken only when strictly nearer.
        for (std::vector<FZonePathLabel*>::iterator It = Steps.begin() + 1; It != Steps.end(); ++It)
        {
            FZonePathLabel* Step = *It;
            Vector const ToStart = SubtractVectors(Current, Step->Start);
            Vector const ToEnd = SubtractVectors(Current, Step->End);
            Vector Joint;
            if (LengthSquaredXZY(ToEnd) > LengthSquaredXZY(ToStart))
            {
                Joint = DivideVector(AddVectors(Current, Step->Start), 2.0f);
                Current = Step->End;
            }
            else
            {
                Joint = DivideVector(AddVectors(Current, Step->End), 2.0f);
                Current = Step->Start;
            }
            Points.insert(Points.end(), 1, Joint);
        }

        // a path whose last end lies within 5000 of its first point
        // (the field = 25e6, TEST AH,5 / JP: only less falls through) is a loop; both
        // ends become their midpoint (float adds, then * 0.5f the field).
        // here the difference is inlined (FLD/FSUB/FSTP z, y, x) and the
        // result built by the routine -- not a CALL of the routine.
        Vector Gap;
        Gap.z = Points.front().z - Current.z;
        Gap.y = Points.front().y - Current.y;
        Gap.x = Points.front().x - Current.x;
        if (LengthSquaredXZY(Gap) < 25000000.0)
        {
            bLoop = true;
            Vector const Sum = AddVectors(Current, Points.front());
            Vector Middle;
            Middle.x = Sum.x * 0.5f;
            Middle.y = Sum.y * 0.5f;
            Middle.z = Sum.z * 0.5f;
            Points.insert(Points.end(), 1, Middle);
            Points.front() = Middle;
        }
        else
        {
            bLoop = false;
            Points.insert(Points.end(), 1, Current);
        }
    }

    // every step index under its zone.
    int StepIndex = 0;
    for (std::vector<FZonePathLabel*>::iterator It = Steps.begin(); It != Steps.end(); ++It)
    {
        unsigned int const ZoneId = (*It)->ZoneId;
        std::map<unsigned int, std::vector<int>>::iterator Found = ZoneSteps.find(ZoneId);
        if (Found == ZoneSteps.end())
        {
            // insert(value_type(zone, empty vector)) (the routine).
            Found = ZoneSteps.insert(
                std::pair<unsigned int const, std::vector<int>>(ZoneId, std::vector<int>())).first;
        }
        Found->second.insert(Found->second.end(), 1, StepIndex);
        ++StepIndex;
    }
}

// (06f378c0-06f37a96, RET 8).
bool FPatrolPath::RegisterObject(unsigned int ObjectId, Vector const* Position)
{
    bool bRegistered = false;
    for (unsigned int Index = 0; Index < Points.size(); ++Index)
    {
        // the offset is built from float differences (the routine); the
        // squares are summed z, y, x. the field = 25e6, TEST AH,5 / JP: only less passes.
        Vector Offset;
        Offset.x = Points[Index].x - Position->x;
        Offset.y = Points[Index].y - Position->y;
        Offset.z = Points[Index].z - Position->z;
        double const DistanceSquared = static_cast<double>(Offset.z) * Offset.z
                                     + static_cast<double>(Offset.y) * Offset.y
                                     + static_cast<double>(Offset.x) * Offset.x;
        if (!(DistanceSquared < 25000000.0))
            continue;


        FPatrolPathObject Entry;
        Entry.ObjectId = ObjectId;
        std::vector<FPatrolPathObject>::iterator Found = FindPathObject(&Objects, ObjectId);
        if (Found != Objects.end())
        {
            Found->PointIndices.insert(Found->PointIndices.end(), 1, static_cast<int>(Index));
        }
        else
        {
            // into the local entry, then the routine insert(end); the re-find
            // and its pair (the routine) are discarded.
            Entry.PointIndices.insert(Entry.PointIndices.end(), 1, static_cast<int>(Index));
            Objects.insert(Objects.end(), Entry);
        }
        bRegistered = true;
    }
    return bRegistered;
}

// (06f37aa0-06f37cf6, RET 0x10).
void FPatrolPath::FindObjectsInRange(std::vector<FPatrolPathObject>* Out, int First, int Last,
                                     unsigned int TypeMask)
{
    // erase(begin, end) (the routine + the routine).
    Out->clear();

    if (First == -1)
        First = 0;
    if (Last == -1)
        Last = static_cast<int>(Points.size()) - 1;

    for (std::vector<FPatrolPathObject>::iterator Object = Objects.begin();
         Object != Objects.end(); ++Object)
    {
        // a zero mask never matches (the type stays -1 and -1 & 0 == 0).
        unsigned int Type = 0xffffffff;
        if (TypeMask != 0)
            pub::SpaceObj::GetType(Object->ObjectId, Type);
        if ((Type & TypeMask) == 0)
            continue;

        for (std::vector<int>::iterator Index = Object->PointIndices.begin();
             Index != Object->PointIndices.end(); ++Index)
        {
            // (JG / JG): inside [First, Last]; 06f37c55-06f37c61 (JLE /
            // JLE) when First > Last: outside (Last, First).
            bool bInRange;
            if (First <= Last)
                bInRange = First <= *Index && *Index <= Last;
            else
                bInRange = First <= *Index || *Index <= Last;
            if (!bInRange)
                continue;

            // The first index in range settles the object: added unless Out already holds
            // its id (the routine insert(end); the re-find the routine and the pair
            // are discarded), then the index scan stops.
            if (FindPathObject(Out, Object->ObjectId) == Out->end())
                Out->insert(Out->end(), *Object);
            break;
        }
    }
}

// (06f37d30-06f37e98, RET 8).
void FPatrolPath::GetObjectPointIndices(unsigned int const* ObjectId, std::vector<int>* Out)
{
    // erase(begin, end).
    Out->clear();

    // (the free of an empty temporary at 06f37d81 has no effect).
    std::vector<FPatrolPathObject>::iterator Found = FindPathObject(&Objects, *ObjectId);
    if (Found == Objects.end())
        return;
    // vector operator= (self-assignment skipped).
    if (&Found->PointIndices == Out)
        return;
    *Out = Found->PointIndices;
}

// (06f37ea0-06f37fea, RET 8).
bool FPatrolPath::GetZoneSegments(unsigned int ZoneId, std::vector<FPatrolPathSegment>* Out)
{
    // erase(begin, end) (the routine + the routine).
    Out->clear();

    std::map<unsigned int, std::vector<int>>::iterator Found = ZoneSteps.find(ZoneId);
    if (Found == ZoneSteps.end())
        return false;

    // grow the capacity to twice the step count when it is smaller.
    std::vector<int> const& Steps = Found->second;
    Out->reserve(Steps.size() * 2);

    // {step, step + 1} each, the routine insert(end).
    for (std::vector<int>::const_iterator Step = Steps.begin(); Step != Steps.end(); ++Step)
    {
        FPatrolPathSegment Segment;
        Segment.FirstPoint = *Step;
        Segment.SecondPoint = *Step + 1;
        Out->insert(Out->end(), 1, Segment);
    }
    return true;
}

// (06f37ff0-06f38117, RET 4).
int FPatrolPath::GetZoneIds(std::vector<unsigned int>* Out)
{
    // erase(begin, end); 06f38021-06f3809a: reserve(ZoneSteps.size()).
    Out->clear();
    Out->reserve(ZoneSteps.size());

    // in-order walk of the map, key each (the routine insert(end)).
    for (std::map<unsigned int, std::vector<int>>::iterator It = ZoneSteps.begin();
         It != ZoneSteps.end(); ++It)
    {
        Out->insert(Out->end(), 1, It->first);
    }
    return static_cast<int>(Out->size());
}
