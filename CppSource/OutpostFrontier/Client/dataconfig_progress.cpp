// Author: Giperion.

#include "dataconfig_progress.h"

#include "client_debris.h" // LoadDebrisArchFile (the routine)
#include "common_ini.h"
#include "common_misc.h" // GetFileSysPath
#include "neuroncomponents.h"
#include "NeuronRuntime.h"
#include "globals.h"
#include "font_imp.h"
#include "graph_library.h"
#include "hud_element_list.h"
#include "ids_strings.h"   // RegisterIdsResourceModule (the routine)
#include "menu_fonts_load.h"
#include "menu_sound_arch.h"
#include "sound_config.h"  // LoadClientSoundCfgIni (the routine)
#include "texture_shapes.h"
#include "vis_effect_arch.h"
#include "voice_profiles.h"

#include <stdlib.h>
#include <string.h>

namespace
{
    // .rdata 0x005e6a08, the source path the routine's FDUMP names.
    const char kMainCppPath[] = "Client\\main.cpp";

    // Every string field starts {0, ""} (the the branch..the branch static initializers);
    // bDrawScripts is the field, 01 in the PE image.
    FIntroConfigState GIntroConfigState = {
        {0u, {0}}, {0u, {0}}, {0u, {0}}, {0u, {0}},
        {{0u, {0}}, {0u, {0}}, {0u, {0}}, {0u, {0}}},
        {0u, {0}}, 0, true};

    // per-key store: a null value clears the field (00564c10: length 0, first byte
    // 0), otherwise the routine copies at most 0x3f chars and the length is stored in front.
    void ReadIntroPathValue(INI_Reader& Reader, FIntroPathString& Field)
    {
        char const* const Value = Reader.get_value_string();
        if (Value == nullptr)
        {
            Field.Length = 0;
            Field.Text[0] = '\0';
            return;
        }
        Field.Length = CopyBoundedString(Field.Text, 0x40, Value);
    }

    // (reached through thunk_FUN_00564b30 from the routine 0x005b0e7e). No reset
    // on entry: a key the file does not carry keeps its previous value (the static
    // initializers zero every field before the first load). An open failure only runs the
    // reader's cleanup (00564b99 -> 00564eb8): no logo, no pick.
    void LoadIntroConfigFromIni(char const* IntroIniPath)
    {
        INI_Reader Reader;
        // the argument -- already resolved by the caller -- goes
        // through GetFileSysPath against NeuronRuntime::Data a second time; the result is opened.
        char ResolvedPath[MAX_PATH];
        GetFileSysPath(ResolvedPath, IntroIniPath, NeuronRuntime::Data);
        if (!Reader.open(ResolvedPath, false))
            return;

        while (Reader.read_header())
        {
            if (Reader.is_header("Images") && Reader.read_value())
            {
                do
                {
                    if (Reader.is_value("TgaPath"))
                        ReadIntroPathValue(Reader, GIntroConfigState.TgaPath);
                    else if (Reader.is_value("ScreenType"))
                        ReadIntroPathValue(Reader, GIntroConfigState.ScreenType);
                    else if (Reader.is_value("ScreenExit"))
                        ReadIntroPathValue(Reader, GIntroConfigState.ScreenExit);
                    else if (Reader.is_value("ScreenReset"))
                        ReadIntroPathValue(Reader, GIntroConfigState.ScreenReset);
                    else if (Reader.is_value("Screen1"))
                        ReadIntroPathValue(Reader, GIntroConfigState.Screen[0]);
                    else if (Reader.is_value("Screen2"))
                        ReadIntroPathValue(Reader, GIntroConfigState.Screen[1]);
                    else if (Reader.is_value("Screen3"))
                        ReadIntroPathValue(Reader, GIntroConfigState.Screen[2]);
                    else if (Reader.is_value("Screen4"))
                        ReadIntroPathValue(Reader, GIntroConfigState.Screen[3]);
                    else if (Reader.is_value("bDrawScripts"))
                        GIntroConfigState.bDrawScripts = Reader.get_value_bool(0);
                    else
                        Reader.log_link("unknown ini parameter");
                } while (Reader.read_value());
            }
        }

        // the logo name, bounded at 0x3f chars, and its length.
        GIntroConfigState.Logo.Length =
            CopyBoundedString(GIntroConfigState.Logo.Text, 0x40, "FRONT_OutpostFrontierlogo.tga");
        // (rand() * 3) >> 15, i.e. 0, 1 or 2.
        GIntroConfigState.IntroBasePick = (rand() * 3) / 0x8000;
    }

