// Oracle suite of work package P4.1 (Oracle.exe game-app): the multiplayer account of a new
// install, Common's CPlayerAccount (common_playeraccount.cpp, compiled in), over
// crates/game-app/tests/fixtures/oracle/account.list. The Rust test
// crates/game-app/tests/oracle_vectors.rs renders the same lines.
//
// The machine and the registry are scripted per case: the source's calls of RegOpenKeyExA,
// RegOpenKeyA, RegQueryValueExA, RegCloseKey, RegCreateKeyExA, RegSetValueExA, UuidCreate,
// GetUserNameA, timeGetTime and OutputDebugStringA are redirected by macro to the stand-ins
// below, which answer from the case. RegQueryValueExA behaves as the system does for a value
// stored without a terminator (as SetTextKey stores them): a buffer smaller than the value
// fails with ERROR_MORE_DATA and the size it needs, a missing value with ERROR_FILE_NOT_FOUND
// and the size untouched.
//
// Common.lib exports CPlayerAccount too; this file defines it (COMMON_API empty), so the
// import library's members for it are never pulled. MD5Hash comes from oracle_util_md5.cpp.
//
// Input lines (hex fields: "." for no bytes):
//   seed <9x> <nt> <uuid> <user> <ms> <name>  -- 9x and nt: nokey (the key does not open),
//       none (no DigitalProductId) or the value; uuid 16 bytes; user "-" (GetUserNameA fails)
//       or the name; ms the counter; name "-" (null) or the name
//   setkey <key>                              -- SetTextKey on the key in a zeroed buffer
//   getkey <value>|nokey                      -- GetTextKey with InstallKey = value
//   load <name>|- <sig>|- <text>              -- LoadAccount (a value "-" is missing),
//       VerifyAccount and GetServerSignature(text)
// Output lines:
//   {"in","name","sig","key"} (GenerateAccount and GenerateTextKey)
//   {"in","ok"[,"install","name","sig"]} (SetTextKey and the three values it wrote)
//   {"in","ok"[,"key"]} (GetTextKey)
//   {"in","verify","server"}

#include <windows.h>
#include <mmsystem.h>
#include <rpc.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

#include "oracle_support.h"

#define NEURONRUNTIME_EXPORTS
#include "md5hash.h"
#undef NEURONRUNTIME_EXPORTS

#include "common_exports.h"
#undef COMMON_API
#define COMMON_API

using namespace oracle;

namespace
{
    // One case's machine and registry.
    struct FAccountCase
    {
        // DigitalProductId per key: absent from the map, the key does not open.
        std::map<std::string, Bytes> ProductIds;
        std::map<std::string, bool> HasProductId;
        unsigned char Uuid[16] = {};
        bool bUserName = false;
        std::string UserName;
        DWORD Milliseconds = 0;
        // HKCU values (the account key): present ones only.
        bool bAccountKey = true;
        std::map<std::string, Bytes> Values;
        // What RegSetValueExA wrote.
        std::map<std::string, std::string> Written;
    };

    FAccountCase GCase;

    const HKEY kKey9x = reinterpret_cast<HKEY>(static_cast<uintptr_t>(0x1001));
    const HKEY kKeyNt = reinterpret_cast<HKEY>(static_cast<uintptr_t>(0x1002));
    const HKEY kKeyAccount = reinterpret_cast<HKEY>(static_cast<uintptr_t>(0x1003));

    std::string KeyName(HKEY Key)
    {
        if (Key == kKey9x)
            return "9x";
        if (Key == kKeyNt)
            return "nt";
        return "account";
    }

    LONG OracleRegOpenKeyExA(HKEY Root, LPCSTR Path, DWORD, REGSAM, PHKEY Out)
    {
        *Out = nullptr;
        if (Root == HKEY_LOCAL_MACHINE)
        {
            const char* const Name =
                strcmp(Path, "Software\\Microsoft\\Windows\\CurrentVersion\\") == 0 ? "9x"
                : strcmp(Path, "Software\\Microsoft\\Windows NT\\CurrentVersion\\") == 0 ? "nt"
                                                                                         : nullptr;
            if (Name == nullptr || GCase.HasProductId.count(Name) == 0)
                return ERROR_FILE_NOT_FOUND;
            *Out = strcmp(Name, "9x") == 0 ? kKey9x : kKeyNt;
            return ERROR_SUCCESS;
        }
        if (!GCase.bAccountKey)
            return ERROR_FILE_NOT_FOUND;
        *Out = kKeyAccount;
        return ERROR_SUCCESS;
    }

    LONG OracleRegOpenKeyA(HKEY Root, LPCSTR Path, PHKEY Out)
    {
        return OracleRegOpenKeyExA(Root, Path, 0, KEY_QUERY_VALUE, Out);
    }

