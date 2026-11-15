// Author: Giperion.

#include "login_mgr.h"
#include "msvcrt_compat.h" // MsvcrtAtoi/MsvcrtWtoi: baseline MSVCRT.DLL wraps mod 2^32, the UCRT saturates

#include "character_select_equip.h" // ApplyCharacterSelectEquipFields (the routine tail)
#include "common_ini.h"
#include "common_misc.h"   // ParseGUID
#include "common_server.h" // IServer
#include "neuroncomponents.h"         // FDUMP
#include "globals.h"       // g_client.packed_version (the field)
#include "perf_options.h"  // GetPerfOptionSlot (the routine)
#include "player_spawn.h"
#include "server_filter.h" // ServerFilter::Match (GetFilteredServers)
#include "reg_key_stack.h" // FRegKeyStack (login history)
#include "vc6_sort.h"      // Vc6Sort::Sort (GetFilteredServers' inlined VC6 std::sort)

#include <algorithm>
#include <malloc.h>        // _alloca (ReadRegHexString)
#include <mmsystem.h>      // timeGetTime
#include <stdio.h>
#include <time.h>          // time (SaveLogin)
#include <stdlib.h>        // atoi, _wtoi, wcstombs (Read)
#include <string.h>
#include <wchar.h>

// inet_addr / ntohl (Read, module storage / 005ab88f): declared by the winsock.h that
// windows.h pulls in (WIN32_LEAN_AND_MEAN is not defined in this build).
#pragma comment(lib, "ws2_32.lib")

// constructed by the static initializer module storage, destroyed through
// the atexit thunk the branch (`MOV ECX,0067e7bc; JMP the routine`).
CLoginMgr G_LoginMgr;

bool G_bServerDisconnected = false;
bool G_bServerConnected = false;

// / the field -- both 01 in the PE image; only TypeSelect's multiplayer pick
// (0x00566d6b..0x00566d7a) writes them.
unsigned char G_bEnumerateLanHosts = 1;
unsigned char G_bInternetServerList = 1;

namespace
{
    char const kLoginMgrSource[] = "Client\\LoginMgr.cpp";

    // (cdecl) -- a REG_SZ value of the top key, decoded by HexToWide into Out.
    // False when the value is missing, not REG_SZ, its size is -1, or the second read does
    // not return exactly the size the first one reported.
    bool ReadRegHexString(FRegKeyStack& Keys, std::wstring& Out, char const* Name)
    {
        DWORD Size = 0;
        DWORD Type = 0;
        if (RegQueryValueExA(Keys.Top(), Name, nullptr, &Type, nullptr, &Size) != ERROR_SUCCESS ||
            Type != REG_SZ || Size == 0xffffffffu)
        {
            return false;
        }

        // _alloca((Size + 3) & ~3).
        BYTE* const Buffer = static_cast<BYTE*>(_alloca((Size + 3) & ~3u));
        DWORD Length = Size;
        DWORD Type2 = 0;
        LONG const Result = RegQueryValueExA(Keys.Top(), Name, nullptr, &Type2, Buffer, &Length);
        DWORD const Read = (Result == ERROR_SUCCESS && Type2 == REG_SZ) ? Length : 0xffffffffu;
        if (Read != Size)
        {
            return false;
        }

        wchar_t Wide[0x400];                                           // [EBP-0x804], 0x800 bytes
        HexToWide(reinterpret_cast<char const*>(Buffer), Wide);
        Out = Wide;
        return true;
    }

    // (cdecl) -- ForEachFaction's callback: the record pushed onto the vector the
    // context points at (the VC6 insert(end, 1, record) inline: grow to size + max(size, 1)
    // when full, the routine copies), then true -- the walk always goes on. The id is not read.
    bool __cdecl PushFactionRecord(unsigned /*NicknameId*/, FNewCharFaction const* Faction,
                                   void* Context)
    {
        static_cast<std::vector<FNewCharFaction>*>(Context)->push_back(*Faction);
        return true;
    }
}

// Inline in the CLoginMgr constructor for +0x94 (module storage-005a8ce2).
FServerInfo::FServerInfo()
    : IpAddress(-1)
    , Port(0)
    , Field_0x28(0)
    , Ping(-2)
    , LastResponseTime(0)
    , Field_0x34(0)
    , ResponseCount(0)
    , Field_0x3c(0)
    , Field_0x3d(0)
    , Field_0x3e(0)
    , Field_0x40(0)
    , PlayerCount(0)
    , MaxPlayers(0)
    , Version(0)
    , Id(0)
    , bPassword(0)
    , Field_0x95(1)
    , Field_0x96(1)
    , Field_0x98(0)
{
}

// (on +0x248, the map). +0x00 is not written by the CLoginMgr constructor
// (0x005a8eab..0x005a8f02); the baseline object is static, so CurrentId starts at 0.
FLoginHistory::FLoginHistory()
    : CurrentId(0)
{
}


bool FLoginHistory::Contains(unsigned int Id) const
{
    return Records.find(Id) != Records.end();
}

// `LEA EAX,[ESP+4]; PUSH EAX; ADD ECX,4; CALL the routine` (operator[]).
FLoginRecord& FLoginHistory::GetRecord(unsigned int Id)
{
    return Records[Id];
}

// `PUSH ECX; ADD ECX,4; CALL the routine; MOV [EAX+0x20],param_1`: the key
// pointer is the history object, i.e. &CurrentId.
void FLoginHistory::SetTime(unsigned int Time)
{
    Records[CurrentId].Time = Time;
}

// the same `PUSH ECX` key as SetTime; the RET-4 argument is never read.
unsigned int FLoginHistory::GetTime(unsigned int IgnoredId)
{
    (void)IgnoredId;
    return Records[CurrentId].Time;
}

