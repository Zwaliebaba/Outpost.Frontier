// Author: Giperion.

#include "client_ime_focus.h"

#include "font_atlas.h"
#include "font_imp.h"
#include "font_string.h"
#include "globals.h"
#include "screen_quad_state.h"
#include "win3d_immediate.h"
#include "win3d_renderables.h"   // ImeFocusDrawPass (the routine)

#include "NeuronRuntime.h"
#include "neuroncomponents.h"
#include "render/IRenderPipeline.h"

#include <cstring>
#include <new>

namespace
{
    IFont* G_ConfirmPlayerFont = nullptr;
    // the client IMEFocus the routine builds; the routine deletes it at
    // shutdown (0x005b2349). .bss zero.
    ClientIMEFocus* G_ClientImeFocus = nullptr;
    // / the field -- set once by the static initializers the branch /
    // (XC table 0x0060fe74 / 0x0060fe70): NEURONCOMPONENTS_CRC::GetCRC32 of the literal.
    unsigned long G_ImeBackgroundKey = NEURONCOMPONENTS_CRC::GetCRC32("IMEbkground");
    unsigned long G_ImeTextKey = NEURONCOMPONENTS_CRC::GetCRC32("IMEtext");

    // (cdecl OutPrev, NewKey) — 0x00417790..0x004177a5: the old the field into
    // ECX, NewKey stored, *OutPrev = old (no null test).
    void SwapFontAtlasMaterialKey(unsigned* OutPrev, unsigned NewKey)
    {
        unsigned const Prev = FontAtlasMaterialKey();
        FontAtlasSetMaterialKey(NewKey);
        *OutPrev = Prev;
    }

    // colour unpack → FontAtlasSetDrawColor (the routine via ptr).
    unsigned PackDrawColorBytes(unsigned Color)
    {
        unsigned char Bytes[4];
        Bytes[0] = static_cast<unsigned char>((Color >> 16) & 0xffu);
        Bytes[1] = static_cast<unsigned char>((Color >> 8) & 0xffu);
        Bytes[2] = static_cast<unsigned char>(Color & 0xffu);
        Bytes[3] = static_cast<unsigned char>((Color >> 24) & 0xffu);
        unsigned Packed = 0;
        Packed |= static_cast<unsigned>(Bytes[0]);
        Packed |= static_cast<unsigned>(Bytes[1]) << 8;
        Packed |= static_cast<unsigned>(Bytes[2]) << 16;
        Packed |= static_cast<unsigned>(Bytes[3]) << 24;
        return Packed;
    }
}

IFont* ClientConfirmPlayerFont()
{
    if (G_ConfirmPlayerFont == nullptr)
        G_ConfirmPlayerFont = FindMenuFontImp("ConfirmPlayerText");
    return G_ConfirmPlayerFont;
}

unsigned long ClientImeBackgroundKey()
{
    return G_ImeBackgroundKey;
}

unsigned long ClientImeTextKey()
{
    return G_ImeTextKey;
}

void ClientImeSwapFontMaterialKey(unsigned* OutPrev, unsigned NewKey)
{
    SwapFontAtlasMaterialKey(OutPrev, NewKey);
}

void ImeFocusDrawPass()
{
    // (cdecl, void), listing-traced 0x00563af0..0x00563ceb. Sole caller: the Win3D
    // draw-all tail 0x0059e4d3.
    // IMEFocus::GetSingleton(); null, or no registered input box
    // (IMEFocus::NumInputBoxes), -> nothing.
    IMEFocus* const Focus = IMEFocus::GetSingleton();
    if (Focus == nullptr)
        return;
    if (Focus->NumInputBoxes() == 0)
        return;

    // glMatrixMode(0x1701) is the empty RET at 0x00401e90.
    // / 0x00563b3b: the routine, the routine.
    Win3DPushProjection();
    Win3DSetOrtho2D();

    // stage 0 types 0x10 / 0x11 read (slot 63) into two locals;
    // both set to 1.
    int SavedStage0Type10 = 0;   // [ESP+0x4]
    int SavedStage0Type11 = 0;   // [ESP+0x8]
    NeuronRuntime::Pipeline->GetTextureStageState(0, 0x10, &SavedStage0Type10);
    NeuronRuntime::Pipeline->GetTextureStageState(0, 0x11, &SavedStage0Type11);
    NeuronRuntime::Pipeline->SetTextureStageState(0, 0x10, 1);
    NeuronRuntime::Pipeline->SetTextureStageState(0, 0x11, 1);

    // two savers (EH states 0, 1): render state 0x17 -> 8, 0xe -> 0.
    FScreenQuadRenderStateSaver const SaverA(NeuronRuntime::Pipeline, 0x17, 8);
    FScreenQuadRenderStateSaver const SaverB(NeuronRuntime::Pipeline, 0xe, 0);

    // the routine(0xb44) -- GL_CULL_FACE off.
    Win3DDisable(kWin3DCullFace);

    // IMEFocus::Draw(0).
    Focus->Draw(nullptr);

    // the routine(Pipeline, the field), then (.., the field).
    ScreenQuadFlush(NeuronRuntime::Pipeline, G_ImeBackgroundKey);
    ScreenQuadFlush(NeuronRuntime::Pipeline, G_ImeTextKey);

    // stage 0 types 0x10 / 0x11 back.
    NeuronRuntime::Pipeline->SetTextureStageState(0, 0x10, SavedStage0Type10);
    NeuronRuntime::Pipeline->SetTextureStageState(0, 0x11, SavedStage0Type11);

    // the routine(0xb44) -- GL_CULL_FACE on.
    Win3DEnable(kWin3DCullFace);

    // the routine. The savers then restore 0xe, then 0x17 (0x00563ca1..0x00563cd6).
    Win3DPopProjection();
}

