// Author: Giperion.

#pragma once

// CLoginMgr -- OutpostFrontier.exe's connect / login context (RTTI ".?AVCLoginMgr@@",
// TypeDescriptor module storage). One static instance, module storage (G_LoginMgr), built
// by the static initializer module storage (constructor module storage, destructor
// registered with atexit on the CDPClient sub-object 0067e7bc).

// Bases, in baseline order (the complete-object locators name both):
//   +0x00  Gun2::IGUNQueueRead  -- vftable module storage, one slot: Read (module storage)
//   +0x04  CDPClient            -- vftable module storage: OnReceive / OnConnect /
//          OnDisconnect unchanged, slot 3 OnAddHost overridden (module storage), slot 4 the
//          deleting destructor (module storage, `this-4`).
// NeuronRuntime's header calls the first base IGunQueueReader; the exe's RTTI string is
// ".?AUIGUNQueueRead@Gun2@@" (module storage). Same single slot, same interface.

// MODERN TYPES. baseline's strings and vectors are MSVCP60 containers; the port uses std::
// containers, so no member below sits at its baseline byte offset. The +0xNN in each comment is
// the baseline offset every listing reads, kept so each access can be matched to its address.

#include "common_newchar.h"      // FNewCharFaction (Factions, +0x1d8)
#include "neuronruntime_net.h"
#include "server_characterset.h" // CCharacterSet::CharacterInfo, OwnerList<T>

#include <map>
#include <string>
#include <vector>

struct IServer;
class ServerFilter;
struct FServerInfo;

// The comparator GetFilteredServers sorts with (cdecl, "A before B"): one of the
// PageServerSelect column tables at module storage (ascending) / 00614ac0 (descending).
typedef bool (*FServerCompare)(FServerInfo const* A, FServerInfo const* B);

// FServerInfo -- one server entry, 0x9c bytes in baseline. Built inline by the CLoginMgr
// constructor for +0x94 (module storage-005a8ce2); copy constructor module storage,
// operator= module storage and destructor module storage are memberwise, which is what the
// implicit special members of this struct do.
struct FServerInfo
{
    FServerInfo();

    std::wstring Address;           // +0x00 host/IP; L"localpass" in single player (005aa226)
    int IpAddress;                  // +0x10 IPv4 in host order, ntohl(inet_addr(host)) (005acf78,
                                    // ); -1 (INADDR_NONE) by default (005a8c3c,
                                    // ); PageServerSelect's address column key
    unsigned int Port;              // +0x14 0 = use Ports (005a9b3c)
    std::vector<unsigned int> Ports; // +0x18 ports joined with ',' by Connect (005a9b49..)
    int Field_0x28;                 // +0x28 0; 1 in single player (005aa28c). TODO: meaning
    int Ping;                       // +0x2c -2; 0 in single player; DirectPlay round-trip ms,
                                    //       kept as the unsigned minimum (OnAddHost 005ad301);
                                    //       ServerFilter range key
    int LastResponseTime;           // +0x30 timeGetTime of the last enum reply (005ace46)
    int Field_0x34;                 // +0x34
    int ResponseCount;              // +0x38 raised by every enum reply for a known server (005ad2f7)
    unsigned char Field_0x3c;       // +0x3c
    unsigned char Field_0x3d;       // +0x3d cleared on every copy into +0x160 (005abd81)
    unsigned char Field_0x3e;       // +0x3e ServerFilter key (562990)
    int Field_0x40;                 // +0x40
    std::wstring Name;              // +0x44 "MP Connect: Server %S" (005a9adc)
    std::wstring Field_0x54;        // +0x54 L"" in single player (005aa30b)
    std::string Field_0x64;         // +0x64
    std::string Field_0x74;         // +0x74 "" in single player (005aa35b)
    int PlayerCount;                // +0x84 0 in single player; dwCurrentPlayers - 1 (005ace1f)
    int MaxPlayers;                 // +0x88 1 in single player; dwMaxPlayers - 1 (005ace33)
    unsigned int Version;           // +0x8c the field in single player (005aa2ae)
    unsigned int Id;                // +0x90 key of FindServer (005abdc4) / the login history
    unsigned char bPassword;        // +0x94 PageServerSelect asks for a password (00572184)
    unsigned char Field_0x95;       // +0x95 1
    unsigned char Field_0x96;       // +0x96 1
    int Field_0x98;                 // +0x98 2 in single player
};