// called from the CLoginMgr constructor (0x005a8f02).
bool FLoginHistory::Load()
{
    Records.clear();                                                   // the routine

    // RegOpenKeyExA(HKCU, root, KEY_READ) pushed on the stack that starts
    // with HKEY_CURRENT_USER; a failed open or a null key ends the load.
    FRegKeyStack Keys;
    if (Keys.Open(G_OutpostFrontierRegKey, KEY_READ) == nullptr)
    {
        return true;
    }

    // EBP: baseline loads it from [ESP+0x1c] (0x005b6340), a slot only the failed-"time" path
    // below writes, so a first record without a readable "id" is keyed by whatever that stack
    // slot held. The port starts from 0.
    unsigned int Id = 0;
    for (int Index = 1; ; ++Index)                                     // / 005b65a7
    {
        char Name[0x2000];                                             // , [ESP+0x198]
        _snprintf(Name, 0x2000, "MP\\Logins\\Login%d", Index);

        // Top(), depth < 32, RegOpenKeyExA(KEY_READ), pushed, non-null.
        if (Keys.Open(Name, KEY_READ) == nullptr)
        {
            break;
        }

        // "id" into a 0x20-byte buffer; REG_DWORD as is, REG_SZ through
        // atoi, anything else (or a failed read) keeps the previous Id.
        DWORD Size = 0x20;
        DWORD Type = 0;
        BYTE Data[0x20];
        if (RegQueryValueExA(Keys.Top(), "id", nullptr, &Type, Data, &Size) == ERROR_SUCCESS)
        {
            if (Type == REG_DWORD)
            {
                Id = *reinterpret_cast<unsigned int const*>(Data);
            }
            else if (Type == REG_SZ)
            {
                Id = static_cast<unsigned int>(MsvcrtAtoi(reinterpret_cast<char const*>(Data)));
            }
        }

        // insert {Id, FLoginRecord()} (the routine / the routine /
        // a no-op when Id is present), then "account" into the node's record.
        std::map<unsigned int, FLoginRecord>::iterator Node =
            Records.insert(std::make_pair(Id, FLoginRecord())).first;
        ReadRegHexString(Keys, Node->second.Account, "account");

        // the same insert again, then "password".
        Node = Records.insert(std::make_pair(Id, FLoginRecord())).first;
        ReadRegHexString(Keys, Node->second.Password, "password");

        // "time" (0x20-byte buffer); REG_DWORD / REG_SZ into operator[](Id).
        // Time, anything else (or a failed read) stores 0.
        Size = 0x20;
        BYTE TimeData[0x20];
        if (RegQueryValueExA(Keys.Top(), "time", nullptr, &Type, TimeData, &Size) == ERROR_SUCCESS &&
            (Type == REG_DWORD || Type == REG_SZ))
        {
            unsigned int const Time =
                Type == REG_DWORD ? *reinterpret_cast<unsigned int const*>(TimeData)
                                  : static_cast<unsigned int>(MsvcrtAtoi(reinterpret_cast<char const*>(TimeData)));
            Records[Id].Time = Time;
        }
        else
        {
            Records[Id].Time = 0;
        }

        Keys.PopClose();                                               // the routine
    }
    // PopClose, 005b65ba CloseAll: the stack's destructor.
    return true;                                                       // MOV AL,1
}

// called by CLoginMgr::SaveLogin (0x005a9645).
bool FLoginHistory::Save()
{
    FRegKeyStack Keys;
    // / 005b689a..005b68f2: RegCreateKeyExA(KEY_READ | KEY_WRITE) twice.
    if (Keys.Create(G_OutpostFrontierRegKey, 0x2001f) == nullptr ||
        Keys.Create("MP\\Logins", 0x2001f) == nullptr)
    {
        return true;                                                   // / 005b6b24
    }

    char Name[0x2000];                                                 // , [ESP+0x8b4]
    int Index = 1;
    for (std::map<unsigned int, FLoginRecord>::iterator It = Records.begin(); It != Records.end();
         ++It, ++Index)
    {
        _snprintf(Name, 0x2000, "Login%d", Index);
        // Top(), depth < 32, RegCreateKeyExA, pushed, non-null -- else the
        // record is skipped (its index is still used up).
        if (Keys.Create(Name, 0x2001f) == nullptr)
        {
            continue;
        }

        DWORD IdValue = It->first;
        RegSetValueExA(Keys.Top(), "id", 0, REG_DWORD, reinterpret_cast<BYTE const*>(&IdValue), 4);

        char AccountHex[0x400];                                        // [ESP+0x4b0]
        WideToHex(It->second.Account.c_str(), AccountHex);
        Keys.SetString("account", AccountHex);                         // the routine

        // the same REG_SZ write, spelled inline.
        char PasswordHex[0x400];                                       // [ESP+0xb0]
        WideToHex(It->second.Password.c_str(), PasswordHex);
        Keys.SetString("password", PasswordHex);

        DWORD TimeValue = It->second.Time;
        RegSetValueExA(Keys.Top(), "time", 0, REG_DWORD, reinterpret_cast<BYTE const*>(&TimeValue), 4);

        Keys.PopClose();                                               // inline
    }

    // delete the stale LoginN keys past the last record until a delete fails.
    for (;; ++Index)
    {
        _snprintf(Name, 0x2000, "Login%d", Index);
        HKEY const Parent = Keys.Top();
        if (Parent == nullptr)
        {
            break;
        }
        if (RegDeleteKeyA(Parent, Name) != ERROR_SUCCESS)
        {
            break;
        }
    }
    // the remaining keys closed (PopClose, then the stack's CloseAll).
    return true;                                                       // MOV AL,1
}


CLoginMgr::CLoginMgr()
    : Field_0x180(90.0f)            // = 42B40000h
    , bConnected(0)
    , LoginName(L"SinglePlayer")
    , bCreateAccount(0)
    , Field_0x1ac(50000.0f)         // = 47435000h
    , bLoginResponse(0)
    , LoginStatus(5)
    , Field_0x1c8(0)
    , SaveFailureCount(0)
    , Field_0x1d0(3000.0f)          // = 453B8000h
    , bCharacterInfoArrived(0)
    , Field_0x218(3000.0f)          // = 453B8000h
    , Field_0x21c(0)
    , ListServerHost("FLListServer2")
    , ListServerPort(0x708a)
    , Gun(nullptr)
    , Field_0x238(0)
    , Field_0x23c(0)
    , Field_0x240(0)
    , Field_0x25c(0)
{
    SelectCharacterName[0] = L'\0'; // `MOV word ptr [ESI+0x1e8],BX`
    Logins.Load();                  // the routine
    // Field_0x1c8 / SaveFailureCount: the constructor does not write them; the baseline object
    // is static and therefore zero-initialised.
}


bool CLoginMgr::SaveLogin()
{
    // the current server's Address copied into a local that is never read
    // (the FServerInfo temporary the routine returned is destroyed right after).
    std::wstring const UnusedAddress = GetCurrentServer().Address;
    std::wstring const Account = GetLoginName();                       // the routine
    std::wstring const UnusedPassword = GetLoginPassword();            // the routine
    (void)UnusedAddress;
    (void)UnusedPassword;

    Logins.GetRecord(GetCurrentServer().Id);                           // , result unused
    Logins.CurrentId = GetCurrentServer().Id;
    Logins.Records[Logins.CurrentId].Account = Account;
    Logins.SetTime(static_cast<unsigned int>(time(nullptr)));
    return Logins.Save();
}

// (RET 4, the copy is built in the caller's slot).
std::wstring CLoginMgr::GetLoginName() const
{
    return LoginName;
}

// the routine's body on +0x198 (the two diff only in the offset).
std::wstring CLoginMgr::GetLoginPassword() const
{
    return LoginPassword;
}

// (RET 4) -- FServerInfo's copy constructor inlined on +0x94.
FServerInfo CLoginMgr::GetCurrentServer() const
{
    return CurrentServer;
}