ClientIMEFocus::ClientIMEFocus(HWND Hwnd)
    : IMEFocus(Hwnd)
    , ConfirmText(nullptr)
{
    // Inlined into the routine, 0x005640bf..0x005640f2: +0x18 = the routine; the field
    // fetched on first use (the routine "ConfirmPlayerText"); slot 2 (+0x8) binds it and slot 8
    // (+0x20) takes 0. baseline tests neither the new object nor the font.
    ConfirmText = new FontStringChild();
    ConfirmText->BindFont(ClientConfirmPlayerFont());
    ConfirmText->SetShadow(0);
}

ClientIMEFocus::~ClientIMEFocus()
{
    // — Release Confirm (vt+0x4) then base IMEFocus dtor.
    if (ConfirmText != nullptr)
    {
        ConfirmText->Release();
        ConfirmText = nullptr;
    }
}

void ClientIMEFocus::DrawRect(int X1, int Y1, int X2, int Y2, DWORD Color)
{
    // (module storage-00564056, RET 0x14; `this` is not read).
    // IMEFocus::GetFocus()+0x9c (the client IMEInputBox's FontString,
    // ) -> vt+0x18 GetSize; the x extents move left by the text width.
    IMEInputBox* Box = GetFocus();
    FontStringChild* SizeProbe =
        *reinterpret_cast<FontStringChild**>(reinterpret_cast<char*>(Box) + 0x9c);
    int SizeWH[2];
    SizeProbe->GetSize(SizeWH);
    int const Left = X1 - SizeWH[0];
    int const Right = X2 - SizeWH[0];

    // the routine(the field).
    ScreenQuadSetMaterialKey(G_ImeBackgroundKey);

    // the rect FILD / FSTP float, UVs {0, 0} .. {1, 1}.
    float Fx0 = static_cast<float>(Left);
    float Fy0 = static_cast<float>(Y1);
    float Fx1 = static_cast<float>(Right);
    float Fy1 = static_cast<float>(Y2);
    float U0 = 0.0f;
    float V0 = 0.0f;
    float U1 = 1.0f;
    float V1 = 1.0f;

    // the routine on x against [0, the field - 1], span
    // (x1 - x0) + 1 (the field); 0 -- nothing.
    float const SpanX = static_cast<float>(static_cast<double>(Fx1) - Fx0 + 1.0);
    float const SpanY = static_cast<float>(static_cast<double>(Fy1) - Fy0 + 1.0);
    if (ScreenQuadClip1D(&Fx0, &Fx1, &U0, &U1, 0.0f,
                         static_cast<float>(static_cast<int>(G_RenderSurfaceWidth) - 1),
                         SpanX) == 0)
        return;
    // on y against [0, the field - 1], span (y1 - y0) + 1 computed
    // before the x clip.
    if (ScreenQuadClip1D(&Fy0, &Fy1, &V0, &V1, 0.0f,
                         static_cast<float>(static_cast<int>(G_RenderSurfaceHeight) - 1),
                         SpanY) == 0)
        return;

    // the routine(&the field, 0, 1).
    ScreenQuadBeginBatchFrame(G_ScreenQuadDefaultFrame, 0, true);
    // one record {x0, y0, x1, y1, 1, 1, u0, v0, u1, v1, Color}.
    FScreenQuadRecord Record;
    Record.X0 = Fx0;
    Record.Y0 = Fy0;
    Record.X1 = Fx1;
    Record.Y1 = Fy1;
    Record.DepthA = 1.0f;
    Record.DepthB = 1.0f;
    Record.U0 = U0;
    Record.V0 = V0;
    Record.U1 = U1;
    Record.V1 = V1;
    Record.ColorBits = Color;
    ScreenQuadAppendRecord(Record);
}