    LONG OracleRegQueryValueExA(HKEY Key, LPCSTR Name, LPDWORD, LPDWORD, LPBYTE Data, LPDWORD Size)
    {
        const Bytes* Value = nullptr;
        if (Key == kKey9x || Key == kKeyNt)
        {
            const std::string Which = KeyName(Key);
            if (strcmp(Name, "DigitalProductId") == 0 && GCase.HasProductId[Which])
                Value = &GCase.ProductIds[Which];
        }
        else if (Key == kKeyAccount)
        {
            const std::map<std::string, Bytes>::const_iterator Found = GCase.Values.find(Name);
            if (Found != GCase.Values.end())
                Value = &Found->second;
        }
        if (Value == nullptr)
            return ERROR_FILE_NOT_FOUND;
        const DWORD Need = static_cast<DWORD>(Value->size());
        if (*Size < Need)
        {
            *Size = Need;
            return ERROR_MORE_DATA;
        }
        if (Need != 0)
            memcpy(Data, Value->data(), Need);
        *Size = Need;
        return ERROR_SUCCESS;
    }

    LONG OracleRegCloseKey(HKEY)
    {
        return ERROR_SUCCESS;
    }

    LONG OracleRegCreateKeyExA(HKEY Root, LPCSTR Path, DWORD, LPSTR, DWORD, REGSAM, void*, PHKEY Out,
                               LPDWORD Disposition)
    {
        if (Disposition != nullptr)
            *Disposition = REG_OPENED_EXISTING_KEY;
        return OracleRegOpenKeyExA(Root, Path, 0, KEY_SET_VALUE, Out);
    }

    LONG OracleRegSetValueExA(HKEY, LPCSTR Name, DWORD, DWORD, const BYTE* Data, DWORD Size)
    {
        GCase.Written[Name] = std::string(reinterpret_cast<const char*>(Data), Size);
        return ERROR_SUCCESS;
    }

    RPC_STATUS OracleUuidCreate(UUID* Out)
    {
        memcpy(Out, GCase.Uuid, sizeof(GCase.Uuid));
        return RPC_S_OK;
    }

    BOOL OracleGetUserNameA(LPSTR Buffer, LPDWORD Size)
    {
        if (!GCase.bUserName || GCase.UserName.size() + 1 > *Size)
            return FALSE;
        memcpy(Buffer, GCase.UserName.c_str(), GCase.UserName.size() + 1);
        *Size = static_cast<DWORD>(GCase.UserName.size() + 1);
        return TRUE;
    }

    DWORD OracleTimeGetTime()
    {
        return GCase.Milliseconds;
    }

    void OracleOutputDebugStringA(LPCSTR)
    {
    }
}

#define RegOpenKeyExA OracleRegOpenKeyExA
#define RegOpenKeyA OracleRegOpenKeyA
#define RegQueryValueExA OracleRegQueryValueExA
#define RegCloseKey OracleRegCloseKey
#define RegCreateKeyExA OracleRegCreateKeyExA
#define RegSetValueExA OracleRegSetValueExA
#define UuidCreate OracleUuidCreate
#define GetUserNameA OracleGetUserNameA
#define timeGetTime OracleTimeGetTime
#define OutputDebugStringA OracleOutputDebugStringA
#include "../Common/common_playeraccount.cpp"
#undef RegOpenKeyExA
#undef RegOpenKeyA
#undef RegQueryValueExA
#undef RegCloseKey
#undef RegCreateKeyExA
#undef RegSetValueExA
#undef UuidCreate
#undef GetUserNameA
#undef timeGetTime
#undef OutputDebugStringA

namespace
{
    std::vector<std::string> Fields(const std::string& Line)
    {
        std::vector<std::string> Out;
        size_t Start = 0;
        while (Start <= Line.size())
        {
            const size_t End = Line.find(' ', Start);
            Out.push_back(Line.substr(Start, End == std::string::npos ? std::string::npos : End - Start));
            if (End == std::string::npos)
                break;
            Start = End + 1;
        }
        return Out;
    }

    // A hex field ("." for no bytes), without the terminator Unhex adds.
    Bytes HexField(const std::string& Field)
    {
        if (Field == ".")
            return Bytes();
        Bytes Data = Unhex(Field);
        Data.pop_back();
        return Data;
    }

    void SetProductId(const char* Which, const std::string& Field)
    {
        if (Field == "nokey")
            return;
        GCase.HasProductId[Which] = Field != "none";
        if (Field != "none")
            GCase.ProductIds[Which] = HexField(Field);
    }

    std::string Text(const Bytes& Data)
    {
        return std::string(reinterpret_cast<const char*>(Data.data()), Data.size());
    }