// (the atexit thunk) / module storage (deleting destructor). Member
// destructors and ~CDPClient follow implicitly in the baseline order.
CLoginMgr::~CLoginMgr()
{
    ResetServerLists();          // the routine
    bLoginResponse = 0;
    bCharacterInfoArrived = 0;
    Field_0x21c = 0;
}

// (Gun2::IGUNQueueRead slot 0, RET 8) -- the list server's server-list reply
// 0x7204. A non-empty page asks the browser for the next one (slot 5, (0x65, page, 0, 0, 0));
// each entry becomes an FServerInfo, appended to Servers when no entry with the same Id and
// +0x64 key is there yet, and copied into +0x160 when its key is non-empty and the Servers
// entry it matches has +0x3e clear. Every path returns 0 (module storage).
long __stdcall CLoginMgr::Read(Gun2::GUNQueueMessage* Message)
{
    Gun2::GUNMessageBody* Body = Message->Body;
    unsigned long const Type = Body->Type;
    if ((Type & 0xf00) != 0x200 || (Type & 0xff000) != 0x7000 || Type != 0x7204)
    {
        return 0;                                                      // / 5d9 / 5e5
    }

    Gun2::GUNServerEntry** Entries = nullptr;
    unsigned long EntryCount = 0;
    if (Body->bHasEntries != 0)
    {
        EntryCount = Body->EntryCount;
        Entries = Body->Entries;
    }
    if (Body->bHasEntries != 0 && EntryCount > 0)                      // JBE
    {
        unsigned int const Page = Field_0x23c;
        Field_0x23c = Page + 1;
        Gun->GetBrowser()->SendRequest7104(0x65, Page, 0, 0, 0);
    }
    else
    {
        Field_0x23c = 0;
    }

    for (unsigned long Index = 0; Index < EntryCount; ++Index)
    {
        Gun2::GUNServerEntry* Entry = Entries[Index];

        // the FServerInfo default spelled inline, then +0x28 = 1,
        // +0x88 = 1, +0x98 = 2 (the other stores it repeats -- +0x2c -2, +0x84 / +0x8c /
        // +0x90 / +0x94 0, +0x95 / +0x96 1 -- are the default constructor's values).
        FServerInfo Info;
        Info.Field_0x28 = 1;
        Info.MaxPlayers = 1;
        Info.Field_0x98 = 2;

        Info.Name = Entry->Name;
        Info.Field_0x54 = Entry->Description;
        Info.Version = static_cast<unsigned int>(MsvcrtWtoi(Entry->Version));
        Info.Address = Entry->Address;

        // [ESP+0xd4], 0x84 bytes up to the record buffer.
        char AddressText[0x84];
        sprintf(AddressText, "%S", Info.Address.c_str());
        Info.IpAddress = static_cast<int>(ntohl(inet_addr(AddressText)));

        // "p1,p2,...:+0x88:password:+0x96:+0x95:+0x64:Id". Any missing ':'
        // or an Id of -1 drops the entry (the branch).
        char Record[0x400];
        wcstombs(Record, Entry->Record, sizeof(Record));
        do
        {
            char* Colon = strchr(Record, ':');
            if (Colon == nullptr)
            {
                break;
            }
            *Colon = '\0';

            // split from the right, so Ports holds the list reversed.
            char* Comma = strrchr(Record, ',');
            while (Comma != nullptr)
            {
                *Comma = '\0';
                Info.Ports.push_back(static_cast<unsigned int>(MsvcrtAtoi(Comma + 1)));
                Comma = strrchr(Record, ',');
            }
            Info.Ports.push_back(static_cast<unsigned int>(MsvcrtAtoi(Record)));

            char* Field = Colon + 1;
            Colon = strchr(Field, ':');
            if (Colon == nullptr)
            {
                break;
            }
            *Colon = '\0';
            Info.MaxPlayers = MsvcrtAtoi(Field);

            Field = Colon + 1;
            Colon = strchr(Field, ':');
            if (Colon == nullptr)
            {
                break;
            }
            *Colon = '\0';
            Info.bPassword = static_cast<unsigned char>(MsvcrtAtoi(Field) != 0);

            Field = Colon + 1;
            Colon = strchr(Field, ':');
            if (Colon == nullptr)
            {
                break;
            }
            *Colon = '\0';
            Info.Field_0x96 = static_cast<unsigned char>(MsvcrtAtoi(Field) != 0);

            Field = Colon + 1;
            Colon = strchr(Field, ':');
            if (Colon == nullptr)
            {
                break;
            }
            *Colon = '\0';
            Info.Field_0x95 = static_cast<unsigned char>(MsvcrtAtoi(Field) != 0);

            Field = Colon + 1;
            Colon = strchr(Field, ':');
            if (Colon == nullptr)
            {
                break;
            }
            *Colon = '\0';
            Info.Field_0x64 = Field;

            Info.Id = static_cast<unsigned int>(MsvcrtAtoi(Colon + 1));
            if (Info.Id == 0xffffffffu)                                // CMP EAX,-1
            {
                break;
            }

            // always true here (1 and 2 above), tested all the same.
            if (Info.Field_0x28 != 0 && Info.Field_0x98 != 0)
            {
                bool bKnown = false;
                for (size_t ServerIndex = 0; ServerIndex < Servers.size(); ++ServerIndex)
                {
                    FServerInfo const& Server = Servers[ServerIndex];
                    if (Server.Id == Info.Id && Server.Field_0x64 == Info.Field_0x64)
                    {
                        bKnown = true;                                 // -> 005abb0a
                        break;
                    }
                }
                if (!bKnown)
                {
                    Servers.push_back(Info);                           // the routine
                    Field_0x238 = 1;
                }
            }


            if (!Info.Field_0x64.empty())
            {
                FServerInfo* Match = nullptr;
                for (size_t ServerIndex = 0; ServerIndex < Servers.size(); ++ServerIndex)
                {
                    FServerInfo& Server = Servers[ServerIndex];
                    if (Server.Field_0x64 == Info.Field_0x64 && Server.Id == Info.Id)
                    {
                        Match = &Server;                               // -> 005abb67
                        break;
                    }
                }
                if (Match != nullptr && Match->Field_0x3e == 0)
                {
                    Field_0x160.push_back(Info);                       // the routine
                }
            }
        } while (false);
        // Info destroyed.
    }
    return 0;
}

