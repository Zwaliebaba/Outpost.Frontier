// Author: Giperion.

#pragma once

#include "neuroncomponents.h"

// Mesh interfaces shared by RendComp and materials.

struct IMaterial;

#ifndef OUTPOST_FRONTIER_IMESH
#define OUTPOST_FRONTIER_IMESH
struct IMesh : INeuronComponent
{
    virtual int NEURONCOMPONENTS_API ResetMesh() = 0; // formerly Slot3
    virtual int NEURONCOMPONENTS_API NoOp() = 0; // formerly Slot4
    virtual int NEURONCOMPONENTS_API RefreshMaterials(unsigned long Flags) = 0; // formerly Slot5
    virtual int NEURONCOMPONENTS_API RejectOperation(void* A) = 0; // formerly Slot6
    virtual int NEURONCOMPONENTS_API RejectOperation() = 0; // formerly Slot7
    virtual int NEURONCOMPONENTS_API GetFaceGroupCount(int* Out) = 0; // formerly Slot8
    virtual int NEURONCOMPONENTS_API OpenFaceGroup(int Index, int Mode) = 0; // formerly Slot9
    virtual int NEURONCOMPONENTS_API CloseFaceGroup() = 0; // formerly Slot10
    virtual int NEURONCOMPONENTS_API AddFaceGroup(void* A) = 0; // formerly Slot11
    virtual int NEURONCOMPONENTS_API RemoveFaceGroup(int Index) = 0; // formerly Slot12
    virtual int NEURONCOMPONENTS_API GetFaceGroupPrimitiveType(unsigned int* Out) = 0; // formerly Slot13
    virtual int NEURONCOMPONENTS_API GetFaceGroupMaterial(IMaterial** Out) = 0; // formerly Slot14
    virtual int NEURONCOMPONENTS_API SetFaceGroupMaterial(IMaterial* Material) = 0; // formerly Slot15
    virtual int NEURONCOMPONENTS_API GetFaceGroupMaterialContext(void** Out) = 0; // formerly Slot16
    virtual int NEURONCOMPONENTS_API SetFaceGroupMaterialContext(void* A) = 0; // formerly Slot17
    virtual int NEURONCOMPONENTS_API OpenFaceGroupChannels(unsigned long Flags) = 0; // formerly Slot18
    virtual int NEURONCOMPONENTS_API CloseFaceGroupChannels() = 0; // formerly Slot19
    virtual int NEURONCOMPONENTS_API LockFaceGroupChannels(unsigned long Mask) = 0; // formerly Slot20
    virtual int NEURONCOMPONENTS_API UnlockFaceGroupChannels() = 0; // formerly Slot21
    virtual int NEURONCOMPONENTS_API GetFaceGroupData(int Channel, void** A, void** B, void** C, void** D) = 0; // formerly Slot22
    virtual int NEURONCOMPONENTS_API SetFaceGroupData(int Channel, void* A, void* B, void* C, void* D) = 0; // formerly Slot23
    virtual int NEURONCOMPONENTS_API GetFaceGroupIndices(int Channel, void** A, void** B) = 0; // formerly Slot24
    virtual int NEURONCOMPONENTS_API SetFaceGroupIndices(int Channel, void* Src, int Count) = 0; // formerly Slot25
    virtual int NEURONCOMPONENTS_API GetFaceGroupIndexCount(int Selector, void** Out) = 0; // formerly Slot26
    virtual int NEURONCOMPONENTS_API SetFaceGroupIndexCount(int Selector, int Count) = 0; // formerly Slot27
    virtual int NEURONCOMPONENTS_API AddFaceGroupChannels(unsigned long Flags) = 0; // formerly Slot28
    virtual int NEURONCOMPONENTS_API RemoveFaceGroupChannels(unsigned long Flags) = 0; // formerly Slot29
};
#endif

#ifndef OUTPOST_FRONTIER_IVMESHWIRE
#define OUTPOST_FRONTIER_IVMESHWIRE
struct IVMeshWire : INeuronComponent
{

    virtual void* NEURONCOMPONENTS_API GetWireData() = 0; // formerly Slot3
    virtual int NEURONCOMPONENTS_API Draw() = 0; // formerly Slot4
    virtual void* NEURONCOMPONENTS_API GetMeshData() = 0; // formerly Slot5
};
#endif
