// P4.13: the client UI toolkit's generic controls, through the reference's own code.
// The Win3D manager (win3d.cpp), WinControl (wincontrol_base.cpp), the factory registry
// (win_factory.cpp), the hover pulse (nn_ui_pulse.cpp) and the controls ListBox / UIListBox
// (list_box.cpp, ui_list_box.cpp), UIScroll / UICycle (ui_scroll.cpp) and WinObject /
// UIWinObject (win_object.cpp, ui_win_object.cpp) are compiled into this file. The controls draw
// through recording stand-ins for the immediate-mode layer (win3d_immediate.cpp), the render
// pipeline, the font strings and the mesh holder; the client functions they reach otherwise
// (the screen projection of text objects, the frame time, the software cursor, the game options,
// the texture shapes, Common's math engine) are stand-ins with the values the case sets. Every
// other client symbol the compiled files name has an empty stand-in. The Rust side is
// crates/neuron-client/tests/oracle_vectors.rs with crates/neuron-client/tests/support/ui.rs;
// the inputs come from crates/neuron-client/tests/fixtures/oracle/make_ui_fixtures.py.
//
// ui_controls.list holds one case per line: operations separated by " ; ", their fields by
// single spaces, floats as f32 bits in hex, integers in decimal, texts and paths in hex. Each
// case starts with a recording parent (a WinControl) and an empty manager:
//   new UIListBox R B | new UIScroll R B E0 E1 E2 E3 F | new UICycle (as UIScroll) |
//   new UIWinObject R B Z PATH                WinFactoryCreate("C", class, parent, payload)
//   xform M0..M8 T0 T1 T2      SetTransform        pos X Y Z         SetPosition
//   ray X Y Z                  the mouse ray       cursor X Y        the software cursor
//   dt D                       the frame time      pulse D           NnUiPulseAdvance
//   opt V                      GetGameOption(0x16) update | draw | over | leave
//   btn F0 F1                  MouseEvent          hit               HitTest(the mouse ray)
//   state                      the object's state
//   ListBox: item TEXT DATA (AddItem), settext I TEXT, data I, f80 I, frame V, rules V,
//            rows N, first N, sel V
//   UIScroll: range A B, value V, page P, step S, size W H (the setters), stepmode V, horiz V,
//             custom V, active V, soff X Y Z (SetScrollOffset), offset X Y Z (property 6),
//             prop M A B (SetProperty with integers)
//   UIWinObject: flags V, tint RRGGBBAA
//
// Line formats, each prefixed with "case": the stand-ins' calls --
//   {"ev":"begin","p"} {"ev":"end"} {"ev":"v","x","y","z"} {"ev":"c3","c"} {"ev":"c4","c"}
//   {"ev":"tc","u","v"} {"ev":"dis","cap"} {"ev":"tex","h"} {"ev":"push"} {"ev":"pop"}
//   {"ev":"ortho"} {"ev":"mv","m"} {"ev":"rs","s","v"} {"ev":"color","c"}
//   {"ev":"paint","t","x","y","l","r"} {"ev":"load","path","mat"} {"ev":"mpos","x","y","z"}
//   {"ev":"mupd"} {"ev":"submit"} {"ev":"tinted","tint","l"}
// -- the parent's (slot 23 and SetProperty from the control; "self" for the control, a list
// row's text in hex or "null") --
//   {"ev":"msg","m","a","b"} {"ev":"prop","m","a","b"}
// -- and the operations' results and states --
//   {"ev":"new","ok"} {"ev":"btn","r"} {"ev":"hit","r"} {"ev":"r","v"}
//   {"ev":"list","hover","f80","sel","count","first","rows","t","w","h"}
//   {"ev":"scroll","flags","pos","v","rect","arrow","drag","rt","dragging","doff","inv","colors",
//    "soff","dpage"}
//   {"ev":"obj","flags","pos","orient","origin","corner"}

#include "oracle_support.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "../OutpostFrontier/Client/win3d.cpp"
#include "../OutpostFrontier/Client/wincontrol_base.cpp"
#include "../OutpostFrontier/Client/win_factory.cpp"
#include "../OutpostFrontier/Client/nn_ui_pulse.cpp"
#include "../OutpostFrontier/Client/list_box.cpp"
#include "../OutpostFrontier/Client/ui_list_box.cpp"
#include "../OutpostFrontier/Client/ui_scroll.cpp"
#include "../OutpostFrontier/Client/win_object.cpp"
#include "../OutpostFrontier/Client/ui_win_object.cpp"

namespace
{
    oracle::Suite* GCtlOut = nullptr;
    unsigned int GCtlCase = 0;
    Win3D* GCtlObject = nullptr;
    double GCtlFrameTime = 0.0;
    int GCtlCursor[2] = {0, 0};
    int GCtlOption = 1;

    void CtlEvent(const std::string& Fields)
    {
        if (GCtlOut != nullptr)
            GCtlOut->Line(oracle::Format("{\"case\":\"%u\",", GCtlCase) + Fields + "}");
    }

    std::string Bits(float Value)
    {
        return oracle::Format("%08x", oracle::FloatBits(Value));
    }

    std::string Bits3(float const* V)
    {
        return Bits(V[0]) + " " + Bits(V[1]) + " " + Bits(V[2]);
    }

    std::string BitsN(float const* V, int Count)
    {
        std::string Out;
        for (int Index = 0; Index < Count; ++Index)
            Out += (Index == 0 ? "" : " ") + Bits(V[Index]);
        return Out;
    }