// One account the player logged in with before, loaded from
// HKCU\...\MP\Logins\Login%d by the routine. Node payload of the +0x248 map (the pair's
// destructor is the routine: two wide strings, then the time). Default constructor
// (two empty wide strings, time 0); destructor the routine.
struct FLoginRecord
{
    std::wstring Account;           // +0x00 "account" (hex-encoded in the registry)
    std::wstring Password;          // +0x10 "password" (hex-encoded in the registry)
    unsigned int Time = 0;          // +0x20 "time"
};

// CLoginMgr+0x244. +0x04 the map (the routine constructs it, the routine destroys it).
struct FLoginHistory
{
    FLoginHistory();

    // (thiscall, returns AL = 1 on every path): the map cleared, then
    // HKCU\<OutpostFrontier>\MP\Logins\Login1, Login2, ... read until one fails to open.
    bool Load();
    // (thiscall, returns AL = 1 on every path): every record written back as
    // Login1..LoginN, then Login(N+1).. deleted until a delete fails.
    bool Save();
    // lower_bound(Id); true when that node exists and its key is not greater.
    bool Contains(unsigned int Id) const;
    // (RET 4): operator[](Id) -- inserts a default record when Id is absent.
    FLoginRecord& GetRecord(unsigned int Id);
    // (RET 4): operator[](CurrentId).Time = Time.
    void SetTime(unsigned int Time);
    // (RET 4): operator[](CurrentId).Time. The argument is not read: baseline
    // pushes the history object itself (`PUSH ECX`, 0x005b6c60) as operator[]'s key pointer,
    // so the key is the dword at +0x00, whatever Id the caller passed (005a9970 passes the
    // server's Id).
    unsigned int GetTime(unsigned int IgnoredId);

    unsigned int CurrentId;         // +0x00 the last login's server Id (the routine, 005a95c5)
    std::map<unsigned int, FLoginRecord> Records; // +0x04
};


class CLoginMgr : public IGunQueueReader, public CDPClient
{
public:
    CLoginMgr();
    virtual ~CLoginMgr();                           // / 005a8f20

    // Gun2::IGUNQueueRead slot 0 (module storage, RET 8): the list server's reply 0x7204
    // into Servers / +0x160, the next page requested through the browser. Reached through
    // the CGunWrapper at +0x234 (CreateGun module storage).
    virtual long __stdcall Read(Gun2::GUNQueueMessage* Message) override;

    // IDPMsgHandler slot 3 on the CDPClient base (module storage, RET 4): one DirectPlay
    // enum-hosts reply. Builds an FServerInfo from the session desc, the sender's
    // "hostname" / "port" components and the reply data "+0x96:+0x95:Version:Id:+0x64:"
    // followed by a wide +0x54 text; refreshes the Servers entry with the same +0x64 key and
    // Id, or appends a new one.
    void OnAddHost(_DPNMSG_ENUM_HOSTS_RESPONSE* Response) override;