// (CLoginMgr's CDPClient vftable 005e64f0 slot 3, thiscall on the CDPClient
// sub-object, RET 4). The reply data, when present, is "+0x96:+0x95:Version:Id:+0x64:"
// followed by a wide string that becomes +0x54 (005ad0ce wcslen on the byte after the fifth
// ':'); a missing ':' or an Id of -1 drops the reply.
void CLoginMgr::OnAddHost(_DPNMSG_ENUM_HOSTS_RESPONSE* Response)
{
    DPN_APPLICATION_DESC const* Desc = Response->pApplicationDescription;

    // the FServerInfo default spelled inline.
    FServerInfo Info;
    Info.Port = 0;
    Info.Field_0x28 = 1;
    Info.Ping = static_cast<int>(Response->dwRoundTripLatencyMS); // / 005ace09
    if (Desc->dwCurrentPlayers != 0)
    {
        Info.PlayerCount = static_cast<int>(Desc->dwCurrentPlayers - 1);
    }
    else
    {
        Info.PlayerCount = 0;
    }
    Info.MaxPlayers = static_cast<int>(Desc->dwMaxPlayers - 1);
    Info.Field_0x98 = 2;
    Info.LastResponseTime = static_cast<int>(timeGetTime());
    Info.Version = 0;
    if (Response->pvUserContext == nullptr)
    {
        Info.Field_0x3e = 1;
    }
    Info.Name = Desc->pwszSessionName;

    // [ESP+0x14c], 0x800 bytes; data type 1 (DPNA_DATATYPE_STRING) in and
    // out through the same dword for both lookups. The HRESULTs are not read.
    wchar_t HostName[0x400];
    DWORD Size = sizeof(HostName);
    DWORD DataType = DPNA_DATATYPE_STRING;
    Response->pAddressSender->GetComponentByName(L"hostname", HostName, &Size, &DataType);
    Info.Address = HostName;

    // [ESP+0xcc], 0x80 bytes up to the host-name buffer.
    char AddressText[0x80];
    sprintf(AddressText, "%S", Info.Address.c_str());
    Info.IpAddress = static_cast<int>(ntohl(inet_addr(AddressText)));

    DWORD PortValue = 0;
    Size = sizeof(PortValue);
    Response->pAddressSender->GetComponentByName(L"port", &PortValue, &Size, &DataType);
    Info.Port = PortValue;

    // DPNSESSION_REQUIREPASSWORD (0x80).
    Info.bPassword = static_cast<unsigned char>((Desc->dwFlags >> 7) & 1);

    char* Data = static_cast<char*>(Response->pvResponseData);
    if (Data != nullptr)
    {
        char* Colon = strchr(Data, ':');
        if (Colon == nullptr)
        {
            return;
        }
        *Colon = '\0';
        Info.Field_0x96 = static_cast<unsigned char>(MsvcrtAtoi(Data) != 0);

        char* Field = Colon + 1;
        Colon = strchr(Field, ':');
        if (Colon == nullptr)
        {
            return;
        }
        *Colon = '\0';
        Info.Field_0x95 = static_cast<unsigned char>(MsvcrtAtoi(Field) != 0);

        Field = Colon + 1;
        Colon = strchr(Field, ':');
        if (Colon == nullptr)
        {
            return;
        }
        *Colon = '\0';
        Info.Version = static_cast<unsigned int>(MsvcrtAtoi(Field));

        Field = Colon + 1;
        Colon = strchr(Field, ':');
        if (Colon == nullptr)
        {
            return;
        }
        *Colon = '\0';
        Info.Id = static_cast<unsigned int>(MsvcrtAtoi(Field));

        Field = Colon + 1;
        Colon = strchr(Field, ':');
        if (Colon == nullptr)
        {
            return;                                                    // -> the branch
        }
        *Colon = '\0';
        Info.Field_0x64 = Field;
        Info.Field_0x54 = reinterpret_cast<wchar_t const*>(Colon + 1);
    }

    if (Info.Id == 0xffffffffu)
    {
        return;
    }

    RemovePendingDirect(Info);                                         // the routine
    RemoveRetryDirect(Info);                                           // the routine


    for (size_t ServerIndex = 0; ServerIndex < Servers.size(); ++ServerIndex)
    {
        FServerInfo& Server = Servers[ServerIndex];
        if (Server.Field_0x64 == Info.Field_0x64 && Server.Id == Info.Id)
        {
            // refresh the known entry. Address, Port, +0x10, +0x64 and Id
            // are left as they are.
            Server.Field_0x96 = Info.Field_0x96;
            Server.Field_0x95 = Info.Field_0x95;
            Server.bPassword = Info.bPassword;
            Server.Field_0x54 = Info.Field_0x54;                       // assign(s, 0, npos)
            Server.Name = Info.Name;
            Server.PlayerCount = Info.PlayerCount;
            Server.MaxPlayers = Info.MaxPlayers;
            Server.ResponseCount = Server.ResponseCount + 1;
            // unsigned minimum (CMP ECX,EAX; JNC keeps the old value).
            if (static_cast<unsigned int>(Info.Ping) < static_cast<unsigned int>(Server.Ping))
            {
                Server.Ping = Info.Ping;
            }
            Server.Field_0x40 = 0;
            Server.LastResponseTime = Info.LastResponseTime;
            Server.Version = Info.Version;
            Server.Field_0x3e = Info.Field_0x3e;
            Field_0x238 = 1;                                           // [CDPClient+0x234]
            Server.Field_0x3d = 0;
            return;
        }
    }

    // always true here (1 and 2 above), tested all the same; the second
    // search, with the terms the other way round, cannot find what the first one missed.
    if (Info.Field_0x28 != 0 && Info.Field_0x98 != 0)
    {
        for (size_t ServerIndex = 0; ServerIndex < Servers.size(); ++ServerIndex)
        {
            FServerInfo const& Server = Servers[ServerIndex];
            if (Server.Id == Info.Id && Server.Field_0x64 == Info.Field_0x64)
            {
                return;                                                // -> the branch
            }
        }
        Servers.push_back(Info);                                       // the routine
        Field_0x238 = 1;
    }
    // Info destroyed.
}