    std::string Units(wchar_t const* Text)
    {
        if (Text == nullptr)
            return "null";
        std::string Out;
        for (; *Text != 0; ++Text)
            Out += oracle::Format("%04x", (unsigned int)(unsigned short)*Text);
        return Out;
    }

    std::string Rgba(unsigned char const* C)
    {
        return oracle::Format("%02x%02x%02x%02x", C[0], C[1], C[2], C[3]);
    }

    // The recording pipeline: SetRenderState only.
    class FOracleCtlPipeline final : public IRenderPipeline
    {
    public:
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Result) override
        {
            *Result = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        GENRESULT NEURONCOMPONENTS_API SetRenderState(D3DRENDERSTATETYPE State, int Value) override
        {
            CtlEvent(oracle::Format("\"ev\":\"rs\",\"s\":\"%d\",\"v\":\"%d\"", (int)State, Value));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetRenderState(D3DRENDERSTATETYPE State, int* Out) override
        {
            *Out = 0;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetTextureStageState(int Stage, int Type, int Value) override
        {
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetTextureStageTexture(int Stage, void* Handle) override
        {
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetPipelineState(int Id, int Value) override { return GR_OK; }
        GENRESULT NEURONCOMPONENTS_API QueryDeviceAbility(unsigned int Index, unsigned long* Result) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API CreateTexture(int Width, int Height, unsigned int* PixelFormat,
                                                     unsigned int Levels, unsigned int Flags,
                                                     void** OutHandle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API DestroyTexture(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetTextureFormat(void* Handle, int* OutFormat) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetTextureLevelData(void* Handle, int Level, int SrcW, int SrcH,
                                                           unsigned int Pitch, unsigned int* SrcFormat,
                                                           const void* Pixels, void* A,
                                                           void* Palette) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Startup(const char* SectionName) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API ShutdownPipeline() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetPipelineState(int Id, int* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetDeviceInfo(void* Out520) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetDisplayModeCount(unsigned int* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetDisplayModeEnum(
            unsigned int Index, void* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SelectMode(
            int* Mode, unsigned int* OutIndex) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API CreateBuffers(
            HWND Hwnd, int* Mode, int* OutMode) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetModeBlob(
            void* OutAdapter, void* OutMode10Dwords) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API DestroyGpuResources() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API ClearBuffers(
            unsigned long Flags, const void* Rects) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SwapBuffers() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API LockBuffer(void* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API UnlockBuffer() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetBufferInterface(
            void* Out, void* Unused) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetDeviceStats(void* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetViewportRect(
            int X, int Y, int W, int H) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetViewportRect(
            int* OutX, int* OutY, int* OutW, int* OutH) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetDepthRange(float MinZ, float MaxZ) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetDepthRange(
            float* OutMinZ, float* OutMaxZ) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetWindow(
            HWND Hwnd, int X, int Y, int W, int H) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetWindow(
            HWND* OutHwnd, int* OutLeft, int* OutRight, int* OutW, int* OutH) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetWorld(const float* M12) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetWorld(float* Out12) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetView(const float* M12) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetView(float* Out12) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetModelview(const float* M12) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetModelview(float* Out12) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetProjection(const float* M16) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetProjection(float* Out16) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetLookat(
            float EyeX, float EyeY, float EyeZ, float AtX, float AtY, float AtZ, float UpX, float UpY,
            float UpZ) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetOrtho(
            float Left, float Right, float Bottom, float Top, float NearPlane,
            float FarPlane) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetPerspective(
            float FovDeg, float Aspect, float NearPlane, float FarPlane) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetLight(
            void* Handle, const D3DLIGHT8* Light) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API DestroyLight(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetLight(
            void* Handle, D3DLIGHT8* OutLight) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetLightEnable(
            void* Handle, int Enable) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetLightEnable(
            void* Handle, int* OutEnable) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetMaterial(
            const D3DMATERIAL8* Material) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetMaterial(D3DMATERIAL8* Material) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API IsTexture(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API LockTexture(
            void* Handle, int Level, void* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API UnlockTexture(void* Handle, int Level) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetTextureDim(
            void* Handle, unsigned int* OutW, unsigned int* OutH,
            int* OutLevels) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetTextureInterface(
            void* Handle, void* UnusedA, void* UnusedB) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API BlitTexture(
            void* DstHandle, int DstLevel, void* SrcHandle, int SrcLevel) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetRenderTarget(
            void* Handle, void* UnusedA, void* UnusedB) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetRenderTarget(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API BeginScene() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API EndScene() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API ResetRenderStatesToDefaults() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetTextureStageState(
            int Stage, int Type, int* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetTextureStageTransform(
            int Stage, const float* Matrix16) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetTextureStageTransform(
            int Stage, float* OutMatrix16) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetTextureStageTexture(
            int Stage, void** OutHandle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API VerifyState() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API DrawPrimitive(
            unsigned long Prim, unsigned int Fvf, const void* Verts, unsigned int Count,
            void* Unused) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API DrawIndexedPrimitiveUp(
            unsigned long Prim, unsigned int Fvf, const void* Verts, unsigned int VertCount,
            const void* Indices, unsigned int IndexCount,
            unsigned long Unused) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API DrawPrimitiveVb(
            unsigned long Prim, void* Vb, unsigned int StartVertex, unsigned int Count,
            void* Unused) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API DrawIndexedPrimitiveVb(
            unsigned long Prim, void* Vb, unsigned int BaseVertex, unsigned int NumVerts,
            const void* Indices, unsigned int IndexCount,
            unsigned long Unused) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API AddLight(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API RemoveLight(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API UpdateLight(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetWorldN(
            int WorldIndex, const float* M12) override { return GR_GENERIC; }
    };

    // The parent every control is created under: slot 23 and SetProperty recorded.
    class FOracleCtlParent final : public WinControl
    {
    public:
        std::string Arg(uintptr_t A) const
        {
            if (GCtlObject != nullptr &&
                (A == reinterpret_cast<uintptr_t>(GCtlObject) ||
                 A == reinterpret_cast<uintptr_t>(GCtlObject->AsWinControl())))
                return "self";
            return oracle::Format("%lx", (unsigned long)A);
        }
        bool Slot23(unsigned Msg, unsigned A, unsigned B) override
        {
            std::string Text;
            if (Msg == 0xf001u || Msg == 0xf002u)
                Text = Units(reinterpret_cast<wchar_t const*>(static_cast<uintptr_t>(A)));
            else
                Text = Arg(A);
            CtlEvent(oracle::Format("\"ev\":\"msg\",\"m\":\"%x\",\"a\":\"%s\",\"b\":\"%d\"", Msg, Text.c_str(),
                                    (int)B));
            return true;
        }
        intptr_t SetProperty(int Msg, uintptr_t A, uintptr_t B) override
        {
            CtlEvent(oracle::Format("\"ev\":\"prop\",\"m\":\"%x\",\"a\":\"%s\",\"b\":\"%d\"", Msg, Arg(A).c_str(),
                                    (int)B));
            return 0;
        }
    };

    // Peeks at the scroll bar's protected fields.
    class FUiScrollPeek : public UiScroll
    {
    public:
        static FUiScrollPeek* Of(Win3D* Object) { return static_cast<FUiScrollPeek*>(static_cast<UiScroll*>(
                                                      static_cast<WinControl*>(Object))); }
        std::string State() const
        {
            FScrollHitRect const* const Rects[5] = {&TopArrow_, &BottomArrow_, &TrackBefore_, &TrackAfter_,
                                                    &Thumb_};
            std::string RectText;
            for (int Index = 0; Index < 5; ++Index)
            {
                RectText += (Index == 0 ? "" : " ") + Bits(Rects[Index]->X0) + " " + Bits(Rects[Index]->Y0) +
                            " " + Bits(Rects[Index]->X1) + " " + Bits(Rects[Index]->Y1) +
                            oracle::Format(" %d", Rects[Index]->bHot ? 1 : 0);
            }
            float const Values[5] = {Value_, RangeMin_, RangeMax_, Page_, Step_};
            return oracle::Format("\"ev\":\"scroll\",\"flags\":\"%02x\",\"pos\":\"", Flags) + Bits3(Position) +
                   "\",\"v\":\"" + BitsN(Values, 5) + "\",\"rect\":\"" + RectText + "\",\"arrow\":\"" +
                   Bits(ArrowSize_) + oracle::Format("\",\"drag\":\"%d\",\"rt\":\"", DragPart_) +
                   Bits(RepeatTimer_) + oracle::Format("\",\"dragging\":\"%d\",\"doff\":\"", bDragging_ ? 1 : 0) +
                   Bits(DragOffset_) + "\",\"inv\":\"" + BitsN(InverseLayout_, 12) +
                   oracle::Format("\",\"colors\":\"%08lx %08lx %08lx %08lx\",\"soff\":\"", ArrowIdleColor_,
                                  ArrowHotColor_, BarIdleColor_, BarHotColor_) +
                   Bits3(ScrollOffset_) + "\",\"dpage\":\"" + Bits(DefaultPage_) + "\"";
        }
        void SetActive(bool b) { bActive_ = b; }
        void SetCustom(bool b) { bCustomColor_ = b; }
    };

    std::vector<std::string> Split(const std::string& Text, const std::string& Separator)
    {
        std::vector<std::string> Out;
        size_t Start = 0;
        for (;;)
        {
            size_t const Found = Text.find(Separator, Start);
            Out.push_back(Text.substr(Start, Found == std::string::npos ? std::string::npos : Found - Start));
            if (Found == std::string::npos)
                return Out;
            Start = Found + Separator.size();
        }
    }

    float F(const std::string& Hex)
    {
        return oracle::FloatFromBits((unsigned int)strtoul(Hex.c_str(), nullptr, 16));
    }

    int I(const std::string& Text)
    {
        return (int)strtol(Text.c_str(), nullptr, 10);
    }

    std::vector<wchar_t> WideFromHex(const std::string& Hex)
    {
        std::vector<wchar_t> Out;
        for (size_t Index = 0; Index + 3 < Hex.size(); Index += 4)
            Out.push_back((wchar_t)strtoul(Hex.substr(Index, 4).c_str(), nullptr, 16));
        Out.push_back(0);
        return Out;
    }

    std::string BytesFromHex(const std::string& Hex)
    {
        std::string Out;
        for (size_t Index = 0; Index + 1 < Hex.size(); Index += 2)
            Out += (char)strtoul(Hex.substr(Index, 2).c_str(), nullptr, 16);
        return Out;
    }

    void ListState(ListBox* List)
    {
        float const Timers[2] = {List->Timer890, List->Timer894};
        CtlEvent(oracle::Format("\"ev\":\"list\",\"hover\":\"%d\",\"f80\":\"%d\",\"sel\":\"%d\",\"count\":\"%d\","
                                "\"first\":\"%d\",\"rows\":\"%d\",\"t\":\"",
                                List->HoverIndex, List->Field80, List->SelectedIndex, List->ItemCount,
                                List->FirstVisible, List->VisibleRows) +
                 BitsN(Timers, 2) + "\",\"w\":\"" + Bits(List->Width) + "\",\"h\":\"" + Bits(List->Height) + "\"");
    }

    void ObjectState(Win3D* Object)
    {
        // UIWinObject's own fields are private: +0xe8 the origin, +0xf4 the corner.
        unsigned char const* const Raw = reinterpret_cast<unsigned char const*>(Object);
        float Origin[3];
        float Corner[3];
        memcpy(Origin, Raw + 0xe8, sizeof(Origin));
        memcpy(Corner, Raw + 0xf4, sizeof(Corner));
        CtlEvent(oracle::Format("\"ev\":\"obj\",\"flags\":\"%02x\",\"pos\":\"", Object->Flags) +
                 Bits3(Object->Position) + "\",\"orient\":\"" + BitsN(Object->Orientation, 9) +
                 "\",\"origin\":\"" + Bits3(Origin) + "\",\"corner\":\"" + Bits3(Corner) + "\"");
    }

    void RunCase(const std::string& Line, FOracleCtlParent* Parent, std::string& Class)
    {
        for (const std::string& Op : Split(Line, " ; "))
        {
            std::vector<std::string> T = Split(Op, " ");
            const std::string& Name = T[0];
            Win3D* const O = GCtlObject;
            ListBox* const List = (O != nullptr && (Class == "UIListBox")) ? static_cast<ListBox*>(O) : nullptr;
            UiScroll* const Scroll = (O != nullptr && (Class == "UIScroll" || Class == "UICycle"))
                                         ? static_cast<UiScroll*>(static_cast<WinControl*>(O))
                                         : nullptr;
            if (Name == "new")
            {
                Class = T[1];
                Win3D* Created = nullptr;
                if (Class == "UIListBox")
                {
                    FUiPayloadBounds P = {F(T[2]), F(T[3])};
                    Created = WinFactoryCreate("C", Class.c_str(), Parent, &P);
                }
                else if (Class == "UIScroll" || Class == "UICycle")
                {
                    FUiPayloadScrollCycle P;
                    memset(&P, 0, sizeof(P));
                    P.Right = F(T[2]);
                    P.Bottom = F(T[3]);
                    P.Extra0 = F(T[4]);
                    P.Extra1 = F(T[5]);
                    P.Extra2 = F(T[6]);
                    P.Extra3 = F(T[7]);
                    P.Flag = (unsigned char)I(T[8]);
                    Created = WinFactoryCreate("C", Class.c_str(), Parent, &P);
                }
                else if (Class == "UIWinObject")
                {
                    std::string Path = BytesFromHex(T.size() > 5 ? T[5] : "");
                    std::vector<char> Buffer(Path.begin(), Path.end());
                    Buffer.push_back(0);
                    FUiPayloadWinObject P = {F(T[2]), F(T[3]), Buffer.data(), F(T[4])};
                    Created = WinFactoryCreate("C", Class.c_str(), Parent, &P);
                }
                GCtlObject = Created;
                CtlEvent(oracle::Format("\"ev\":\"new\",\"ok\":\"%d\"", Created != nullptr ? 1 : 0));
            }
            else if (O == nullptr)
                continue;
            else if (Name == "xform")
            {
                float M[12];
                for (int Index = 0; Index < 12; ++Index)
                    M[Index] = F(T[1 + Index]);
                O->SetTransform(M);
            }
            else if (Name == "pos")
            {
                float P[3] = {F(T[1]), F(T[2]), F(T[3])};
                O->SetPosition(P);
            }
            else if (Name == "ray")
            {
                float* const Ray = Win3DMouseRayStorage();
                Ray[0] = F(T[1]);
                Ray[1] = F(T[2]);
                Ray[2] = F(T[3]);
            }
            else if (Name == "cursor")
            {
                GCtlCursor[0] = I(T[1]);
                GCtlCursor[1] = I(T[2]);
            }
            else if (Name == "dt")
                GCtlFrameTime = F(T[1]);
            else if (Name == "pulse")
                NnUiPulseAdvance(F(T[1]));
            else if (Name == "opt")
                GCtlOption = I(T[1]);
            else if (Name == "update")
                O->Update();
            else if (Name == "draw")
                O->Draw();
            else if (Name == "over")
            {
                float Ray[3];
                Win3DGetMouseRay(Ray);
                O->MouseOver(Ray);
            }
            else if (Name == "leave")
            {
                float Ray[3];
                Win3DGetMouseRay(Ray);
                O->MouseLeave(Ray);
            }
            else if (Name == "btn")
            {
                FWin3DMouseEvent Event = {I(T[1]), I(T[2])};
                bool const bResult = O->MouseEvent(&Event);
                CtlEvent(oracle::Format("\"ev\":\"btn\",\"r\":\"%d\"", bResult ? 1 : 0));
            }
            else if (Name == "hit")
            {
                float Ray[3];
                Win3DGetMouseRay(Ray);
                bool const bResult = O->HitTest(Ray);
                CtlEvent(oracle::Format("\"ev\":\"hit\",\"r\":\"%d\"", bResult ? 1 : 0));
            }
            else if (Name == "state")
            {
                if (List != nullptr)
                    ListState(List);
                else if (Scroll != nullptr)
                    CtlEvent(FUiScrollPeek::Of(O)->State());
                else
                    ObjectState(O);
            }
            else if (List != nullptr && Name == "item")
            {
                std::vector<wchar_t> Text = WideFromHex(T[1]);
                int const Result = List->AddItem(Text.data(), I(T[2]));
                CtlEvent(oracle::Format("\"ev\":\"r\",\"v\":\"%d\"", Result));
            }
            else if (List != nullptr && Name == "settext")
            {
                std::vector<wchar_t> Text = WideFromHex(T[2]);
                bool const bResult = List->SetItemText(I(T[1]), Text.data());
                CtlEvent(oracle::Format("\"ev\":\"r\",\"v\":\"%d\"", bResult ? 1 : 0));
            }
            else if (List != nullptr && Name == "data")
                CtlEvent(oracle::Format("\"ev\":\"r\",\"v\":\"%d\"", List->GetItemData(I(T[1]))));
            else if (List != nullptr && Name == "f80")
                List->Field80 = I(T[1]);
            else if (List != nullptr && Name == "frame")
                List->bDrawFrame = (unsigned char)I(T[1]);
            else if (List != nullptr && Name == "rules")
                List->bDrawRowRules = (unsigned char)I(T[1]);
            else if (List != nullptr && Name == "rows")
                List->VisibleRows = I(T[1]);
            else if (List != nullptr && Name == "first")
                List->FirstVisible = I(T[1]);
            else if (List != nullptr && Name == "sel")
                List->bSelectable = (unsigned char)I(T[1]);
            else if (Scroll != nullptr && Name == "range")
                Scroll->SetRange(F(T[1]), F(T[2]));
            else if (Scroll != nullptr && Name == "value")
                Scroll->SetValue(F(T[1]));
            else if (Scroll != nullptr && Name == "page")
                Scroll->SetPage(F(T[1]));
            else if (Scroll != nullptr && Name == "step")
                Scroll->SetStep(F(T[1]));
            else if (Scroll != nullptr && Name == "size")
                Scroll->SetSize(F(T[1]), F(T[2]));
            else if (Scroll != nullptr && Name == "stepmode")
                Scroll->SetStepMode(I(T[1]) != 0);
            else if (Scroll != nullptr && Name == "horiz")
                Scroll->SetHorizontal(I(T[1]) != 0);
            else if (Scroll != nullptr && Name == "custom")
                FUiScrollPeek::Of(O)->SetCustom(I(T[1]) != 0);
            else if (Scroll != nullptr && Name == "active")
                FUiScrollPeek::Of(O)->SetActive(I(T[1]) != 0);
            else if (Scroll != nullptr && Name == "soff")
            {
                float V[3] = {F(T[1]), F(T[2]), F(T[3])};
                Scroll->SetScrollOffset(V);
            }
            else if (Scroll != nullptr && Name == "offset")
            {
                float V[3] = {F(T[1]), F(T[2]), F(T[3])};
                intptr_t const Result = Scroll->SetProperty(6, reinterpret_cast<uintptr_t>(V), 0);
                CtlEvent(oracle::Format("\"ev\":\"r\",\"v\":\"%d\"", (int)Result));
            }
            else if (Scroll != nullptr && Name == "prop")
            {
                intptr_t const Result = Scroll->SetProperty(
                    (int)strtol(T[1].c_str(), nullptr, 16), (uintptr_t)(intptr_t)I(T[2]), (uintptr_t)(intptr_t)I(T[3]));
                CtlEvent(oracle::Format("\"ev\":\"r\",\"v\":\"%d\"", (int)Result));
            }
            else if (Name == "flags")
                static_cast<WinObject*>(O)->DrawFlags = (unsigned char)I(T[1]);
            else if (Name == "tint")
            {
                unsigned long const Packed = strtoul(T[1].c_str(), nullptr, 16);
                WinObject* const W = static_cast<WinObject*>(O);
                W->TintRgba[0] = (unsigned char)(Packed >> 24);
                W->TintRgba[1] = (unsigned char)(Packed >> 16);
                W->TintRgba[2] = (unsigned char)(Packed >> 8);
                W->TintRgba[3] = (unsigned char)Packed;
            }
        }
    }

    void ControlsSuite()
    {
        oracle::Suite Out("ui_controls.jsonl");
        GCtlOut = &Out;
        FOracleCtlPipeline Pipeline;
        IRenderPipeline* const SavedPipeline = NeuronRuntime::Pipeline;
        NeuronRuntime::Pipeline = &Pipeline;
        GCtlCase = 0;
        for (const std::string& Line : oracle::InputLines("ui_controls.list"))
        {
            GCtlObject = nullptr;
            GCtlFrameTime = 0.0;
            GCtlCursor[0] = 0;
            GCtlCursor[1] = 0;
            GCtlOption = 1;
            NnUiPulseReset();
            float* const Ray = Win3DMouseRayStorage();
            Ray[0] = 0.0f;
            Ray[1] = 0.0f;
            Ray[2] = -1.0f;
            FOracleCtlParent* const Parent = new FOracleCtlParent();
            Parent->Create(nullptr, nullptr);
            std::string Class;
            RunCase(Line, Parent, Class);
            GCtlOut = nullptr;
            while (Win3DGetCapture() != nullptr)
                Win3DRemoveCapture(Win3DGetCapture());
            if (GCtlObject != nullptr)
            {
                GCtlObject->Destroy();
                delete GCtlObject;
            }
            GCtlObject = nullptr;
            Parent->Destroy();
            delete Parent;
            GCtlOut = &Out;
            ++GCtlCase;
        }
        GCtlOut = nullptr;
        NeuronRuntime::Pipeline = SavedPipeline;
    }
}

// ---- the recording stand-ins of the immediate-mode layer (win3d_immediate.cpp, star_sphere.cpp)

void Win3DBegin(int PrimitiveMode)
{
    CtlEvent(oracle::Format("\"ev\":\"begin\",\"p\":\"%d\"", PrimitiveMode));
}
void Win3DEnd()
{
    CtlEvent("\"ev\":\"end\"");
}
void Win3DVertex3f(float X, float Y, float Z)
{
    CtlEvent("\"ev\":\"v\",\"x\":\"" + Bits(X) + "\",\"y\":\"" + Bits(Y) + "\",\"z\":\"" + Bits(Z) + "\"");
}
void Win3DColor3ub(unsigned char R, unsigned char G, unsigned char B)
{
    CtlEvent(oracle::Format("\"ev\":\"c3\",\"c\":\"%02x%02x%02x\"", R, G, B));
}
void Win3DColor4ubv(const unsigned char* Rgba4)
{
    CtlEvent("\"ev\":\"c4\",\"c\":\"" + Rgba(Rgba4) + "\"");
}
void Win3DTexCoord2f(float U, float V)
{
    CtlEvent("\"ev\":\"tc\",\"u\":\"" + Bits(U) + "\",\"v\":\"" + Bits(V) + "\"");
}
void Win3DDisable(int Capability)
{
    CtlEvent(oracle::Format("\"ev\":\"dis\",\"cap\":\"%x\"", Capability));
}
void Win3DBindTexture(void* TextureHandle)
{
    CtlEvent(oracle::Format("\"ev\":\"tex\",\"h\":\"%lx\"", (unsigned long)(uintptr_t)TextureHandle));
}
void Win3DPushProjection()
{
    CtlEvent("\"ev\":\"push\"");
}
void Win3DPopProjection()
{
    CtlEvent("\"ev\":\"pop\"");
}
void Win3DSetOrtho2D()
{
    CtlEvent("\"ev\":\"ortho\"");
}
void Win3DBindModelview(void* /*ObjectOrNull*/, float const* Matrix12)
{
    CtlEvent("\"ev\":\"mv\",\"m\":\"" + BitsN(Matrix12, 12) + "\"");
}

// ---- the font strings (font_string.cpp, font_imp.cpp, font_atlas.cpp): the list's rows store
// their text and record their paints; the "ListText" font has a 16-pixel line and 7-pixel
// characters.

namespace
{
    class FOracleCtlFont final : public IFont
    {
    public:
        char const* Slot00() override { return "ListText"; }
        char const* GetName() override { return "ListText"; }
        float GetFixedHeight() override { return 16.0f; }
        HFONT__* Slot03() override { return nullptr; }
        tagLOGFONTA const* GetLogFont() override { return nullptr; }
        float GetGlyphAdvance(wchar_t /*Ch*/) override { return 7.0f; }
        int GetLineHeight() override { return 16; }
        int Slot07() override { return 16; }
        void MeasureString(int* OutWH, wchar_t const* Text, int Len) override
        {
            int Count = 0;
            if (Text != nullptr)
                for (; (Len < 0 || Count < Len) && Text[Count] != 0; ++Count)
                {
                }
            OutWH[0] = 7 * Count;
            OutWH[1] = 16;
        }
        int ProbeString(int MaxWidth, wchar_t const* Text, int Len) override { return 0; }
        int GetCharWidths(wchar_t const* Text, int Len, int* OutWidths) override { return 0; }
    };

    FOracleCtlFont GCtlFont;
}

IFont* FindMenuFontImp(char const* /*Nickname*/)
{
    return &GCtlFont;
}

void FontAtlasSetDrawColor(unsigned PackedArgb)
{
    CtlEvent(oracle::Format("\"ev\":\"color\",\"c\":\"%08x\"", PackedArgb));
}

FontStringChild::FontStringChild()
    : Text_(nullptr)
    , Capacity_(0)
    , Font_(nullptr)
    , RefCount_(1)
    , GlyphHead_(nullptr)
    , CachedW_(0)
    , CachedH_(0)
    , LayoutGen_(0)
    , Color_(0xff080808u)
    , bShadow_(0)
{
}
FontStringChild::~FontStringChild()
{
    free(Text_);
}
int FontStringChild::AddRef()
{
    return ++RefCount_;
}
int FontStringChild::Release()
{
    int const Count = --RefCount_;
    if (Count == 0)
        delete this;
    return Count;
}
void FontStringChild::BindFont(IFont* Font)
{
    Font_ = Font;
}
IFont* FontStringChild::GetFont() const
{
    return Font_;
}
wchar_t const* FontStringChild::GetText() const
{
    return Text_;
}
void FontStringChild::SetText(wchar_t const* Text)
{
    // font_string.cpp's early outs: the same text (a null read as empty) changes nothing, so a
    // new row given an empty text keeps a null text; null empties the text.
    wchar_t const* const Left = Text != nullptr ? Text : L"";
    wchar_t const* const Right = Text_ != nullptr ? Text_ : L"";
    size_t Same = 0;
    while (Left[Same] != 0 && Left[Same] == Right[Same])
        ++Same;
    if (Left[Same] == Right[Same])
        return;
    if (Text == nullptr)
    {
        Text_[0] = 0;
        return;
    }
    free(Text_);
    Text_ = nullptr;
    size_t Length = 0;
    while (Text[Length] != 0)
        ++Length;
    Text_ = (wchar_t*)malloc((Length + 1) * sizeof(wchar_t));
    memcpy(Text_, Text, (Length + 1) * sizeof(wchar_t));
}
int* FontStringChild::GetSize(int* OutWH)
{
    OutWH[0] = 0;
    OutWH[1] = 0;
    return OutWH;
}
void FontStringChild::SetColor(unsigned const* Argb)
{
    Color_ = *Argb;
}
void FontStringChild::SetShadow(unsigned char Value)
{
    bShadow_ = Value;
}
void FontStringChild::Paint(void* Origin, void* DrawCtx, void* DrawArg)
{
    float const* const O = static_cast<float const*>(Origin);
    float Left = 0.0f;
    float Right = 0.0f;
    memcpy(&Left, &DrawCtx, sizeof(Left));
    memcpy(&Right, &DrawArg, sizeof(Right));
    CtlEvent("\"ev\":\"paint\",\"t\":\"" + Units(Text_) + "\",\"x\":\"" + Bits(O[0]) + "\",\"y\":\"" + Bits(O[1]) +
             "\",\"l\":\"" + Bits(Left) + "\",\"r\":\"" + Bits(Right) + "\"");
}

// ---- the screen projection of text objects (text_win.cpp): a fixed mapping in place of the
// Win3D camera -- x = 400 X + 320, y = 240 - 400 Y, extents 400 times the world's.

void TextWinProjectPoint(float const* World, float* OutX, float* OutY)
{
    *OutX = static_cast<float>(static_cast<double>(World[0]) * 400.0 + 320.0);
    *OutY = static_cast<float>(240.0 - static_cast<double>(World[1]) * 400.0);
}

void TextWinProjectExtent(float Width, float Height, float /*Z*/, float* OutWidth, float* OutHeight)
{
    *OutWidth = static_cast<float>(static_cast<double>(Width) * 400.0);
    *OutHeight = static_cast<float>(static_cast<double>(Height) * 400.0);
}

// ---- the frame, the cursor and the options (globals.cpp, input_manager.cpp, game_options.cpp)

double GetClientFrameTimeCapped()
{
    return GCtlFrameTime;
}

void GetClientSoftwareCursor(int* OutX, int* OutY, int* OutZ)
{
    *OutX = GCtlCursor[0];
    *OutY = GCtlCursor[1];
    if (OutZ != nullptr)
        *OutZ = 0;
}

int GetGameOption(int Index)
{
    return Index == 0x16 ? GCtlOption : 1;
}

// ---- the scroll bars' shapes (texture_shapes.cpp): the three names of UITextures.ini, whose
// GPU handles are 0x101 (the arrow), 0x102 (the caps) and 0x103 (the middle).

namespace
{
    FTexShape GCtlShapes[3];
}

FTexShape* FShapeDB::Find(char const* Name, char const* /*SourceFile*/)
{
    if (strcmp(Name, "HUD_sliderarrow") == 0)
        return &GCtlShapes[0];
    if (strcmp(Name, "HUD_slidertextopbottom") == 0)
        return &GCtlShapes[1];
    if (strcmp(Name, "HUD_slidertexmid") == 0)
        return &GCtlShapes[2];
    return nullptr;
}

void* TexShapeGetGpuHandle(FTextureRef const* Ref, int /*Frame*/)
{
    for (int Index = 0; Index < 3; ++Index)
        if (Ref == &GCtlShapes[Index])
            return reinterpret_cast<void*>(static_cast<uintptr_t>(0x101 + Index));
    return nullptr;
}

// ---- the mesh holder (client_mesh_load.cpp) and the engine submits (win3d_engine_submit.cpp)

bool ClientMeshLoadFromPath(FClientMeshHolder* Holder, char const* Path, char const* MaterialPath, bool)
{
    CtlEvent("\"ev\":\"load\",\"path\":\"" + oracle::HexC(Path) + "\",\"mat\":\"" + oracle::HexC(MaterialPath) +
             "\"");
    Holder->EngineInstance = reinterpret_cast<void*>(static_cast<uintptr_t>(0x1000));
    return true;
}

bool ClientMeshHolderHasInstance(FMeshHolder const* Holder)
{
    return Holder->EngineInstance != nullptr &&
           Holder->EngineInstance != reinterpret_cast<void*>(static_cast<ptrdiff_t>(-1));
}

void ClientMeshHolderSetPosition(FMeshHolder* /*Holder*/, float const Position[3])
{
    CtlEvent("\"ev\":\"mpos\",\"x\":\"" + Bits(Position[0]) + "\",\"y\":\"" + Bits(Position[1]) + "\",\"z\":\"" +
             Bits(Position[2]) + "\"");
}

void ClientMeshHolderUpdate(FMeshHolder* /*Holder*/)
{
    CtlEvent("\"ev\":\"mupd\"");
}

void SubmitWin3DEngineInstance(FClientMeshHolder* /*Holder*/, void* /*ArchAnchor*/, unsigned char /*bFrontCull*/,
                               float /*Scale*/, unsigned long /*Flags*/)
{
    CtlEvent("\"ev\":\"submit\"");
}

void SubmitWin3DTintedEngineInstanceWrapped(void* /*ArchAnchor*/, void* /*EngineInstance*/,
                                            unsigned char /*bFrontCull*/, unsigned char const ColorRgba[4],
                                            unsigned char bLightingEnable, float /*Scale*/,
                                            unsigned long /*Flags*/)
{
    CtlEvent("\"ev\":\"tinted\",\"tint\":\"" + Rgba(ColorRgba) + oracle::Format("\",\"l\":\"%d\"", bLightingEnable));
}

// ---- the math of the UI_PAGE widgets (ui_page.cpp) and of the HUD (hud_target.cpp), as the
// reference writes them, over Common's math engine.

I3DMathEngine* UiWidgetMathEngine()
{
    static I3DMathEngine* Math = nullptr;
    if (Math == nullptr)
        Math = Acquire3DMathEngine();
    return Math;
}

void UiWidgetProjectExtent(float const Matrix[9], float Z, float Width, float Height, float* OutWidth,
                           float* OutHeight)
{
    FMatrix const& M = *reinterpret_cast<FMatrix const*>(Matrix);
    FVector Source;
    FVector WidthPoint;
    FVector Origin;
    Source.X = Width;
    Source.Y = 0.0f;
    Source.Z = Z;
    UiWidgetMathEngine()->TransformMatrix(WidthPoint, M, Source);
    Source.X = 0.0f;
    Source.Y = 0.0f;
    Source.Z = Z;
    UiWidgetMathEngine()->TransformMatrix(Origin, M, Source);
    *OutWidth = fabsf(WidthPoint.X - Origin.X);
    FVector HeightPoint;
    Source.X = 0.0f;
    Source.Y = Height;
    Source.Z = Z;
    UiWidgetMathEngine()->TransformMatrix(HeightPoint, M, Source);
    Source.X = 0.0f;
    Source.Y = 0.0f;
    Source.Z = Z;
    UiWidgetMathEngine()->TransformMatrix(Origin, M, Source);
    *OutHeight = fabsf(HeightPoint.Y - Origin.Y);
}

FVector* HudTransformPoint(FTransform const* Xform, FVector* Out, FVector const* In)
{
    FVector Result;
    UiWidgetMathEngine()->TransformXform(Result, *Xform, *In);
    *Out = Result;
    return Out;
}

// ---- the other client functions the compiled files name: the Win3D manager's startup, frame
// and draw services, WinControl's INI hook and tooltip feed, the mesh holder's teardown and the
// shape database. No case reaches them.

namespace Archetype
{
    FCliMaterialEntry* ResolveMaterialEntryFromFs(IFileSystem*, char const*, bool, unsigned)
    {
        return nullptr;
    }
    int ReleaseMaterialEntrySlot(FCliMaterialEntry**)
    {
        return 0;
    }
}

void ButtonMontageEnsure()
{
}
void ButtonMontageFlushBatch()
{
}
void ClientMeshHolderConstruct(FClientMeshHolder*)
{
}
void ClientUiMeshHolderRelease(FClientMeshHolder* Holder)
{
    Holder->EngineInstance = reinterpret_cast<void*>(static_cast<ptrdiff_t>(-1));
}
FMeshHolder::~FMeshHolder()
{
}
FClientMeshHolder::~FClientMeshHolder()
{
}
void EngineLightClearRegisteredList()
{
}
void FontStringConstruct(FFontString*)
{
}
void FontStringDestroy(FFontString*)
{
}
unsigned char GetClientMouseCursorDrawFlag()
{
    return 0;
}
int GetClientMouseWheel()
{
    return 0;
}
FSpaceCamera* GetWin3DSceneCamera()
{
    return nullptr;
}
FHudCmdRecord* HudCommandRecord()
{
    return nullptr;
}
void HudDestroyEventHandler(FHudCmdHandler*)
{
}
void HudRegisterCommandHandler(FHudCmdHandler*, unsigned)
{
}
void HudUnregisterCommandHandler(FHudCmdHandler*)
{
}
void ImeFocusDrawPass()
{
}
void MouseFrameUpdate()
{
}
void MouseInitCursors()
{
}
void MouseShutdown()
{
}
void NnRolloverTickGlobal(double)
{
}
void NnToolTipFeedHover(FToolTipHoverInfo const*)
{
}
void NnToolTipTickGlobal(double)
{
}
void OracleOnWinControlIni(char const*, Win3D*)
{
}
void SpaceCameraSetHalfFovX(FSpaceCamera*, float)
{
}
void SpaceCameraSetNearFar(FSpaceCamera*, float, float)
{
}
bool SpaceCameraSetViewport(FSpaceCamera*, const int*)
{
    return false;
}
void SpaceCameraSetViewportEx(FSpaceCamera*, const int*, int)
{
}
void ToolTipHoverDestroy(FToolTipHoverInfo*)
{
}
void UiOverlayPrepPipeline()
{
}
void Win3DApplyCamera(ICamera*)
{
}
void Win3DImmedBatchCreateAll()
{
}
void Win3DImmedBatchDestroyAll()
{
}
void Win3DImmedBatchFlushAll()
{
}
void Win3DImmedBatchReset()
{
}
void Win3DModelCacheClear()
{
}
bool Win3DRayHitsAabbHalfExtents(float const[3], float const[12], float const[3])
{
    return false;
}
void Win3DSetViewport(ICamera*, const char*, int)
{
}

char const kShapeDbHeaderLower[] = "s_cmn\\shapedb.h";
FShapeDB G_UiShapeDB;
FShapeDB::FShapeDB()
{
}
int FShapeDB::Load(char const*, IFileSystem*)
{
    return 0;
}
void FShapeDB::Clear()
{
}
FTextureRef::FTextureRef()
    : Texture(nullptr)
    , Field04(-1)
{
}
FTextureRef::~FTextureRef()
{
}

void RunUiControlSuites()
{
    ControlsSuite();
}