    int Connect(bool bDisconnectFirst);
    void OnConnectResponse(unsigned int ClientId);  // (IClient slot 11)
    bool SetupSinglePlayer();
    bool Logout();
    int Login();
    // after a multiplayer login (Login 0x005aa5e2): the current server's Id
    // becomes the history's CurrentId, its record gets the login name and time(nullptr), and
    // the history is saved. Returns Save's result (Login ignores it).
    bool SaveLogin();
    std::wstring GetLoginName() const;              // a copy of +0x188
    std::wstring GetLoginPassword() const;          // a copy of +0x198
    FServerInfo GetCurrentServer() const;           // a copy of +0x94
    void OnLoginResponse(unsigned int Unused, unsigned char const* Status);
    int RequestCharacterInfo();
    // RequestCharacterInfo's shape around IServer::DestroyCharacter (+0x5c): no server -> 0;
    // otherwise the list is dropped and the answer awaited (1 = it came). Caller: NN_LoadSave's
    // delete confirmation (0x0048723e).
    int DestroyCharacter(CHARACTER_ID const& CharId);
    // (RET 4) -- DestroyCharacter's shape around IServer::CreateNewCharacter
    // (+0x58): no server -> 0; the list dropped, the request sent, the answer awaited. An answer
    // that came with a roster returns the wait's result; an answer with an empty roster asks for
    // the character info again (+0x4c) and turns that wait's 1 into 4. Caller: PageCharacterCreate
    // (0x005615d8), which reads 1 as created, 4 as refused (NN_Error 0x738).
    int CreateNewCharacter(SCreateCharacterInfo const& Info);
    // The record at Index in the +0x1b4 list, or null past the end. Caller: NN_LoadSave's
    // refresh (0x0048791a).
    CCharacterSet::CharacterInfo* CharacterAt(int Index);
    void Reset();
    void ResetServerLists();
    // (RET 4) -- WinMain's "-S host:port" (0x005b2d17): false for a null
    // argument or a failed strdup; with a ':' (the last one) a default FServerInfo whose Address
    // is the widened host, IpAddress ntohl(inet_addr(host)) and one port atoi(port) is appended
    // to DirectServers (+0x150); true otherwise.
    bool AddDirectServer(char const* HostPort);
    int GetGunStatus();
    void DestroyGun();
    FServerInfo* FindServer(unsigned int Id);
    // false unless Server +0x28 and +0x98 are both non-zero; otherwise
    // CurrentServer = Server (FServerInfo::operator= inline), true. Caller: PageServerSelect's
    // connect (0x00572123).
    bool SetCurrentServer(FServerInfo const& Server);
    // with the internet list picked (the field) and no Gun yet: a
    // CGunWrapper on "..\exe\gundll.dll" for ListServerHost:ListServerPort, request count 0.
    // Always true. Callers: IntroFocus EnterState case 5 (0x00574bbe), PageServerSelect's failed
    // connect (0x0057227e).
    bool CreateGun();
    // the Gun connected (status 2) with a browser: +0x240 = 0 and, the first
    // time only (+0x23c == 0), browser slot 5 (0x65, 0, 0, 0, 0) and +0x23c + 1 -> true.
    // Otherwise +0x240 = 1 (when there is no connected browser) and false.
    bool RequestServerList();
    // LAN enumeration, list-server entries aging out (+0x3e: ping -1 after 5 s,
    // dropped after 30 s), then the direct-server queue +0x160 / +0x170 pinged five at a time.
    void PingServers();
    // the Gun's queue and Update, DispatchMsgs(-1), PingServers; true once a
    // second at most while +0x238 (list changed) is set, which it then clears.
    bool PumpServerList();
    // (RET 0xc) -- Out = every server Filter (may be null) accepts, then sorted
    // with Compare when it is not null.
    void GetFilteredServers(std::vector<FServerInfo*>& Out, FServerCompare Compare,
                            ServerFilter const* Filter);
    // (RET 4) -- erase the first +0x160 entry with +0x3d set that matches Server
    // (same +0x64 and Id when +0x64 is not empty, else same Address and Port when Port is not
    // zero); +0x25c - 1.
    void RemovePendingDirect(FServerInfo const& Server);
    // (RET 4) -- the same match over +0x170, any entry, no count change.
    void RemoveRetryDirect(FServerInfo const& Server);
    bool IsKnownLogin(FServerInfo const& Server) const;
    unsigned int GetLoginTime(FServerInfo const& Server);
    void SetSelectCharacterName(wchar_t const* Name);

    // The character list (+0x1b4) walks. Each walks from the head and returns the record, or
    // null at the end of the list -- except FindCharacterByName, which has no end test.
    CCharacterSet::CharacterInfo* FindCharacterByName(wchar_t const* Name);
    CCharacterSet::CharacterInfo* FindCharacterById(CHARACTER_ID const& CharId);
    CCharacterSet::CharacterInfo* FindRestartCharacter();
    CCharacterSet::CharacterInfo* FindAutoSaveCharacter();
    // Factions (+0x1d8) emptied, then refilled with a copy of every NewChar
    // [Faction] record (NewChar::TheDB slot 4 ForEachFaction, callback the branch). Always
    // true. Callers: PageCharacterSelect Create (0x0056829e) / delete failure (0x00569e33),
    // PageCharacterSelectSingle Create (0x0056b510) / delete failure (0x0056c643), the
    // single-player start (0x00573bce).
    bool ReloadFactions();
    // &Factions (+0x1d8). Caller: PageCharacterSelect Update (0x0056aaa2).
    std::vector<FNewCharFaction>& GetFactions();
    // The [Login] auto-login pick by SelectCharacterName (+0x1e8), then SelectCharacter.
    bool SelectAutoCharacter();
    // The record whose Id == CharId (CHARACTER_ID operator==), then SelectCharacter; with no
    // match SelectCharacter(0), i.e. false.
    bool SelectCharacterById(CHARACTER_ID const& CharId);
    // IServer::CharacterSelect, wait for the answer (+0x21c), then copy the record into the
    // player globals (character_select_equip.cpp) and its name into the field.
    bool SelectCharacter(CCharacterSet::CharacterInfo* Info);
    // IClient slot 14 (the branch): the CharacterSelect answer -- +0x21c = 1 and the
    // response fields (character_select_equip.cpp NotifyCharacterSelectComplete).
    void OnCharacterSelectResponse(unsigned int const* Response);
    void SetListServerHost(char const* Host);
    void SetListServerPort(unsigned int Port);
    void SetAppGuid(GUID& Guid);
    void LoadConfig(char const* IniPath);