int CLoginMgr::Connect(bool bDisconnectFirst)
{
    // copies of Name and Address are built and destroyed unused.
    if (bDisconnectFirst && bConnected && GetPlayerConnectionId() != 0)
    {
        G_Server->DisConnect(GetPlayerConnectionId(), EFLConnection::NONE); // +0x40
        G_bServerDisconnected = false;
        G_bServerConnected = false;
        bConnected = 0;
        bLoginResponse = 0;
        bCharacterInfoArrived = 0;
        Characters.free();
        Field_0x21c = 0;
    }

    bConnected = 0;

    char Address[0x400];
    if (SinglePlayer())
    {
        FDUMP(0x100003, "%s(%d) : NOTICE:General:%s", kLoginMgrSource, 0x15f,
              "Starting single player game.\n");
        SetupSinglePlayer();        // ; its result is not read
        sprintf(Address, "%s", "localpass");
    }
    else
    {
        char Notice[0x2000];
        sprintf(Notice, "MP Connect: Server %S, IP %S\n", CurrentServer.Name.c_str(),
                CurrentServer.Address.c_str());
        FDUMP(0x100003, "%s(%d) : NOTICE:General:%s", kLoginMgrSource, 0x165, Notice);

        char* Cursor = Address + sprintf(Address, "tcpip, %S:", CurrentServer.Address.c_str());
        if (CurrentServer.Port != 0)
        {
            sprintf(Cursor, "%u", CurrentServer.Port);
        }
        else
        {
            // "%u" per port, joined with ','.
            std::vector<unsigned int>::const_iterator It = CurrentServer.Ports.begin();
            while (It != CurrentServer.Ports.end())
            {
                Cursor += sprintf(Cursor, "%u", *It);
                ++It;
                if (It == CurrentServer.Ports.end())
                {
                    break;
                }
                Cursor[0] = ',';
                Cursor[1] = '\0';
                ++Cursor;
            }
        }
    }

    // a null pointer when the password is empty (_Len == 0), not L"".
    unsigned short* Password = nullptr;
    if (!ServerPassword.empty())
    {
        Password = reinterpret_cast<unsigned short*>(const_cast<wchar_t*>(ServerPassword.c_str()));
    }
    G_Server->Connect(Address, Password); // +0x3c

    return WaitForServerResponse(&bConnected, 5.0f, 60.0f);
}

// IClient slot 11 (the branch) after it sets the field.
void CLoginMgr::OnConnectResponse(unsigned int ClientId)
{
    SetPlayerConnectionId(ClientId);
    bConnected = 1;
}


bool CLoginMgr::SetupSinglePlayer()
{
    if (bConnected)
    {
        return false;
    }

    CurrentServer.Address = L"localpass";
    CurrentServer.IpAddress = -1;
    CurrentServer.Field_0x28 = 1;
    CurrentServer.Ping = 0;
    CurrentServer.PlayerCount = 0;
    CurrentServer.MaxPlayers = 1;
    CurrentServer.Field_0x98 = 2;
    CurrentServer.Version = g_client.packed_version;
    CurrentServer.Name = L"";
    CurrentServer.Field_0x54 = L"";
    CurrentServer.Field_0x74 = "";

    // the body of the routine, inlined.
    ResetServerLists();
    return true;
}


bool CLoginMgr::Logout()
{
    if (GetPlayerConnectionId() != 0)
    {
        G_Server->DisConnect(GetPlayerConnectionId(), EFLConnection::NONE); // +0x40
        G_bServerDisconnected = false;
        G_bServerConnected = false;
        bConnected = 0;
        bLoginResponse = 0;
        bCharacterInfoArrived = 0;
        Characters.free();          // inlined OwnerList::free (module storage-005aa4ef)
        Field_0x21c = 0;
    }
    return true;
}


int CLoginMgr::Login()
{
    if (!bConnected)
    {
        return 0;
    }

    bLoginResponse = 0;

    SLoginInfo Info = {};
    Info.CreateFlag = bCreateAccount;                      // byte at +0xc0
    wcsncpy(Info.AccountName, LoginName.c_str(), 0x28);
    wcsncpy(Info.Field_0xA0, LoginPassword.c_str(), 0x10);
    Info.AccountName[0x27] = L'\0';                        // word at +0x4e
    Info.Field_0xA0[0x0f] = L'\0';                         // word at +0xbe
    G_Server->Login(Info, GetPlayerConnectionId());        // +0x48

    int const Result = WaitForServerResponse(&bLoginResponse, 5.0f, 60.0f);
    if (Result != 1 || LoginStatus != 3)
    {
        return 0;
    }

    if (!SinglePlayer())
    {
        SaveLogin();                // the routine, result ignored
        return Result;
    }

    if (G_Server == nullptr)
    {
        return Result;
    }

    // +0x15c ReqDifficultyScale: 0 under "+G", else perf-option slot 6.
    float const Scale = G_bCommandLineG ? 0.0f : GetPerfOptionSlot(6);
    G_Server->ReqDifficultyScale(Scale, GetPlayerConnectionId());
    return Result;
}

// IClient slot 12 (the branch jumps here with both arguments).
void CLoginMgr::OnLoginResponse(unsigned int Unused, unsigned char const* Status)
{
    (void)Unused;
    LoginStatus = *Status;
    bLoginResponse = 1;
}


int CLoginMgr::RequestCharacterInfo()
{
    if (G_Server == nullptr)
    {
        return 0;
    }

    bCharacterInfoArrived = 0;
    Characters.free();              // inlined OwnerList::free (module storage-005aa70f)
    G_Server->CharacterInfoReq(GetPlayerConnectionId(), false); // +0x4c
    return WaitForServerResponse(&bCharacterInfoArrived, 5.0f, 60.0f);
}

// (RET 4). The same inlined OwnerList::free as RequestCharacterInfo
// (module storage-005aa980, instruction for instruction), then IServer::DestroyCharacter and
// the wait on +0x1d4 (the routine(&+0x1d4, 5.0f, 60.0f)).
int CLoginMgr::DestroyCharacter(CHARACTER_ID const& CharId)
{
    if (G_Server == nullptr)
    {
        return 0;
    }

    bCharacterInfoArrived = 0;
    Characters.free();
    G_Server->DestroyCharacter(CharId, GetPlayerConnectionId()); // +0x5c
    return WaitForServerResponse(&bCharacterInfoArrived, 5.0f, 60.0f);
}

// (RET 4).
int CLoginMgr::CreateNewCharacter(SCreateCharacterInfo const& Info)
{
    // no server -> 0.
    if (G_Server == nullptr)
    {
        return 0;
    }

    // +0x1d4 = 0 and the inlined OwnerList::free (the same body as
    // RequestCharacterInfo's).
    bCharacterInfoArrived = 0;
    Characters.free();
    // IServer slot 22 (+0x58) with the client id the field.
    G_Server->CreateNewCharacter(Info, GetPlayerConnectionId());
    // the routine(&+0x1d4, 5.0f, 60.0f).
    int Result = WaitForServerResponse(&bCharacterInfoArrived, 5.0f, 60.0f);

    // no answer (the byte at +0x1d4), or an answer that brought
    // characters (+0x1b8, the list head) -> the wait's result.
    if (bCharacterInfoArrived == 0 || Characters.get_head() != nullptr)
    {
        return Result;
    }

    // the server re-tested; gone -> 0.
    if (G_Server == nullptr)
    {
        return 0;
    }

    // RequestCharacterInfo's body inlined (+0x1d4 = 0, the list freed,
    // IServer slot 19 +0x4c (client id, false), the routine(&+0x1d4, 5.0f, 60.0f)).
    bCharacterInfoArrived = 0;
    Characters.free();
    G_Server->CharacterInfoReq(GetPlayerConnectionId(), false);
    Result = WaitForServerResponse(&bCharacterInfoArrived, 5.0f, 60.0f);
    // 1 -> 4.
    if (Result == 1)
    {
        Result = 4;
    }
    return Result;
}

