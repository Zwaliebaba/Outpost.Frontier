// Author: Giperion.

#include "client_cliobj.h"     // FlushMaterialCache (the routine)
#include "msvcrt_compat.h" // MsvcrtAtoi: baseline MSVCRT.DLL wraps mod 2^32, the UCRT saturates
#include "client_frame_tick.h"
#include "client_ime_focus.h" // ReleaseClientIMEFocus (the routine)
#include "client_state.h"
#include "intro_connect.h"
#include "oracle_mode.h" // Oracle instrumentation (P0.5)

#include "common_ini.h"
#include "common_id.h"
#include "common_reputation.h"
#include "common_async.h"        // Async::Shutdown
#include "common_dataconfig.h"   // DataConfig::shutdown
#include "common_errorhandler.h" // ErrorHandler::activate / deactivate / new_log
#include "common_misc.h"         // GetUserLocalDataPath
#include "common_text.h"
#include "character_info.h"   // ShutdownCharacterInfo (the routine)
#include "chat_init.h"        // ChatShutdown (the routine)
#include "client_debris.h"    // ShutdownDebrisArchDb (the routine)
#include "connect_flow.h"
#include "effect_base.h"      // EffectSystemShutdown (the routine)
#include "vis_effect_arch.h"  // EffectArchShutdown (the routine)
#include "font_imp.h"         // FontShutdown (the routine)
#include "client_tick_profile.h" // Port-only diagnostics, released before fonts
#include "gameflow.h"
#include "globals.h"
#include "graph_library.h"
#include "neuroncomponents.h"
#include "NeuronRuntime.h"
#include "neuronruntime_net.h"
#include "ids_strings.h"
#include "loading_screen.h"   // LoadingScreenShutdown (the routine)
#include "menu_keymap_config.h"
#include "menu_sound_arch.h"
#include "menu_ui_init.h"
#include "login_mgr.h"
#include "mission_briefing.h"  // GetMissionBriefingState (the routine)
#include "nn_error.h"
#include "nn_modal.h"         // NnSystemShutdown (the routine)
#include "nn_montage_draw.h"  // ClearButtonMontage (the routine)
#include "nn_rollover.h"      // NnRolloverDestroyGlobal (the routine)
#include "nn_tooltip.h"       // GetNnToolTipSingleton (the field)
#include "space_state.h"      // GetSpaceState (the routine)
#include "win3d.h"
#include "server_connect.h"
#include "texture_shapes.h"   // ClearEffectShapesFiles (the routine)
#include "ui_scroll.h"
#include "voice_profiles.h"
#ifdef OUTPOST_FRONTIER_DX9RENDERER
#include "render_ex.h"        // RenderEx::Shutdown (port-only Direct3D 9 extensions)
#endif

#include <ctype.h>
#include <float.h>
#include <io.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

// "+G"/"-G" (the routine); read by CLoginMgr::Login (the routine), hence
// outside the anonymous namespace (declared in login_mgr.h).
bool G_bCommandLineG = false;

namespace
{
    // the application name the routine hands Dr. Watson (0x80 wchar, the last
    // one forced to zero at the field).
    wchar_t G_CrashAppName[0x80];

    // the '|'-separated list of files Dr. Watson attaches: the routine puts the
    // spew log path here, the routine appends the dxdiag report. 0x410 wchar, ending right
    // where G_CrashAppName begins in baseline's .data.
    wchar_t G_CrashReportFiles[0x410];

    // (module storage-005a54dd): run "dxdiag.exe <temp>\dxdiag.txt", wait up to 60 s
    // and, unless it timed out, append the report path to G_CrashReportFiles.
    void AppendDxDiagReport()
    {
        STARTUPINFOA StartupInfo;
        memset(&StartupInfo, 0, sizeof(StartupInfo));
        PROCESS_INFORMATION ProcessInfo;
        ProcessInfo.hProcess = nullptr;
        ProcessInfo.hThread = nullptr;
        ProcessInfo.dwProcessId = 0;
        StartupInfo.cb = 0x44;
        ProcessInfo.dwThreadId = 0;

        char ReportPath[0x104];
        GetTempPathA(0x104, ReportPath);
        strcat(ReportPath, "dxdiag.txt");

        char CommandLine[0x104];
        wsprintfA(CommandLine, "dxdiag.exe %s", ReportPath);
        if (CreateProcessA(nullptr, CommandLine, nullptr, nullptr, FALSE, 0x4000020, nullptr,
                           nullptr, &StartupInfo, &ProcessInfo) == 0)
            return;

        if (WaitForSingleObject(ProcessInfo.hProcess, 60000) != WAIT_TIMEOUT)
        {
            if (wcslen(G_CrashReportFiles) != 0)
                wcscat(G_CrashReportFiles, L"|");
            const size_t Length = wcslen(G_CrashReportFiles);
            MultiByteToWideChar(CP_UTF8, 0, ReportPath, -1, G_CrashReportFiles + Length,
                                static_cast<int>(0x410 - Length));
            // baseline's terminator lands 0x104 wchar past the old end, not after the
            // appended text.
            G_CrashReportFiles[Length + 0x104] = L'\0';
        }
        CloseHandle(ProcessInfo.hProcess);
        CloseHandle(ProcessInfo.hThread);
    }

