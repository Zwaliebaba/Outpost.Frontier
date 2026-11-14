// Every library builds and links (Plan 0.2).
#include "Test.h"

#include "Core/Core.h"
#include "Data/Data.h"
#include "Sim/Sim.h"
#include "Platform/Platform.h"
#include "Render/Render.h"
#include "Audio/Audio.h"
#include "Video/Video.h"
#include "Anim/Anim.h"
#include "Ui/Ui.h"
#include "Scene/Scene.h"
#include "Script/Script.h"
#include "Net/Net.h"

TEST(EveryLibraryLinks)
{
    CHECK(Core::ModuleName() == "Core");
    CHECK(Data::ModuleName() == "Data");
    CHECK(Sim::ModuleName() == "Sim");
    CHECK(Platform::ModuleName() == "Platform");
    CHECK(Render::ModuleName() == "Render");
    CHECK(Audio::ModuleName() == "Audio");
    CHECK(Video::ModuleName() == "Video");
    CHECK(Anim::ModuleName() == "Anim");
    CHECK(Ui::ModuleName() == "Ui");
    CHECK(Scene::ModuleName() == "Scene");
    CHECK(Script::ModuleName() == "Script");
    CHECK(Net::ModuleName() == "Net");
}
