// Shared pieces of the game-data dumps of work package P2.1
// (oracle_game_data.cpp, oracle_game_archetype.cpp, oracle_game_universe.cpp).

#pragma once

#include <set>
#include <string>

#include "oracle_support.h"

// One dump kind: <output>\<kind>\files.txt lists the files Path hands out.
class FGameKind
{
public:
    explicit FGameKind(const char* Name) : Name(Name), List(oracle::GOutputDir + "\\" + Name + "\\files.txt")
    {
    }

    std::string Path(const char* File)
    {
        List.Line(File);
        return oracle::DataOutputFile(Name, File);
    }

private:
    const char* Name;
    oracle::Suite List;
};

// A dump line built field by field: {"key":"value",...}.
class FLine
{
public:
    void Add(const char* Key, const std::string& Value);
    std::string Done() const;

private:
    std::string Text;
};

// Printable ASCII as it is except '"' and '\', every other byte as \u00xx.
std::string GameText(const char* Value);
// A float's bits, eight hex digits.
std::string GameF(float Value);
// Eight hex digits.
std::string GameId(unsigned int Value);

// oracle_game_archetype.cpp.
void DumpGoods(FGameKind& K);
void DumpLoadouts(FGameKind& K, const std::set<unsigned int>& LoadoutIds);
void DumpReputation(FGameKind& K);
void DumpArchetypes(FGameKind& K, const std::set<unsigned int>& MotorIds,
                    const std::set<unsigned int>& ExplosionIds);

// oracle_game_universe.cpp.
void DumpUniverse(FGameKind& K);
void DumpBaseData(FGameKind& K);
