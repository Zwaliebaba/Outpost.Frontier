// Renderer call trace (design/migration-plan.md P0.5): the session that writes the
// JSONL trace and its content-addressed blobs, the handle ids, the pending locks
// and the state slots a recorded frame starts from. README.md in this folder
// describes the format and the replay contract. Oracle instrumentation only:
// nothing here changes what the renderer does.

#pragma once

#include "neuroncomponents.h"
#include "render/IRenderPipeline.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace RenderTrace
{
    // When a call's line is written. kAlways: in every frame up to the last
    // selected one (lifecycle, configuration, resource creation and uploads).
    // kState: in selected frames, and its line is kept as a state slot that the
    // next selected frame starts from. kFrame: in selected frames only.
    enum ECategory
    {
        kAlways = 0,
        kState = 1,
        kFrame = 2,
    };

    // Handle namespaces. Every id comes from one counter and is never reused.
    enum EHandleKind
    {
        kTexture = 0,
        kVertexBuffer = 1,
        kIndexBuffer = 2,
        kLight = 3,
    };

    // Bytes per vertex of an FVF as DX8RENDERER computes it (FvfStride and
    // VertexStrideFromFvf in DX8RENDERER.cpp, which agree).
    unsigned int FvfStride(unsigned int Fvf);
    // DX8RENDERER's PrimCountFromVerts.
    unsigned int PrimCount(unsigned long Prim, unsigned int Count);
    // Vertices (or indices) Direct3D reads for PrimitiveCount primitives.
    unsigned int VerticesRead(unsigned long Prim, unsigned int PrimitiveCount);
    // What the user-pointer draws read from the caller's memory.
    size_t DrawUpVertexBytes(unsigned long Prim, unsigned int Fvf, unsigned int Count);
    size_t DrawIndexedUpVertexBytes(unsigned long Prim, unsigned int Fvf, unsigned int VertCount,
                                    unsigned int IndexCount);
    size_t DrawIndexedUpIndexBytes(unsigned long Prim, unsigned int IndexCount);
    size_t DrawIndexedVbIndexBytes(unsigned long Prim, unsigned int IndexCount);
    // Bytes a surface of pfenum Format spans: (rows - 1) * Pitch + one row. 0 when the
    // format's size is not known (pfenum 0, 9 and 15..21).
    size_t SurfaceBytes(unsigned int Format, unsigned int Width, unsigned int Height, unsigned int Pitch);
    unsigned long long Fnv1a64(const void* Data, size_t Size);

    // A buffer region handed out by LockVb, LockIb or LockVertexMemory and not yet
    // unlocked. First is the first vertex (index for an IB) the region starts at.
    struct FBufferLock
    {
        const unsigned char* Bits;
        unsigned int First;
        bool bFirstKnown;
        unsigned int Count;
        unsigned int Stride;
    };

    struct FTextureLock
    {
        const unsigned char* Bits;
        unsigned int Width;
        unsigned int Height;
        unsigned int Pitch;
        unsigned int Format;
    };

    class FSession;

    // One line of the trace. Construct it after the forwarded call returned (or
    // before, when the call invalidates memory the line reads), write the
    // arguments, the result and an optional state key; the destructor commits it.
    // Every writer is a no-op while the call is not active.
    class FCall
    {
    public:
        FCall(FSession& Session, ECategory Category, const char* Interface, const char* Method);
        ~FCall();

        bool Active() const { return bActive; }
        // The line goes to the file (not only to a state slot).
        bool Writes() const { return bWrites; }
        unsigned long long Seq() const { return SeqNo; }

        void Int(const char* Name, long long Value);
        void UInt(const char* Name, unsigned long long Value);
        void Float(const char* Name, float Value);
        // The float stored at Address, by its bits.
        void FloatAt(const char* Name, const void* Address);
        // A pointer-sized value written as a number. Addresses are not stable
        // between runs; this is for flags, HWNDs and unused arguments.
        void Raw(const char* Name, const void* Value);
        void Str(const char* Name, const char* Value);
        void Bool(const char* Name, bool Value);
        void Null(const char* Name);
        // Count 32-bit words read from Data, each as 8 hex digits (floats as bit patterns).
        void Words(const char* Name, const void* Data, size_t Count);
        // Size bytes read from Data: inline hex up to 64 bytes, else a blob file.
        void Bytes(const char* Name, const void* Data, size_t Size);
        // The value an out-pointer holds after the call (null for a null pointer).
        void OutI32(const char* Name, const void* Pointer);
        void OutU32(const char* Name, const void* Pointer);
        void OutF32(const char* Name, const void* Pointer);
        void OutPtr(const char* Name, const void* Pointer);
        // A handle as its id: 0 for null, -1 for the renderer's (void*)-1 scratch or
        // current buffer, a positive id otherwise (assigned on first sight if needed).
        // Returns the id written (0 while the call is not active).
        long long Handle(const char* Name, EHandleKind Kind, const void* Value);
        void OutHandle(const char* Name, EHandleKind Kind, const void* Pointer);
        void HandleId(const char* Name, long long Id);
        // A vertex source descriptor resolved to the bytes the converters read.
        // DstFvf is the destination FVF when known (0 otherwise).
        void VertexSource(const char* Name, const FRpVertexSrcDesc* Desc, unsigned int Start,
                          unsigned int Count, unsigned int DstFvf);
        void BeginObject(const char* Name);
        void EndObject();
        void Return(GENRESULT Result);
        // Keep this line as the state slot Key (kState calls).
        void KeepAsState(const char* Key);
        void Note(const char* Text);
        // Marks the line "synthetic": the proxy wrote it for something the renderer did
        // without a call (LightData).
        void Synthetic();

    private:
        FCall(const FCall&);
        FCall& operator=(const FCall&);

        void Key(const char* Name);
        long long ResolveHandle(EHandleKind Kind, const void* Value);
        void Stream(const char* Name, const FRpVertexStream& Stream, unsigned int Element, bool bIndexed,
                    unsigned int Start, unsigned int Count);

        FSession& Session;
        ECategory Category;
        bool bActive;
        bool bWrites;
        bool bHasResult;
        bool bSynthetic;
        GENRESULT Result;
        unsigned long long SeqNo;
        std::string Line;
        std::string StateKey;
        std::string NoteText;
        std::vector<long long> FirstSeen;
        std::vector<bool> First; // per open object: no member written yet
    };

    class FSession
    {
    public:
        FSession();
        ~FSession();

        // Reads [RenderTrace] (output, frames) from the engine INI and the
        // OUTPOST_RENDER_TRACE_DIR / OUTPOST_RENDER_TRACE_FRAMES environment
        // variables, which win; opens the output when both are set. Section is the
        // [System] line's extra text, recorded in the header.
        void Configure(const char* Section);
        void Close();

        // Tracing is configured and the last selected frame has not ended.
        bool Tracking() const { return bTracking; }
        bool FrameRecorded() const { return bFrameRecorded; }
        unsigned int Frame() const { return FrameNo; }
        // After SwapBuffers: ends the frame, starts the next and writes its snapshot
        // when it is selected, closes the trace after the last selected frame.
        void EndFrame();

        long long NewHandle(EHandleKind Kind, const void* Value);
        long long LookupHandle(EHandleKind Kind, const void* Value, bool* bNew);
        long long FindHandle(EHandleKind Kind, const void* Value) const;
        void RetireHandle(EHandleKind Kind, const void* Value);

        void MarkPoolVb(const void* Vb, unsigned int Fvf);
        bool IsPoolVb(const void* Vb) const;

        void PushBufferLock(EHandleKind Kind, const void* Buffer, const FBufferLock& Lock);
        bool PopBufferLock(EHandleKind Kind, const void* Buffer, FBufferLock* Out);
        // The locked VB region holding Address, for CopyVertexData's destination.
        bool FindVbRegion(const void* Address, const void** Vb, size_t* Offset) const;
        void SetTextureLock(const void* Texture, int Level, const FTextureLock& Lock);
        bool PopTextureLock(const void* Texture, int Level, FTextureLock* Out);

        // State model (see README.md, "Recorded frames").
        void OnStageTexture(int Stage, const void* Texture);
        void OnTextureUnbound(const void* Texture);
        void OnResetStates();
        void OnDeviceReset();
        void OnRenderState(unsigned int State, int Value);
        void OnLightGone(long long LightId);
        // Light data the renderer reads from the caller's D3DLIGHT8 at its next light flush
        // (README.md, LightData). AddLight and UpdateLight make a light pending; so does
        // SetLightEnable on, and CreateBuffers for every live light, since the flush then
        // reads the data too. RemoveLight and DestroyLight forget the light.
        void OnLightData(const void* Light);
        void OnLightEnabled(const void* Light);
        void OnLightRemoved(const void* Light);
        // Moves the pending lights (id, the caller's pointer) out, in id order.
        void TakePendingLights(std::vector<std::pair<long long, const void*> >* Out);
        // After a successful CreateBuffers: the renderer's defaults as GetRenderState and
        // GetPipelineState read them (README.md, defaults line). Written in every frame up to
        // the last selected one and kept as the state slot "DEFAULTS". Read is false where the
        // getter failed; that entry is left out.
        void WriteDefaults(const unsigned int* States, const unsigned long* RenderValues, const bool* RenderRead,
                           size_t RenderCount, const int* PipelineValues, const bool* PipelineRead,
                           size_t PipelineCount);
        // A selected frame's reference image after its SwapBuffers: the BMP file's bytes,
        // or null and the reason it is missing.
        void WriteFrameImage(const void* Bmp, size_t Size, const char* Note);
        // Where the frame image is written before it is read back: a .bmp in the temporary
        // folder, unique to this process and session.
        std::string FrameImagePath() const;
        void OnVbGone(const void* Vb);
        void OnIbGone(const void* Ib);
        void ResetSelection();
        void SelectVb(const void* Vb);
        void SelectPoolVb(unsigned int Fvf);
        void SelectIb(const void* Ib, unsigned int BaseVertex);
        void ClearIb();

        // Used by FCall.
        void Lock();
        void Unlock();
        unsigned long long NextSeq() { return ++Seq; }
        void WriteLine(const std::string& Line);
        void StoreSlot(const std::string& Key, const std::string& Line);
        std::string StoreBlob(const void* Data, size_t Size);

    private:
        FSession(const FSession&);
        FSession& operator=(const FSession&);

        struct FSlot
        {
            unsigned long long Order;
            std::string Line;
        };

        struct FSelection
        {
            int VbKind; // 0 none, 1 handle, 2 pool buffer of VbFvf not seen yet
            const void* Vb;
            unsigned int VbFvf;
            const void* Ib; // null: none
            unsigned int IbBase;
        };

        bool FrameSelected(unsigned int Frame) const;
        void BeginFrame();
        void EraseSlotsWithPrefix(const char* Prefix);
        std::string SyntheticHead(const char* Interface, const char* Method) const;
        void Flush();

        void* CriticalSection; // CRITICAL_SECTION, allocated so this header needs no <windows.h> layout
        long Instance;         // 1 for trace.jsonl, 2 for trace-2.jsonl, ...
        bool bConfigured;
        bool bTracking;
        bool bFrameRecorded;
        unsigned int FrameNo;
        unsigned long long Seq;
        unsigned long long SlotOrder;
        long long NextId;
        std::vector<std::pair<unsigned int, unsigned int> > Frames; // inclusive ranges
        unsigned int LastFrame;
        std::string FrameSpec;
        std::string Directory;
        std::string BlobDirectory;
        FILE* File;
        std::string Buffer;
        std::set<unsigned long long> Blobs;

        std::map<std::pair<int, const void*>, long long> Handles;
        std::set<const void*> PoolVbs;
        std::map<unsigned int, const void*> PoolVbByFvf;
        std::map<std::pair<int, const void*>, std::vector<FBufferLock> > BufferLocks;
        std::map<std::pair<const void*, int>, FTextureLock> TextureLocks;

        std::map<std::string, FSlot> Slots;
        std::map<int, const void*> StageTextures;
        std::map<long long, const void*> LiveLights;    // AddLight or UpdateLight seen, not removed
        std::map<long long, const void*> PendingLights; // data to read at the next light flush
        bool bSwvpKnown;
        int Swvp;
        FSelection Selection;
        unsigned long long SelectionSeq;
        unsigned int SelectionFrame;

        friend class FCall;
    };

    // Takes the session lock while it exists.
    class FScopedLock
    {
    public:
        explicit FScopedLock(FSession& InSession) : Session(InSession) { Session.Lock(); }
        ~FScopedLock() { Session.Unlock(); }

    private:
        FScopedLock(const FScopedLock&);
        FScopedLock& operator=(const FScopedLock&);
        FSession& Session;
    };
}