    // (module storage-005a5909), the unhandled-exception filter the routine
    // installs. Breakpoint (0x80000003) and single-step (0x80000004) are left to the next handler.
    // Everything else: close a hosted DirectPlay session, then hand the crash to Dr. Watson
    // (dw15.exe, shipped next to OutpostFrontier.exe) through a 0x1c50-byte shared block and wait for
    // it. Returns EXCEPTION_EXECUTE_HANDLER unless dw15 says the process may continue.
    LONG WINAPI ClientCrashFilter(EXCEPTION_POINTERS* Info)
    {
        LONG Result = 1; // local_20
        const DWORD Code = Info->ExceptionRecord->ExceptionCode;
        if (Code == 0x80000003 || Code == 0x80000004)
            return 0;

        // guarded by its own __try with a filter that returns 1.
        __try
        {
            CDPServer::CrashCleanup();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }

        SECURITY_ATTRIBUTES Security;
        Security.lpSecurityDescriptor = nullptr;
        Security.nLength = 0xc;
        Security.bInheritHandle = TRUE;
        HANDLE Mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, &Security, PAGE_READWRITE, 0,
                                            0x1c50, nullptr);
        if (Mapping == nullptr)
            return Result;

        unsigned char* const Block =
            static_cast<unsigned char*>(MapViewOfFile(Mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0,
                                                      0, 0));
        if (Block != nullptr)
        {
            memset(Block, 0, 0x714 * 4);
            HANDLE const EventDone = CreateEventA(&Security, FALSE, FALSE, nullptr); // local_34
            if (EventDone != nullptr)
            {
                HANDLE const EventAlive = CreateEventA(&Security, FALSE, FALSE, nullptr); // local_38
                if (EventAlive != nullptr)
                {
                    HANDLE const Mutex = CreateMutexA(&Security, FALSE, nullptr); // local_3c
                    if (Mutex != nullptr)
                    {
                        if (DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(),
                                            GetCurrentProcess(),
                                            reinterpret_cast<HANDLE*>(Block + 0x24), 0x1f0fff,
                                            TRUE, 0) != 0)
                        {
                            DWORD* const Words = reinterpret_cast<DWORD*>(Block);
                            Words[1] = GetCurrentProcessId();
                            Words[2] = GetCurrentThreadId();
                            *reinterpret_cast<HANDLE*>(Block + 0x1c) = EventDone;
                            *reinterpret_cast<HANDLE*>(Block + 0x14) = EventAlive;
                            *reinterpret_cast<HANDLE*>(Block + 0x20) = Mutex;
                            Words[0] = 0x1c50;
                            *reinterpret_cast<EXCEPTION_POINTERS**>(Block + 0x10) = Info;
                            *reinterpret_cast<void**>(Block + 0xc) =
                                Info->ExceptionRecord->ExceptionAddress;
                            Words[0x34 / 4] = 1;
                            Words[0x3c / 4] = 0x10;
                            wcscpy(reinterpret_cast<wchar_t*>(Block + 0x48), G_CrashAppName);
                            strcpy(reinterpret_cast<char*>(Block + 0x1148),
                                   "Microsoft\\PCHealth\\ErrorReporting\\DW");
                            strcpy(reinterpret_cast<char*>(Block + 0x1040),
                                   "HKLM\\Software\\Microsoft\\Microsoft Games\\OutpostFrontier\\1.0\\"
                                   "DigitalProductID");

                            HKEY Key;
                            if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                                              "Software\\Microsoft\\Microsoft Games\\OutpostFrontier\\1.0",
                                              0, KEY_QUERY_VALUE, &Key) == ERROR_SUCCESS)
                            {
                                DWORD LangId;
                                DWORD Size = 4;
                                if (RegQueryValueExA(Key, "LangID", nullptr, nullptr,
                                                     reinterpret_cast<BYTE*>(&LangId),
                                                     &Size) == ERROR_SUCCESS)
                                    Words[0x1144 / 4] = LangId + 0x400;
                                RegCloseKey(Key);
                            }

                            Words[0x28 / 4] = 0x100;
                            memcpy(Block + 0x1210, L"common.dll", 22);

                            char ModulePath[0x104];
                            GetModuleFileNameA(nullptr, ModulePath, 0x104);
                            MultiByteToWideChar(CP_UTF8, 0, ModulePath, -1,
                                                reinterpret_cast<wchar_t*>(Block + 0xb8), 0x104);

                            AppendDxDiagReport();
                            memcpy(Block + 0x1418, G_CrashReportFiles,
                                   wcslen(G_CrashReportFiles) * 2 + 2);

                            ErrorHandler::deactivate();

                            STARTUPINFOA StartupInfo;
                            memset(&StartupInfo, 0, sizeof(StartupInfo));
                            StartupInfo.cb = 0x44;
                            PROCESS_INFORMATION ProcessInfo;
                            ProcessInfo.hProcess = nullptr;
                            ProcessInfo.hThread = nullptr;
                            ProcessInfo.dwProcessId = 0;
                            ProcessInfo.dwThreadId = 0;

                            char Arguments[0x104];
                            wsprintfA(Arguments, " -x -s %u", reinterpret_cast<UINT_PTR>(Mapping));
                            if (CreateProcessA("dw15.exe", Arguments, nullptr, nullptr, TRUE,
                                               0x4000020, nullptr, nullptr, &StartupInfo,
                                               &ProcessInfo) == 0)
                            {
                                Result = 0;
                            }
                            else
                            {
                                // .
                                bool bWaiting = true;
                                do
                                {
                                    if (WaitForSingleObject(EventDone, 20000) == WAIT_OBJECT_0)
                                    {
                                        if (WaitForSingleObject(EventAlive, 1) == WAIT_OBJECT_0)
                                        {
                                            bWaiting = false;
                                            if (Words[0x2c / 4] == 0x10)
                                                Result = 0;
                                        }
                                    }
                                    else
                                    {
                                        const DWORD MutexWait = WaitForSingleObject(Mutex, 20000);
                                        if (MutexWait == WAIT_TIMEOUT)
                                        {
                                            bWaiting = false;
                                        }
                                        else if (MutexWait == WAIT_ABANDONED)
                                        {
                                            bWaiting = false;
                                            ReleaseMutex(Mutex);
                                        }
                                        else
                                        {
                                            if (WaitForSingleObject(EventDone, 1) != WAIT_OBJECT_0)
                                            {
                                                SetEvent(EventAlive);
                                                bWaiting = false;
                                            }
                                            else if (WaitForSingleObject(EventAlive, 1) ==
                                                     WAIT_OBJECT_0)
                                            {
                                                bWaiting = false;
                                            }
                                            ReleaseMutex(Mutex);
                                        }
                                    }
                                } while (bWaiting);
                            }
                        }
                        CloseHandle(Mutex);
                    }
                    CloseHandle(EventAlive);
                }
                CloseHandle(EventDone);
            }
            UnmapViewOfFile(Block);
        }
        CloseHandle(Mapping);
        return Result;
    }

    // (module storage-005a593c).
    void InstallClientCrashFilter(const wchar_t* AppName)
    {
        wcsncpy(G_CrashAppName, AppName, 0x80);
        G_CrashAppName[0x7f] = L'\0';
        SetUnhandledExceptionFilter(ClientCrashFilter);
    }

    // installed into NeuronRuntime::FatalErrorFn by WinMain (0x005b30e8). analysis left it
    // as data; inspected from the PE bytes. Loads caption 0xd1 and the message Id from the
    // resources, appends Text, drops the render device (the routine(1)), forces the cursor
    // visible, shows the message box and exits.
    void __cdecl ClientFatalError(unsigned int Id, const char* Text)
    {
        wchar_t Caption[0x100];
        if (GetIdsStringWWithRawFallback(0xd1, Caption, 0x100) == 0)
            wcscpy(Caption, L"OutpostFrontier Fatal Error");

        wchar_t Message[0x1000];
        if (GetIdsStringWWithRawFallback(Id, Message, 0x1000) == 0)
            wcscpy(Message, L"OutpostFrontier has encountered a fatal error and will now terminate.\n");

        if (Text != nullptr)
        {
            wcscat(Message, L"\n\n");
            const size_t Length = wcslen(Message);
            MultiByteToWideChar(CP_ACP, 0, Text, -1, Message + Length,
                                static_cast<int>(0x1000 - Length));
        }

        ShutdownRenderMgr(true);
        while (ShowCursor(TRUE) < 0)
        {
        }
        MessageBoxW(g_client.window, Message, Caption, 0x10030);
        exit(1);
    }

    // installed into NeuronRuntime::CriticalWarningFn (0x005b30f4). Same text assembly
    // with caption 0xd2; a Yes/No box. "No" drops the render device and exits; "Yes" puts the
    // cursor count back and returns true.
    bool __cdecl ClientCriticalWarning(unsigned int Id, const char* Text)
    {
        wchar_t Caption[0x100];
        if (GetIdsStringWWithRawFallback(0xd2, Caption, 0x100) == 0)
            wcscpy(Caption, L"OutpostFrontier Critical Warning");

        wchar_t Message[0x1000];
        if (GetIdsStringWWithRawFallback(Id, Message, 0x1000) == 0)
            wcscpy(Message, L"OutpostFrontier has encountered an unknown issue.\n");

        if (Text != nullptr)
        {
            wcscat(Message, L"\n\n");
            const size_t Length = wcslen(Message);
            MultiByteToWideChar(CP_ACP, 0, Text, -1, Message + Length,
                                static_cast<int>(0x1000 - Length));
        }

        int ExtraShows = 0;
        if (ShowCursor(TRUE) < 0)
        {
            do
            {
                ++ExtraShows;
            } while (ShowCursor(TRUE) < 0);
        }

        const bool bContinue = MessageBoxW(g_client.window, Message, Caption, 0x10024) == IDYES;
        if (!bContinue)
        {
            ShutdownRenderMgr(true);
            exit(1);
        }

        if (ExtraShows >= 0)
        {
            for (int Count = ExtraShows + 1; Count != 0; --Count)
                ShowCursor(FALSE);
        }
        return bContinue;
    }

    // the assert callback WinMain hands ErrorHandler::activate. In a fullscreen
    // session (the field) with a registered Win3D (the routine) the message goes to the
    // NN_Error modal the routine and the result is 4 + (its bool) (005b0b1c..005b0b29);
    // otherwise MessageBoxA.
    int __cdecl ClientAssertHandler(const char* Message, const char* Caption, bool bOkCancel)
    {
        if (g_display.fullscreen != 0 && Win3DGetFirstRegistered() != nullptr)
            return NnErrorRunModalMessage(Message) ? 5 : 4;
        const UINT Flags = (bOkCancel ? 2u : 0x12u) | 2u;
        return MessageBoxA(nullptr, Message, Caption, Flags);
    }

    // (module storage-005b2d55), cdecl(FClientCommandLine*): "+G"/"-G", "+P<port>"
    // and "+S<host:port>" in either case, the sign ignored. A token without a sign matches
    // nothing.
    void ApplyClientCommandLineFlags(const FClientCommandLine* CommandLine)
    {
        for (unsigned Index = 0; Index < CommandLine->Count; ++Index)
        {
            const char* Token = CommandLine->Argv[Index];
            char Letter = 0;
            if (Token[0] == '+' || Token[0] == '-')
            {
                Letter = static_cast<char>(toupper(Token[1]));
                Token += 2;
            }

            if (Letter == 'G' || Letter == 'g')
            {
                G_bCommandLineG = true;
            }
            else if (Letter == 'P' || Letter == 'p')
            {
                const int Port = MsvcrtAtoi(Token);
                if (Port != 0)
                    CDPClient::SetSourcePort(static_cast<unsigned long>(Port));
            }
            else if (Letter == 'S' || Letter == 's')
            {
                // the routine on the connect context the field; its
                // answer is not read.
                G_LoginMgr.AddDirectServer(Token);
            }
        }
    }

    // WinMain's exception filter (module storage-005b33c6): the routine(1), ShowCursor(TRUE),
    // EXCEPTION_CONTINUE_SEARCH.
    int ClientWinMainFilter()
    {
        ShutdownRenderMgr(true);
        ShowCursor(TRUE);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // registered with atexit right after CoInitialize (0x005b3101), so it runs
    // after WinMain has returned. Calls are in baseline's order; a call the port has no baseline
    // counterpart for yet is named in place.
    void __cdecl ClientAtExitTeardown()
    {
        CGunWrapper::Shutdown();
        // the routine -- delete the loading screen's Image (the field) and its
        // progress bar tree (the field), set the field.
        LoadingScreenShutdown();
        Async::Shutdown();
        if (g_client.current_state != nullptr)
        {
            // slot 13 (+0x34, Destroy) of the current state object,
            // then the field = 0.
            g_client.current_state->Destroy();
            g_client.current_state = nullptr;
        }
        ShutdownServerConnection();
        G_LoginMgr.Reset();         // on the field
        G_LoginMgr.DestroyGun();    // on the field
        // Singleton slot 9 (+0x24, Shutdown) on the routine / the routine / the routine /
        // (0x005b21d2..0x005b21ff). IntroFocus's is 0x00401e90, a bare RET; GF's
        // -> the routine, the space state's the routine, MissionBriefing's
        // .
        GetIntroFocusState()->Shutdown();
        GetGameflowState()->Shutdown();
        GetSpaceState()->Shutdown();
        GetMissionBriefingState()->Shutdown();
        ShutdownCharacterInfo();    // (0x005b2202)
        // the ToolTip singleton the field -- slot 10 (Destroy), then,
        // re-read and still set, slot 0 with 1 (the scalar deleting dtor, the routine clears
        // when it is this tooltip).
        if (GetNnToolTipSingleton() != nullptr)
        {
            GetNnToolTipSingleton()->Destroy();
            NnToolTipControl* const Tip = GetNnToolTipSingleton();
            if (Tip != nullptr)
                delete Tip;
        }
        NnRolloverDestroyGlobal();  // (0x005b2228)
        ChatShutdown();            // (0x005b222d; the field..88, the routine)
        NnSystemShutdown();         // (0x005b2232)
        ClearButtonMontage();       // (0x005b2237)
        // the the field tree -- the routine inlined (null -> skip;
        // each child through the routine with the next sibling read first; slot 10; deleting
        // dtor). The root is always null in baseline (globals.h).
        Win3DDeleteTree(G_WinDebugRoot);
        Win3DShutdownManager(); // (0x005b2271)
        ShutdownUiSoundTable();
        StopAllClientSounds();  // (0x005b227b)
        DataConfig::shutdown();
        GraphLibraryShutdown();
        EffectSystemShutdown();   // (0x005b228b)
        ClearEffectShapesFiles(); // (0x005b2290)
        UiScrollClearShapes();
        ShutdownClientTickProfile(); // CLIENT-BETA-LABEL: release cached atlas owners first
        FontShutdown(); // (0x005b229a)
        ShutdownRichText();
        ShutdownDebrisArchDb(); // (0x005b22a5)
        EffectArchShutdown();      // (0x005b22aa)
        ClearClientVoices();
        ShutdownClientSoundArch();
        ShutdownRenderMgr(false);  // (0)
#ifdef OUTPOST_FRONTIER_DX9RENDERER
        // Port-only: the Direct3D 9 extension objects and interfaces go before the pipeline.
        RenderEx::Shutdown();
#endif
        // the routine: frees every heap option object and list node of the
        // PerfOptions std::list (the field). The port's registry is the static G_Options
        // table (perf_options.cpp), so there is nothing to free; nothing after this point reads
        // the registry in baseline either.
        if (Reputation::Vibe::Count() != 0)
        {
            FDUMP(0x100002, "%s(%d) : *** WARNING: %d REP_INSTANCE(s) orphaned\n",
                  "Client\\main.cpp", 0x4be, Reputation::Vibe::Count());
        }
        Archetype::FlushMaterialCache(); // (0x005b22f6), result discarded
        AleLibShutdown();                // (ALELib::Shutdown, 0x005b22fb)
        NeuronRuntime::CloseData();
        NeuronRuntime::Shutdown(g_client.window);
        G_LoginMgr.Cleanup(); // CDPClient::Cleanup on the field
        FreeLibrary(g_client.resources);
        g_client.resources = nullptr;
        ErrorHandler::deactivate();
        CFLIDMaker::Shutdown();
        StringShutdown();
        ReleaseClientIMEFocus(); // (0x005b2349): delete the field
        CoUninitialize();
    }
}