    // One [Data] value of the routine (0x005b0ce3..0x005b0ee9): an is_value chain in listing
    // order. Every key but fonts / rich_fonts / intro resolves the value with
    // GetFileSysPath(path, value, 0) into the 0x104-byte frame buffer first; intro resolves
    // against NeuronRuntime::Data into a second buffer (ESP+0x1688). Loader results are dropped.
    void LoadClientDataValue(INI_Reader& Reader)
    {
        char Path[MAX_PATH];
        if (Reader.is_value("fonts"))
        {
            // the routine(value, NeuronRuntime::Data).
            LoadFonts(Reader.get_value_string(), NeuronRuntime::Data);
        }
        else if (Reader.is_value("rich_fonts"))
        {
            // the routine(value, NeuronRuntime::Data).
            LoadRichFontsFromIni(Reader.get_value_string(), NeuronRuntime::Data);
        }
        else if (Reader.is_value("sounds"))
        {
            // the routine(path, 0).
            GetFileSysPath(Path, Reader.get_value_string(), nullptr);
            LoadClientSoundArchIni(Path, false);
        }
        else if (Reader.is_value("voices"))
        {
            // the routine(path).
            GetFileSysPath(Path, Reader.get_value_string(), nullptr);
            LoadClientVoicesIni(Path);
        }
        else if (Reader.is_value("effects"))
        {
            // the routine(path, 0).
            GetFileSysPath(Path, Reader.get_value_string(), nullptr);
            EffectArchLoadIni(Path, false);
        }
        else if (Reader.is_value("effect_shapes"))
        {
            // the routine(path) -- queued, the routine drains it at the
            // end of InitializeEngine.
            GetFileSysPath(Path, Reader.get_value_string(), nullptr);
            AddEffectShapesFile(Path);
        }
        else if (Reader.is_value("debris"))
        {
            // the routine(path, 0).
            GetFileSysPath(Path, Reader.get_value_string(), nullptr);
            LoadDebrisArchFile(Path, false);
        }
        else if (Reader.is_value("intro"))
        {
            // GetFileSysPath(path, value, NeuronRuntime::Data), then
            // thunk_FUN_00564b30(path).
            char IntroPath[MAX_PATH];
            GetFileSysPath(IntroPath, Reader.get_value_string(), NeuronRuntime::Data);
            LoadIntroConfigFromIni(IntroPath);
        }
        else if (Reader.is_value("HUD"))
        {
            // the routine(path) stores the path only; the routine
            // instantiates the elements later.
            GetFileSysPath(Path, Reader.get_value_string(), nullptr);
            SetHudIniPath(Path);
        }
        else if (Reader.is_value("igraph"))
        {
            // the routine(path).
            GetFileSysPath(Path, Reader.get_value_string(), nullptr);
            GraphLibraryLoad(Path);
        }
    }
}

FIntroConfigState const* GetIntroConfigState()
{
    return &GIntroConfigState;
}

bool LoadClientIniData(char const* IniPath)
{
    // (module storage-005b12f8). 0x005b0c6b calls the routine (MOV AL,1 / RET)
    // and drops the result -- nothing to port.
    INI_Reader Reader;
    if (Reader.open(IniPath, false))
    {
        // / 0x005b0f98: one read_header per section; a false one ends the scan.
        while (Reader.read_header())
        {
            if (Reader.is_header("Data"))
            {
                // .
                if (Reader.read_value())
                {
                    do
                    {
                        LoadClientDataValue(Reader);
                    } while (Reader.read_value());
                }
            }
            else if (Reader.is_header("Resources"))
            {
                // every DLL key is LoadLibraryA'd; a failure is
                // reported (main.cpp line 0x22c) and the null handle is still pushed onto the
                // ids module list (the routine at 0x005b0f7f runs on both paths), so the
                // ids of the DLLs after it keep their index.
                if (Reader.read_value())
                {
                    do
                    {
                        if (Reader.is_value("DLL"))
                        {
                            HMODULE const Module = LoadLibraryA(Reader.get_value_string());
                            if (Module == nullptr)
                            {
                                char const* const Name = Reader.get_value_string();
                                DWORD const Error = GetLastError();
                                FDUMP(0x100001,
                                      "%s(%d) : *** ERROR: Couldn't load %s! Error code %d\n",
                                      kMainCppPath, 0x22c, Name, Error);
                            }
                            RegisterIdsResourceModule(Module);
                        }
                    } while (Reader.read_value());
                }
            }
        }

        // still inside the open success -- the routine on
        // GetFileSysPath("audio\\SoundCFG.ini", 0).
        char SoundCfgPath[MAX_PATH];
        GetFileSysPath(SoundCfgPath, "audio\\SoundCFG.ini", nullptr);
        LoadClientSoundCfgIni(SoundCfgPath);
    }
    // the reader's destructor, then MOV AL,1 on both exits.
    return true;
}