// (RET 4). Walk from the head (+0x1b8) counting nodes; the node at Index gives
// its data (+0x08), a list shorter than Index + 1 gives null.
CCharacterSet::CharacterInfo* CLoginMgr::CharacterAt(int Index)
{
    MetaNode<CCharacterSet::CharacterInfo>* Node = nullptr;
    int Count = 0;
    while (Characters.traverse(Node, true))
    {
        if (Count == Index)
        {
            return Node->Data;
        }
        Count = Count + 1;
    }
    return nullptr;
}

// the routine's body, then three flags.
void CLoginMgr::Reset()
{
    ResetServerLists();
    bLoginResponse = 0;
    bCharacterInfoArrived = 0;
    Field_0x21c = 0;
}


void CLoginMgr::ResetServerLists()
{
    CancelEnums();
    // erase(begin, end) through the routine: the three lists end up empty.
    Servers.clear();
    Field_0x160.clear();
    Field_0x170.clear();
    Field_0x25c = 0;

    // clear +0x3d on the source entry, then insert a copy at the end of
    // +0x160 (the routine(end, 1, entry)).
    for (size_t Index = 0; Index < DirectServers.size(); ++Index)
    {
        DirectServers[Index].Field_0x3d = 0;
        Field_0x160.push_back(DirectServers[Index]);
    }
}

// (thiscall, RET 4).
bool CLoginMgr::AddDirectServer(char const* HostPort)
{
    // no argument, or no copy -> false.
    if (HostPort == nullptr)
        return false;
    char* const Copy = _strdup(HostPort);
    if (Copy == nullptr)
        return false;

    // the last ':'; none -> the copy freed, true (0x005ac99c).
    char* const Colon = strrchr(Copy, ':');
    if (Colon != nullptr)
    {
        // the ':' cut to 0, the host widened into a 0x400-wchar local.
        *Colon = '\0';
        char const* const PortText = Colon + 1;
        wchar_t Host[0x400];
        mbstowcs(Host, Copy, 0x400);

        // a default server entry (the FServerInfo ctor inlined).
        FServerInfo Entry;
        // Address = the widened host (wcslen, grow, copy, eos).
        Entry.Address.assign(Host, wcslen(Host));
        // IpAddress = ntohl(inet_addr(host)).
        Entry.IpAddress = static_cast<int>(ntohl(inet_addr(Copy)));
        // one port, atoi(port), into Ports (the routine insert(end, 1)).
        unsigned const Port = static_cast<unsigned>(MsvcrtAtoi(PortText));
        Entry.Ports.push_back(Port);
        // appended to +0x150 (the routine(end, 1, &Entry)).
        DirectServers.push_back(Entry);
    } // the entry's strings, its port buffer and its address destroyed

    // .
    free(Copy);
    return true;
}


int CLoginMgr::GetGunStatus()
{
    if (Gun == nullptr)
    {
        return 3;
    }
    return static_cast<int>(Gun->GetStatus());
}

// vtable +4 with 1 is CGunWrapper's deleting destructor.
void CLoginMgr::DestroyGun()
{
    if (Gun != nullptr)
    {
        delete Gun;
        Gun = nullptr;
    }
}


FServerInfo* CLoginMgr::FindServer(unsigned int Id)
{
    for (size_t Index = 0; Index < Servers.size(); ++Index)
    {
        if (Servers[Index].Id == Id)
        {
            return &Servers[Index];
        }
    }
    return nullptr;
}


bool CLoginMgr::SetCurrentServer(FServerInfo const& Server)
{
    if (Server.Field_0x28 == 0 || Server.Field_0x98 == 0)
        return false;
    // FServerInfo::operator= (the routine) inline into +0x94.
    CurrentServer = Server;
    return true;
}


bool CLoginMgr::CreateGun()
{
    if (G_bInternetServerList != 0 && Gun == nullptr)
    {
        // the same application GUID CDPServer hosts with.
        GUID Guid = {0xa690f026, 0x26f0, 0x4e57, {0xac, 0xa0, 0xec, 0xf8, 0x68, 0xe4, 0x8d, 0x21}};
        // operator_new(0x28), then the ctor with the host's text
        // (the empty string when there is none), the port and no cd-key.
        Gun = new CGunWrapper("..\\exe\\gundll.dll", Guid, ListServerHost.c_str(),
                              ListServerPort, nullptr);
        Field_0x23c = 0;
    }
    return true;
}


bool CLoginMgr::RequestServerList()
{
    if (Gun != nullptr && Gun->GetBrowser() != nullptr &&
        static_cast<int>(Gun->GetStatus()) == 2)
    {
        Field_0x240 = 0;
        if (Field_0x23c != 0)
            return false;
        Gun->GetBrowser()->SendRequest7104(0x65, 0, 0, 0, 0);
        ++Field_0x23c;
        return true;
    }
    Field_0x240 = 1;
    return false;
}

namespace
{
    // The two lookups PingServers spells inline (0x005ac423..0x005ac471 and
    // ): the list-server entry with Entry's +0x64 text and Id, or null.
    // Only asked when that text is not empty.
    FServerInfo* FindServerByKey(std::vector<FServerInfo>& Servers, FServerInfo const& Entry)
    {
        for (size_t Index = 0; Index < Servers.size(); ++Index)
        {
            if (Servers[Index].Field_0x64 == Entry.Field_0x64 && Servers[Index].Id == Entry.Id)
                return &Servers[Index];
        }
        return nullptr;
    }

    // The match RemovePendingDirect / RemoveRetryDirect share (0x005abe00..0x005abe46).
    bool IsSameDirectServer(FServerInfo const& Entry, FServerInfo const& Server)
    {
        if (!Entry.Field_0x64.empty() && Entry.Field_0x64 == Server.Field_0x64 &&
            Entry.Id == Server.Id)
            return true;
        if (Entry.Port != 0 && Entry.Address == Server.Address && Entry.Port == Server.Port)
            return true;
        return false;
    }
}