// (module storage): `MOV dword ptr [ECX],0; RET`.
FClientCommandLine::FClientCommandLine()
{
    Count = 0;
}

// (module storage-005b34aa), __thiscall, RET 4.
void FClientCommandLine::Parse(const char* CommandLine)
{
    static const char kDelimiters[] = " \t";

    Count = 0;
    strcpy(Buffer, CommandLine);

    char* Cursor = Buffer;
    for (;;)
    {
        Cursor += strspn(Cursor, kDelimiters);
        char* const Token = Cursor;
        unsigned Length = 0;
        char Ch = *Cursor;
        while (Ch != '\0')
        {
            ++Cursor;
            if (strchr(kDelimiters, Ch) != nullptr)
                break;
            if (Ch == '"')
            {
                // Copy up to the closing quote; a missing one ends the token (and the scan).
                char Inner = *Cursor;
                if (Inner == '\0')
                    break;
                bool bClosed = false;
                do
                {
                    ++Cursor;
                    if (Inner == '"')
                    {
                        bClosed = true;
                        break;
                    }
                    Token[Length] = Inner;
                    ++Length;
                    Inner = *Cursor;
                } while (Inner != '\0');
                if (!bClosed)
                    break;
            }
            else
            {
                Token[Length] = Ch;
                ++Length;
            }
            Ch = *Cursor;
        }

        Token[Length] = '\0';
        if (Token[0] == '\0')
            return;
        Argv[Count] = Token;
        ++Count;
    }
}

