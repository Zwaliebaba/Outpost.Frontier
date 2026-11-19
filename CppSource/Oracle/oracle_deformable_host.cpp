// P4.7 (third increment): the scene runtime as the client composes it, for the deformable animation
// suite (oracle_deformable_anim.cpp, which cannot include SceneRuntime.h next to Deformable2's
// sources). NeuronRuntime::Startup creates the Engine and calls IEngine::LoadEngineComponents, which
// aggregates the components of the profile's [Engine] section into it (Animation among them):
// each made with the engine as its outer unknown, its inner unknown linked into the engine's
// component list, then initialised through IAggregateComponent; Startup then takes IAnimation2 and
// IChannel from the engine's QueryInterface, which asks the aggregated components. Here the
// Animation is aggregated the same way (the profile's walk is replaced by the one component), so
// that the engine also answers IRootData through it, as PlayInstanceAnim asks.

#include "oracle_support.h"

#include <string.h>

#include "neuroncomponents.h"
#include "../SceneRuntime/SceneRuntime.h"

// oracle_anim.cpp.
bool OracleLoadSceneRuntime();

bool OracleMakeAnimatedEngine(IEngine** OutEngine, IAnimation2** OutAnimation, IChannel** OutChannel)
{
    *OutEngine = nullptr;
    *OutAnimation = nullptr;
    *OutChannel = nullptr;
    if (!OracleLoadSceneRuntime())
        return false;
    ICOManager* Manager = NEURONCOMPONENTS_Acquire();
    FAggDesc8 EngineDesc;
    EngineDesc.Size = G_AggDescSizeEngine;
    EngineDesc.Name = "Engine";
    void* Created = nullptr;
    if (Manager->CreateInstance((AGGDESC*)(&EngineDesc), &Created) != GR_OK || Created == nullptr)
        return false;
    IEngine* Engine = (IEngine*)(Created);
    FEngineObject* EngineObject = static_cast<FEngineObject*>(Engine);

    // LoadEngineComponents' first pass for "Animation".
    void* OuterInner = nullptr;
    FAggDesc18 Desc;
    memset(&Desc, 0, sizeof(Desc));
    Desc.Size = G_AggDescSizeAnimation;
    Desc.Name = "Animation";
    Desc.Inner = Engine;
    Desc.Outer = &OuterInner;
    Created = nullptr;
    if (Manager->CreateInstance((AGGDESC*)(&Desc), &Created) < 0 || OuterInner == nullptr)
        return false;
    FEngineChildNode* Node = new FEngineChildNode;
    memset(Node, 0, sizeof(*Node));
    memcpy(Node->Name, "Animation", sizeof("Animation"));
    Node->Object = (INeuronComponent*)(OuterInner);
    FEngineChildNode* Sentinel = EngineObject->ComponentList;
    Node->Next = Sentinel;
    Node->Prev = Sentinel->Prev;
    Sentinel->Prev->Next = Node;
    Sentinel->Prev = Node;
    EngineObject->ComponentCount += 1;

    // The second pass: IAggregateComponent::Initialize.
    IAggregateComponent* Aggregate = nullptr;
    if (Node->Object->QueryInterface(Iids::AggregateComponent, (void**)(&Aggregate)) < 0 || Aggregate == nullptr ||
        Aggregate->Initialize() < 0)
        return false;
    Aggregate->Release();

    // NeuronRuntime::Startup's queries.
    if (Engine->QueryInterface(Iids::Animation2, (void**)(OutAnimation)) < 0 ||
        Engine->QueryInterface(Iids::Channel, (void**)(OutChannel)) < 0)
        return false;
    *OutEngine = Engine;
    return true;
}