void CLoginMgr::PingServers()
{
    // CMP [ESI+0x7c],0 -- CDPClient::PendingConnectHandle (+0x78) read directly.
    if (G_bEnumerateLanHosts != 0 && PendingConnectHandle == 0)
        EnumerateHosts();

    DWORD const Now = timeGetTime();
    // an entry that answered the list (+0x3e) and has been silent for
    // more than 5 s shows no ping; past 30 s it is dropped (erase, the next one moves up).
    size_t Index = 0;
    while (Index < Servers.size())
    {
        FServerInfo& Entry = Servers[Index];
        unsigned long const Age = Now - static_cast<unsigned long>(Entry.LastResponseTime);
        if (Entry.Field_0x3e != 0 && Age > 5000)
        {
            bool const bDrop = Age > 30000;
            Entry.Ping = -1;
            Field_0x238 = 1;
            if (bDrop)
            {
                Servers.erase(Servers.begin() + Index);
                Field_0x238 = 1;
                continue;
            }
        }
        ++Index;
    }

    if (Field_0x23c != 0)
        return;

    // direct servers whose enumeration has run for a second. Under
    // five tries they go to the retry list +0x170; after that their list-server twin (if any)
    // shows no ping. Either way they leave +0x160 and the in-flight count drops.
    Index = 0;
    while (Index < Field_0x160.size())
    {
        FServerInfo& Entry = Field_0x160[Index];
        if (Entry.Field_0x3d == 0 ||
            Now - static_cast<unsigned long>(Entry.LastResponseTime) < 1000)
        {
            ++Index;
            continue;
        }
        if (static_cast<unsigned>(Entry.Field_0x34) < 5)
        {
            Entry.Field_0x3d = 0;
            Field_0x170.push_back(Entry);
        }
        else
        {
            FServerInfo* const Twin =
                Entry.Field_0x64.empty() ? nullptr : FindServerByKey(Servers, Entry);
            if (Twin != nullptr)
            {
                Twin->Ping = -1;
                Field_0x238 = 1;
            }
        }
        Field_0x160.erase(Field_0x160.begin() + Index);
        --Field_0x25c;
    }

    if (Field_0x25c >= 5)
        return;

    // +0x160 drained -> every retry goes back into it (+0x3d cleared),
    // its list-server twin showing "????" (-2); then +0x170 is emptied.
    if (Field_0x160.empty() && !Field_0x170.empty())
    {
        for (size_t Retry = 0; Retry < Field_0x170.size(); ++Retry)
        {
            FServerInfo& Entry = Field_0x170[Retry];
            Entry.Field_0x3d = 0;
            Field_0x160.push_back(Entry);
            FServerInfo* const Twin =
                Entry.Field_0x64.empty() ? nullptr : FindServerByKey(Servers, Entry);
            if (Twin != nullptr)
            {
                Twin->Ping = -2;
                Field_0x238 = 1;
            }
        }
        Field_0x170.clear();
    }

    // start an enumeration on every port of each idle entry until five
    // are in flight.
    size_t const End = Field_0x160.size();
    for (Index = 0; Index < End; ++Index)
    {
        FServerInfo& Entry = Field_0x160[Index];
        if (Entry.Field_0x3d != 0)
            continue;
        bool bStarted = false;
        for (size_t PortIndex = 0; PortIndex < Entry.Ports.size(); ++PortIndex)
        {
            if (EnumerateHost(Entry.Address.c_str(), Entry.Ports[PortIndex], 2, 0, 0))
                bStarted = true;
        }
        if (bStarted)
        {
            ++Field_0x25c;
            Entry.Field_0x3d = 1;
        }
        Entry.LastResponseTime = static_cast<int>(timeGetTime());
        ++Entry.Field_0x34;
        if (Field_0x25c >= 5)
            return;
    }
}


bool CLoginMgr::PumpServerList()
{
    if (Gun != nullptr && Gun->GetConnection() != nullptr)
    {
        Gun->GetConnection()->DispatchQueue(static_cast<IGunQueueReader*>(this));
        Gun->Update();
    }
    DispatchMsgs(0xffffffff);
    PingServers();

    // the last time the page was told to refresh.
    static DWORD LastRefreshTime = 0;
    bool bRefresh = false;
    if (Field_0x238 != 0 && timeGetTime() - LastRefreshTime >= 1000)
    {
        bRefresh = true;
        Field_0x238 = 0;
        LastRefreshTime = timeGetTime();
    }
    return bRefresh;
}


void CLoginMgr::GetFilteredServers(std::vector<FServerInfo*>& Out, FServerCompare Compare,
                                   ServerFilter const* Filter)
{
    Out.clear();
    size_t const Count = Servers.size();
    Out.reserve(Count);
    for (size_t Index = 0; Index < Count; ++Index)
    {
        FServerInfo* const Server = &Servers[Index];
        if (Filter == nullptr || Filter->Match(*Server))
            Out.push_back(Server);
    }
    // no comparator (EBX) -> unsorted; else VC6 std::sort inlined
    // (the routine / the routine / the routine / the routine), which vc6_sort.h ports so
    // that servers with equal keys keep baseline's order.
    if (Compare != nullptr)
        Vc6Sort::Sort(Out.data(), Out.data() + Out.size(), Compare);
}


void CLoginMgr::RemovePendingDirect(FServerInfo const& Server)
{
    for (size_t Index = 0; Index < Field_0x160.size(); ++Index)
    {
        FServerInfo const& Entry = Field_0x160[Index];
        if (Entry.Field_0x3d != 0 && IsSameDirectServer(Entry, Server))
        {
            Field_0x160.erase(Field_0x160.begin() + Index);
            --Field_0x25c;
            return;
        }
    }
}


void CLoginMgr::RemoveRetryDirect(FServerInfo const& Server)
{
    for (size_t Index = 0; Index < Field_0x170.size(); ++Index)
    {
        if (IsSameDirectServer(Field_0x170[Index], Server))
        {
            Field_0x170.erase(Field_0x170.begin() + Index);
            return;
        }
    }
}


bool CLoginMgr::IsKnownLogin(FServerInfo const& Server) const
{
    return Logins.Contains(Server.Id);
}


unsigned int CLoginMgr::GetLoginTime(FServerInfo const& Server)
{
    return Logins.GetTime(Server.Id);
}

// an unbounded wcscpy into the 24-character buffer, as baseline.
void CLoginMgr::SetSelectCharacterName(wchar_t const* Name)
{
    wcscpy(SelectCharacterName, Name);
}

// (RET 4). wcscmp of each record's Name (+0x204; the _Nullstr when empty)
// against Name. There is no end-of-list test: the step past the tail reads node 0
// (module storage / 005aad50), as baseline does.
CCharacterSet::CharacterInfo* CLoginMgr::FindCharacterByName(wchar_t const* Name)
{
    MetaNode<CCharacterSet::CharacterInfo>* Node = nullptr;
    for (;;)
    {
        Characters.traverse(Node, true);
        if (wcscmp(Node->Data->Name.c_str(), Name) == 0)
        {
            return Node->Data;
        }
    }
}

// (RET 4) -- the first record whose key equals CharId (COMMON operator==), else
// null. The walk starts at the list head +0x1b8 and follows each node's next.
CCharacterSet::CharacterInfo* CLoginMgr::FindCharacterById(CHARACTER_ID const& CharId)
{
    MetaNode<CCharacterSet::CharacterInfo>* Node = nullptr;
    while (Characters.traverse(Node, true))
    {
        if (Node->Data->Id == CharId)
        {
            return Node->Data;
        }
    }
    return nullptr;
}