// developer FPU-exception-trap toggle, gated on "+X"/"-X" (G_bFpExceptionTrap).
// Enabling unmasks FP exceptions (crash on NaN/overflow/etc.); baseline calls this every frame with
// Enable=false at frame-begin (the routine) and Enable=true at frame-end (the routine), in
// addition to the one-time WinMain call.
void SetFpuExceptionTrap(bool Enable)
{
    if (Enable)
    {
        _clearfp();
        const unsigned int Prev = _controlfp(0, 0);
        _control87(Prev & 0xffffffe3u, 0x8001f);
    }
    else
    {
        _control87(0x9001f, 0x8001f);
    }
}

// (module storage-005b2880).
void OpenSpewLog()
{
    char Path[0x104];
    if (!GetUserLocalDataPath(Path))
        return;
    strcat(Path, "\\FLSpew.txt");
    if (ErrorHandler::new_log(Path, -1, 7, false, false) != nullptr)
        MultiByteToWideChar(CP_UTF8, 0, Path, -1, G_CrashReportFiles, 0x104);
}

// Client/main.cpp WinMain.
int WINAPI WinMain(HINSTANCE instance, HINSTANCE /*prev*/, LPSTR cmd, int /*show*/)
{
    int ExitCode = 1; // local_20

    // The whole body runs under a frame handler (scope table module storage) whose filter
    // (0x005b33a9) drops the render device and shows the cursor, then lets the exception go on.
    __try
    {
        // the baseline single-instance guard (named mutex; null handle or
        // ERROR_ALREADY_EXISTS 0xb7 -> return 1). DEVIATION (deviations.md "Client: single-instance
        // guard behind FL_SINGLE_INSTANCE"): compiled only with FL_SINGLE_INSTANCE defined, so the
        // owner can run several clients on one machine for multiplayer tests.
#if defined(FL_SINGLE_INSTANCE)
        HANDLE mutex = CreateMutexA(nullptr, FALSE, "OutpostFrontierClient");
        if (mutex == nullptr || GetLastError() == ERROR_ALREADY_EXISTS)
        {
            CloseHandle(mutex);
            return 1;
        }
#endif

        char ModulePath[MAX_PATH];
        GetModuleFileNameA(nullptr, ModulePath, MAX_PATH);
        char Drive[_MAX_DRIVE];
        char Dir[_MAX_DIR];
        _splitpath(ModulePath, Drive, Dir, nullptr, nullptr);
        char Folder[MAX_PATH];
        _makepath(Folder, Drive, Dir, nullptr, nullptr);
        SetCurrentDirectoryA(Folder);

        // Probes data\ next to the exe, after the chdir above (baseline probed ..\DATA).
        if (_access("data\\CHARACTERS\\ANIMATIONS\\FACIALMALE.ANM", 0) != 0)
        {
            STARTUPINFOA StartupInfo;
            memset(&StartupInfo, 0, sizeof(StartupInfo));
            StartupInfo.cb = 0x44;
            PROCESS_INFORMATION ProcessInfo;
            memset(&ProcessInfo, 0, sizeof(ProcessInfo));
            if (CreateProcessA("OutpostServer.exe", nullptr, nullptr, nullptr, FALSE, 0x20, nullptr,
                               nullptr, &StartupInfo, &ProcessInfo))
            {
                CloseHandle(ProcessInfo.hProcess);
                CloseHandle(ProcessInfo.hThread);
            }
            return 1;
        }

        // The baseline EULA gate (EBUEula.dll showing EULA.rtf / Warranty.rtf) is removed.

        // Global NeuronRuntime data exports ?FatalErrorFn@@3P6AXIPBD@ZA / ?CriticalWarningFn@@3P6A_NIPBD@ZA
        // (not in namespace NeuronRuntime), stored at 0x005b30e2 / 0x005b30ee.
        ::FatalErrorFn = ClientFatalError;
        ::CriticalWarningFn = ClientCriticalWarning;
        CoInitialize(nullptr);
        atexit(ClientAtExitTeardown);
        g_client.instance = instance;
        g_client.resources = LoadLibraryA("UiText.dll");
        RegisterIdsResourceModule(g_client.resources);

        unsigned int Major;
        unsigned int Minor;
        unsigned int Build;
        if (NEURONCOMPONENTS_GetDllVersion(ModulePath, &Major, &Minor, &Build) == 0)
        {
            InstallClientCrashFilter(L"OutpostFrontier");
        }
        else
        {
            // .
            Major = 1;
            Minor = 0;
            Build = 0xb;
        }
        g_client.packed_version = (((Minor & 0xFFu) | (Major << 8)) << 16) | (Build & 0xFFFFu);

        FClientCommandLine CommandLine;
        CommandLine.Parse(cmd);
        OracleParseCommandLine(&CommandLine); // Oracle instrumentation (P0.5): `+oracle`
        ApplyClientCommandLineFlags(&CommandLine);
        LoadStartupConfig(&g_display, &CommandLine);
        ErrorHandler::activate(g_display.ini_path.c_str(), ClientAssertHandler);
        OpenSpewLog();
        // (&CommandLine, ini path): [OutpostFrontier] local_server / AppGUID, [ListServer]
        // hostname / port and [Login] name / pwd / char into the connect context. The command
        // line argument is not read.
        G_LoginMgr.LoadConfig(g_display.ini_path.c_str());
        if (G_bFpExceptionTrap)
            SetFpuExceptionTrap(true);

        WNDCLASSA WindowClass;
        char ClassName[] = "OutpostFrontier";
        WindowClass.style = CS_DBLCLKS;
        WindowClass.lpfnWndProc = ClientWndProc;
        WindowClass.cbClsExtra = 0;
        WindowClass.cbWndExtra = 0;
        WindowClass.hInstance = instance;
        WindowClass.hIcon = LoadIconA(instance, MAKEINTRESOURCEA(0x67));
        WindowClass.hCursor = LoadCursorA(nullptr, IDC_ARROW);
        WindowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        WindowClass.lpszMenuName = nullptr;
        WindowClass.lpszClassName = ClassName;
        RegisterClassA(&WindowClass);

        g_client.window = CreateGameWindow(ClassName, &g_display, "OutpostFrontier");
        ShowCursor(FALSE);

        if (InitializeEngine(&CommandLine))
        {
            ClientNoteEngineReady(); // = 1; the routine(0) if a close came early

            // the routine(the routine(), nullptr) into the field.
            g_client.current_state = EnterClientState(ComputeInitialEnvironment(), nullptr);

            RegisterAcceleratorGroup("IDR_ALWAYS_PRESENT");

            for (;;)
            {
                UpdateClientTiming();
                const double DeltaSeconds = GetClientFrameTimeCapped();
                if (!PumpMessages(&ExitCode))
                    break;
                if (ClientPresentAllowed())
                    TickClientFrame(DeltaSeconds);
            }
        }
    }
    __except (ClientWinMainFilter())
    {
        // unreachable, the filter never accepts.
    }

    // baseline WinMain returns straight away; the mutex is left to process exit and the teardown
    // runs from atexit (ClientAtExitTeardown).
    return ExitCode;
}