    void SeedCase(Suite& Out, const std::string& Line, const std::vector<std::string>& F)
    {
        SetProductId("9x", F[1]);
        SetProductId("nt", F[2]);
        const Bytes Uuid = HexField(F[3]);
        memcpy(GCase.Uuid, Uuid.data(), Uuid.size() < 16 ? Uuid.size() : 16);
        GCase.bUserName = F[4] != "-";
        if (GCase.bUserName)
            GCase.UserName = Text(HexField(F[4]));
        GCase.Milliseconds = static_cast<DWORD>(strtoul(F[5].c_str(), nullptr, 10));
        const std::string Name = F[6] == "-" ? std::string() : Text(HexField(F[6]));

        CPlayerAccount Account;
        Account.GenerateAccount(F[6] == "-" ? nullptr : Name.c_str());
        // GenerateTextKey writes 0x28 characters and the terminator.
        char Key[0x40] = {};
        const bool bKey = CPlayerAccount::GenerateTextKey(Key);
        Out.Line("{\"in\":\"" + Line + "\",\"name\":\"" + Account.GetAccountNameChar() + "\",\"sig\":\"" +
                 Account.GetAccountNameSigChar() + "\",\"key\":\"" + (bKey ? Key : "-") + "\"}");
    }

    void SetKeyCase(Suite& Out, const std::string& Line, const std::vector<std::string>& F)
    {
        // SetTextKey hashes 0x23 bytes and reads the text at 0x24 whatever the key's length; a
        // zeroed buffer holds every key of the list.
        char Key[0x100] = {};
        const Bytes Data = HexField(F[1]);
        memcpy(Key, Data.data(), Data.size() < sizeof(Key) - 1 ? Data.size() : sizeof(Key) - 1);
        const bool bOk = CPlayerAccount::SetTextKey(Key);
        if (!bOk)
        {
            Out.Line("{\"in\":\"" + Line + "\",\"ok\":\"0\"}");
            return;
        }
        Out.Line("{\"in\":\"" + Line + "\",\"ok\":\"1\",\"install\":\"" +
                 HexC(GCase.Written["InstallKey"].c_str()) + "\",\"name\":\"" + GCase.Written["MPAccountName"] +
                 "\",\"sig\":\"" + GCase.Written["MPAccountNameSig"] + "\"}");
    }

    void GetKeyCase(Suite& Out, const std::string& Line, const std::vector<std::string>& F)
    {
        GCase.bAccountKey = F[1] != "nokey";
        if (GCase.bAccountKey)
            GCase.Values["InstallKey"] = HexField(F[1]);
        char Key[0x40] = {};
        const bool bOk = CPlayerAccount::GetTextKey(Key);
        if (!bOk)
        {
            Out.Line("{\"in\":\"" + Line + "\",\"ok\":\"0\"}");
            return;
        }
        Out.Line("{\"in\":\"" + Line + "\",\"ok\":\"1\",\"key\":\"" + HexC(Key) + "\"}");
    }

    void LoadCase(Suite& Out, const std::string& Line, const std::vector<std::string>& F)
    {
        // LoadAccount reads into a 0x400-byte buffer the value does not terminate: each value
        // is stored with its terminator here, so FromString sees only the value.
        if (F[1] != "-")
        {
            Bytes Value = HexField(F[1]);
            Value.push_back(0);
            GCase.Values["MPAccountName"] = Value;
        }
        if (F[2] != "-")
        {
            Bytes Value = HexField(F[2]);
            Value.push_back(0);
            GCase.Values["MPAccountNameSig"] = Value;
        }
        const std::string Server = Text(HexField(F[3]));
        CPlayerAccount Account;
        Account.LoadAccount();
        const bool bVerify = Account.VerifyAccount();
        const std::wstring Signature = Account.GetServerSignature(Server.c_str());
        std::string Narrow;
        for (wchar_t Ch : Signature)
            Narrow.push_back(static_cast<char>(Ch));
        Out.Line("{\"in\":\"" + Line + "\",\"verify\":\"" + (bVerify ? "1" : "0") + "\",\"server\":\"" +
                 Narrow + "\"}");
    }
}

void RunGameAppSuites()
{
    RunAccountSuite();
    RunPerfOptionsSuite();
    RunEffectArchSuite();
}

void RunAccountSuite()
{
    Suite Out("account.jsonl");
    for (const std::string& Line : InputLines("account.list"))
    {
        GCase = FAccountCase();
        const std::vector<std::string> F = Fields(Line);
        if (F[0] == "seed" && F.size() == 7)
            SeedCase(Out, Line, F);
        else if (F[0] == "setkey" && F.size() == 2)
            SetKeyCase(Out, Line, F);
        else if (F[0] == "getkey" && F.size() == 2)
            GetKeyCase(Out, Line, F);
        else if (F[0] == "load" && F.size() == 4)
            LoadCase(Out, Line, F);
        else
        {
            fprintf(stderr, "account.list: bad line %s\n", Line.c_str());
            GFailed = true;
        }
    }
}