    FServerInfo CurrentServer;                      // +0x94
    std::wstring ServerPassword;                    // +0x130 handed to IServer::Connect
    std::vector<FServerInfo> Servers;               // +0x140 searched by FindServer
    std::vector<FServerInfo> DirectServers;         // +0x150 copied into +0x160 on reset
    std::vector<FServerInfo> Field_0x160;           // +0x160
    std::vector<FServerInfo> Field_0x170;           // +0x170
    float Field_0x180;                              // +0x180 90.0f (the field)
    unsigned char bConnected;                       // +0x184 set by OnConnectResponse
    std::wstring LoginName;                         // +0x188 L"SinglePlayer"
    std::wstring LoginPassword;                     // +0x198
    unsigned char bCreateAccount;                   // +0x1a8 SLoginInfo+0xc0
    float Field_0x1ac;                              // +0x1ac 50000.0f (the field)
    unsigned char bLoginResponse;                   // +0x1b0 set by OnLoginResponse
    unsigned char LoginStatus;                      // +0x1b1 5 at construction, 3 = accepted
    OwnerList<CCharacterSet::CharacterInfo> Characters; // +0x1b4 filled by the routine
    unsigned int Field_0x1c8;                       // +0x1c8 never written by this class
    unsigned int SaveFailureCount;                  // +0x1cc the routine (module storage)
    float Field_0x1d0;                              // +0x1d0 3000.0f (the field)
    unsigned char bCharacterInfoArrived;            // +0x1d4
    // +0x1d8 -- a copy of every NewChar [Faction] record (0x50 bytes each, FNewCharFaction,
    // common_newchar.h), rebuilt by ReloadFactions (the routine); element copy the routine,
    // destructor the routine. (Earlier notes called these list-server records; the GUN
    // reader the routine never writes +0x1d8.)
    std::vector<FNewCharFaction> Factions;          // +0x1d8
    wchar_t SelectCharacterName[24];                // +0x1e8 [Login] char (the routine)
    float Field_0x218;                              // +0x218 3000.0f (the field)
    unsigned char Field_0x21c;                      // +0x21c CharacterSelect answered (005ab385)
    std::string ListServerHost;                     // +0x220 "FLListServer2"
    unsigned int ListServerPort;                    // +0x230 0x708a
    CGunWrapper* Gun;                               // +0x234
    unsigned char Field_0x238;                      // +0x238
    unsigned int Field_0x23c;                       // +0x23c
    unsigned char Field_0x240;                      // +0x240
    FLoginHistory Logins;                           // +0x244
    unsigned int Field_0x25c;                       // +0x25c
};


extern CLoginMgr G_LoginMgr;

// Connect-state globals next to the context in baseline .data.
extern bool G_bServerDisconnected;
extern bool G_bServerConnected;
extern bool G_bCommandLineG;         // ("+G", main.cpp)
extern IServer* G_Server;
extern void* G_ChatServer;
extern long G_ConnectResult;         // (HandleConnectError)
extern bool G_bWaitingForServer;
extern char G_LocalServerDll[32];    // "Server.dll", [OutpostFrontier] local_server

// The LAN / INTERNET pick of TypeSelect's multiplayer prompt (0x00566d6b..0x00566d7a): the
// first is set to 1, the second to (choice == INTERNET). Readers, none ported yet:
// (the field set and no +0x7c -> CDPClient::EnumerateHosts),
// and 0x0056ec35 (the field).
extern unsigned char G_bEnumerateLanHosts;  // , initially 1
extern unsigned char G_bInternetServerList; // , initially 1

// pump the server until *bResponse is set, the connection drops or the
// timeout (SpTimeout in single player, MpTimeout otherwise) runs out. 1 = response,
// 0 = timeout / no server / disconnected, 2 = the operation was cancelled (MP), 4 / 5 / 6 =
// the DirectPlay connect error recorded by HandleConnectError.
int WaitForServerResponse(unsigned char* bResponse, float MpTimeout, float SpTimeout);