void ClientIMEFocus::DrawTextW(const wchar_t* Text, unsigned Color, void* Layout,
                               unsigned Count)
{
    // — Win3D underline runs (stride 0x20) + Confirm FontString paint.
    float* const Entries = static_cast<float*>(Layout);

    Win3DBegin(kWin3DTriangleFan);
    Win3DVertex3f(Entries[0], Entries[1], 0.0f);

    if (Count > 1u)
    {
        unsigned char* const Base = static_cast<unsigned char*>(Layout);
        unsigned char* Esi = Base + 0x32;
        for (unsigned Edi = 1u; Edi < Count; ++Edi)
        {
            // Entry+0x10 AARRGGBB dword; [ESI-2..ESI]=B,G,R → Win3DColor3ub(R,G,B).
            Win3DColor3ub(Esi[0], Esi[-1], Esi[-2]);

            float* const Cur = reinterpret_cast<float*>(Esi - 0x12);
            Win3DVertex3f(Cur[0], Cur[1], 0.0f);

            unsigned NextIdx = (Edi == Count - 1u) ? 1u : (Edi + 1u);
            float* const Next = reinterpret_cast<float*>(Base + (NextIdx << 5));
            Win3DVertex3f(Next[0], Next[1], 0.0f);

            Esi += 0x20;
        }
    }

    Win3DEnd();

    unsigned PrevMat = 0;
    SwapFontAtlasMaterialKey(&PrevMat, static_cast<unsigned>(G_ImeTextKey));

    int PosX = static_cast<int>(Entries[0]);
    int PosY = static_cast<int>(Entries[1]);
    FontAtlasSetDrawColor(PackDrawColorBytes(Color));

    ConfirmText->SetText(Text);
    int SizeWH[2];
    ConfirmText->GetSize(SizeWH);

    // x - (w SHR 1) and y - (h SHR 1), each FILD as a zero-extended
    // qword (so a negative result becomes a large positive float). Paint reads only x and y.
    unsigned const OriginXBits =
        static_cast<unsigned>(PosX) - (static_cast<unsigned>(SizeWH[0]) >> 1);
    unsigned const OriginYBits =
        static_cast<unsigned>(PosY) - (static_cast<unsigned>(SizeWH[1]) >> 1);
    FFontStringDrawOrigin Origin;
    Origin.X = static_cast<float>(static_cast<double>(OriginXBits));
    Origin.Y = static_cast<float>(static_cast<double>(OriginYBits));
    Origin.Scale = 0.0f;
    // vt+0x24(&origin, 0.0f, 0.0f) -- no trim.
    ConfirmText->Paint(&Origin, nullptr, nullptr);

    unsigned Discard = 0;
    SwapFontAtlasMaterialKey(&Discard, PrevMat);
}

IMEFocus* EnsureClientIMEFocus()
{
    // listing 00564060..00564141. Caller: the pre-init tail 0x005b1f5f, once.
    // LoadLibraryA("ImeUi.dll"); on failure the current the field
    // is returned (0x0056412e). The handle is never freed (the exe also imports the DLL).
    if (LoadLibraryA("ImeUi.dll") == nullptr)
        return G_ClientImeFocus;

    // operator_new(0x1c), IMEFocus::IMEFocus(the field), the
    // vftable the routine and the ConfirmPlayerText string (ClientIMEFocus's ctor).
    ClientIMEFocus* Focus = new ClientIMEFocus(g_client.window);
    // the object is stored before the check.
    G_ClientImeFocus = Focus;

    // byte +0x17 zero -> scalar delete (slot 0, the routine with 1),
    // = 0 and null is returned.
    if (!Focus->IsImeAvailable())
    {
        delete Focus;
        G_ClientImeFocus = nullptr;
        return nullptr;
    }
    return Focus;
}

void ReleaseClientIMEFocus()
{
    // slot 0 (scalar delete, 1) on the field when set; then it is zeroed.
    if (G_ClientImeFocus != nullptr)
        delete G_ClientImeFocus;
    G_ClientImeFocus = nullptr;
}