// the first record with +0x200 (bIsRestart) set.
CCharacterSet::CharacterInfo* CLoginMgr::FindRestartCharacter()
{
    MetaNode<CCharacterSet::CharacterInfo>* Node = nullptr;
    while (Characters.traverse(Node, true))
    {
        if (Node->Data->bIsRestart != 0)
        {
            return Node->Data;
        }
    }
    return nullptr;
}

// the first record with +0x201 (bIsAutoSave) set.
CCharacterSet::CharacterInfo* CLoginMgr::FindAutoSaveCharacter()
{
    MetaNode<CCharacterSet::CharacterInfo>* Node = nullptr;
    while (Characters.traverse(Node, true))
    {
        if (Node->Data->bIsAutoSave != 0)
        {
            return Node->Data;
        }
    }
    return nullptr;
}

// .
bool CLoginMgr::ReloadFactions()
{
    // erase(begin, end) inline -- nothing to move down (the copy loop
    // runs from end to end), every record destroyed (the routine), end = begin.
    Factions.clear();
    // NewChar::TheDB slot 4 (+0x10) with the branch and &Factions;
    // its result is not read.
    NewChar::TheDB->ForEachFaction(PushFactionRecord, &Factions);
    return true;
}

// LEA EAX,[ECX+0x1d8]; RET.
std::vector<FNewCharFaction>& CLoginMgr::GetFactions()
{
    return Factions;
}

// . Reached from the routine (the [Login] auto-login, 005b2146).
bool CLoginMgr::SelectAutoCharacter()
{
    MetaNode<CCharacterSet::CharacterInfo>* Found = nullptr; // EBP
    MetaNode<CCharacterSet::CharacterInfo>* Node = nullptr;  // ESI
    while (Characters.traverse(Node, true))
    {
        if (!SinglePlayer())
        {
            // the first record whose name _wcsicmp-matches.
            if (_wcsicmp(Node->Data->Name.c_str(), SelectCharacterName) != 0)
            {
                continue;
            }
            Found = Node;
            break;
        }

        // single player. [Login] char = "AutoSave" (the field) picks the
        // first autosave record.
        if (_wcsicmp(SelectCharacterName, L"AutoSave") == 0)
        {
            if (Node->Data->bIsAutoSave == 0)
            {
                continue;
            }
            Found = Node;
            break;
        }

        // otherwise every record with the name is a candidate; the
        // first one is kept unless it is an autosave and a later non-autosave record matches.
        if (_wcsicmp(Node->Data->Name.c_str(), SelectCharacterName) != 0)
        {
            continue;
        }
        if (Found != nullptr)
        {
            if (Found->Data->bIsAutoSave == 0)
            {
                continue;
            }
            if (Node->Data->bIsAutoSave != 0)
            {
                continue;
            }
        }
        Found = Node;
    }

    // / 005ab1e2.
    if (Found == nullptr)
    {
        return false;
    }
    return SelectCharacter(Found->Data);
}

// (RET 4).
bool CLoginMgr::SelectCharacterById(CHARACTER_ID const& CharId)
{
    MetaNode<CCharacterSet::CharacterInfo>* Node = nullptr;
    while (Characters.traverse(Node, true))
    {
        if (Node->Data->Id == CharId) // ??8@YA_NABUCHARACTER_ID@@0@Z
        {
            return SelectCharacter(Node->Data);
        }
    }
    return SelectCharacter(nullptr);
}

// (RET 4).
bool CLoginMgr::SelectCharacter(CCharacterSet::CharacterInfo* Info)
{
    if (Info == nullptr)
    {
        return false;
    }
    Field_0x21c = 0;
    G_Server->CharacterSelect(Info->Id, GetPlayerConnectionId()); // +0x50
    WaitForServerResponse(&Field_0x21c, 5.0f, 60.0f);
    if (Field_0x21c == 0)
    {
        return false;
    }
    // the record into the player globals, then its name into
    // (character_select_equip.cpp).
    ApplyCharacterSelectEquipFields(Info);
    return true;
}

// (RET 4), reached from IClient slot 14 (the branch).
void CLoginMgr::OnCharacterSelectResponse(unsigned int const* Response)
{
    Field_0x21c = 1;
    NotifyCharacterSelectComplete(Response);
}

// std::string::assign(Host, strlen(Host)).
void CLoginMgr::SetListServerHost(char const* Host)
{
    ListServerHost.assign(Host, strlen(Host));
}


void CLoginMgr::SetListServerPort(unsigned int Port)
{
    ListServerPort = Port;
}

// `ADD ECX,4; JMP CDPClient::SetGUID`.
void CLoginMgr::SetAppGuid(GUID& Guid)
{
    SetGUID(Guid);
}

// (cdecl; the first argument, the command line, is not read). The server
// module name goes to G_LocalServerDll, the rest into the context.
void CLoginMgr::LoadConfig(char const* IniPath)
{
    GUID AppGuid;
    bool bHaveGuid = false;

    INI_Reader Reader;
    Reader.open(IniPath, false);   // result not tested (module storage)
    if (!Reader.read_header())
    {
        return;
    }

    do
    {
        if (Reader.is_header("OutpostFrontier"))
        {
            while (Reader.read_value())
            {
                if (Reader.is_value("local_server"))
                {
                    char const* Value = Reader.get_value_string();
                    if (Value != nullptr)
                    {
                        // at most 31 characters, then a terminator.
                        size_t Length = strlen(Value);
                        if (Length >= 0x20)
                        {
                            Length = 0x1f;
                        }
                        memcpy(G_LocalServerDll, Value, Length);
                        G_LocalServerDll[Length] = '\0';
                    }
                }
                else if (Reader.is_value("AppGUID"))
                {
                    ParseGUID(Reader.get_value_string(), &AppGuid);
                    bHaveGuid = true;
                }
            }
        }
        else if (Reader.is_header("ListServer"))
        {
            while (Reader.read_value())
            {
                if (Reader.is_value("hostname"))
                {
                    SetListServerHost(Reader.get_value_string());
                }
                else if (Reader.is_value("port"))
                {
                    // get_value_float(0) then _ftol.
                    SetListServerPort(static_cast<unsigned int>(
                        static_cast<long long>(Reader.get_value_float(0))));
                }
            }
        }
        else if (Reader.is_header("Login"))
        {
            while (Reader.read_value())
            {
                if (Reader.is_value("name"))
                {
                    LoginName = std::wstring(Reader.get_value_wstring());
                }
                else if (Reader.is_value("pwd"))
                {
                    LoginPassword = std::wstring(Reader.get_value_wstring());
                }
                else if (Reader.is_value("char"))
                {
                    SetSelectCharacterName(Reader.get_value_wstring());
                }
            }
        }
    } while (Reader.read_header());

    if (bHaveGuid)
    {
        SetAppGuid(AppGuid);
    }
}
