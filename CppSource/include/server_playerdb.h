// Author: Giperion.

// Server.dll's account database: ci_char_traits / ci_wchar_traits, CiString / CiWString,
// CAccount, PlayerDB and the `Players` singleton. Split out of common_server.h (which
// includes it back, so every existing consumer is unchanged) because OutpostServer.exe needs
// these types in MFC translation units, where common_server.h's closure (common_cobject.h,
// OutpostFrontier's `class CObject`) collides with MFC's CObject. This header's include closure
// must stay free of common_cobject.h.
#pragma once

#include "server_exports.h"         // SERVER_API
#include "common_server_types.h"    // SLoginInfo, SCreateCharacterInfo
#include "common_net.h"             // CPlayerAccount (by value in PlayerDB at +0x10)
#include "common_server_playerdb.h" // PlayerData (PlayerDB's slot array)
#include <cstring>   // _strnicmp (ci_char_traits)
#include <cwchar>    // _wcsnicmp (ci_wchar_traits)
#include <list>
#include <map>
#include <string>

// ============================================================================
// ci_char_traits / ci_wchar_traits -- baseline's case-insensitive string traits
// ============================================================================
// Every PlayerDB/CAccount export that takes an account or character name uses
// one of these; baseline's mangling, e.g.
//   ?FindAccountFromName@PlayerDB@@QAEPAVCAccount@@AAV?$basic_string@G
//     Uci_wchar_traits@@V?$allocator@G@std@@@std@@@Z
// shows a `struct ci_wchar_traits` over VC6's wchar_t (which VC6 spelled
// `unsigned short`, G) and a `struct ci_char_traits` over char. Ours are the same
// traits over the real wchar_t, so our decorated names carry _W where baseline's
// carry G (project rule, 2026-09-24: modern types, same functions). The only member baseline's code shows
// is compare, as basic_string::compare inlines it for the two account maps:
// _wcsnicmp(lhs, rhs, min(len)); equal -> shorter is less
// _strnicmp(lhs, rhs, min(len)); same tail
// Everything else is inherited from std::char_traits unchanged.
struct ci_char_traits : public std::char_traits<char>
{
    static int compare(const char* Lhs, const char* Rhs, size_t Count)
    {
        return _strnicmp(Lhs, Rhs, Count);
    }
};

struct ci_wchar_traits : public std::char_traits<wchar_t>
{
    static int compare(const wchar_t* Lhs, const wchar_t* Rhs, size_t Count)
    {
        return _wcsnicmp(Lhs, Rhs, Count);
    }
};

// basic_string<wchar_t, ci_wchar_traits>: account and character names.
typedef std::basic_string<wchar_t, ci_wchar_traits> CiWString;
// basic_string<char, ci_char_traits>: "<hash>.fl" character ids.
typedef std::basic_string<char, ci_char_traits> CiString;

// (FAccountInfo is gone, 2026-09-24 claude-code:w5-server-06d4: baseline's two
// PlayerDB maps store a bare CAccount* at node+0x1c, and the locked / not-banned
// / admin bytes and the client id live in the CAccount itself at +0x30..+0x34.
// The port's copies of those flags in the map value drifted from the account.)

// ============================================================================
// CAccount
// ============================================================================
// baseline Server.dll exports exactly these CAccount members: the four special
// members, ??_7, AppendCharacterNames, DeleteCharacterFromID, ForceLogout and
// InitFromFolder. The accessors the port used to declare here (GetName,
// IsAdmin, SetClientID, Save, ...) were inventions that became exports of
// their own; the data is public now and server_classes.cpp reads it directly,
// as baseline's PlayerDB code does with raw offsets.
class SERVER_API CAccount {
public:
    CAccount();
    CAccount(const CAccount& Other);
    virtual ~CAccount();
    CAccount& operator=(const CAccount& Other);

    // Server module storage: `RET 4` -- the body is empty (ICF-folded with the
    // empty IServerImpl slots and ContentCallback::scan_state_change).
    void AppendCharacterNames(std::list<CiWString>& Names);
    void DeleteCharacterFromID(CiString& CharId);
    // Server module storage, returns void.
    void ForceLogout();
    void InitFromFolder(const char* FolderPath);

    // Member order is baseline's 0x38-byte layout, read off the default ctor
    // (module storage .. module storage):
    //   +0x00  vftable                     (module storage)
    //   +0x04  basic_string<G, ci_wchar_traits>  the account name
    //          (module storage .. 06d567ff; same type as the +0x44 map key --
    // copy-constructs the key from it with the
    //          `this == &src` self-test of basic_string(const basic_string&))
    //   +0x14  narrow std::string           the account directory
    //          (module storage .. 06d56813)
    //   +0x24  std::list<CiWString>         character names (module storage ..
    // self-linked 0x18-byte head at +0x28, size at +0x2c)
    //   +0x30  bool  locked                 (module storage, 0)
    //   +0x31  bool  NOT banned             (module storage, 1)
    //   +0x32  bool  admin                  (module storage, 0)
    //   +0x34  unsigned int client id       (module storage, 0)
    // The containers are the modern std ones (no baseline ABI, AGENTS.md).
    // baseline has no password member: the port's m_Password[16] and m_bValid
    // were inventions and are gone (2026-09-24, claude-code:w5-server-06d4).
    CiWString m_Name;
    // baseline CAccount+0x14, a narrow std::string holding the account's own
    // directory on disk. Written by the internal-login helper the routine and
    // read back by PlayerDB::logout for its FDUMP line. Raw bytes, wave 6:
    // 8b9d00040000  MOV  EBX,[EBP + 0x400]   ; slot->account_ptr
    // 83c314        ADD  EBX,0x14            ; &account->m_DirectoryPath
    // 8bfe          MOV  EDI,ESI             ; ESI = the directory path
    // f2ae          SCASB.REPNE ES:EDI       ; strlen
    // ff15844bd606  CALL [MSVCP60::basic_string::_Grow]
    // 8b7b04        MOV  EDI,[EBX + 0x4]     ; str._Ptr
    // f3a5          MOVSD.REP ES:EDI,ESI
    // 894308        MOV  [EBX + 0x8],EAX     ; str._Len
    // c60300        MOV  byte ptr [EBX],0x0  ; _Ptr[len] = 0
    // (_Ptr at +0x04 inside the string is why logout reads account+0x18.)
    std::string m_DirectoryPath;
    // The character list baseline keeps at CAccount+0x24 (std::list, head at
    // +0x28, size member at +0x2c). It holds the character NAMES, wide and
    // case-insensitive, not the ".fl" file names -- the element type of
    // AppendCharacterNames' list -- and CAccount::DeleteCharacterFromID
    // (0x06d56bd0) hashes EVERY entry back into a "<hash>.fl" through
    // before comparing it with the id it was given.
    std::list<CiWString> m_Characters;
    // +0x30: set by PlayerDB::LockAccountAccess (module storage), cleared by
    // UnlockAccountAccess (module storage); the routine refuses a login with 7
    // while it is set (module storage).
    bool m_bLocked;
    // +0x31: 1 = the account may log in. InitFromFolder clears it when a
    // "banned" file exists (module storage); the routine returns 0 when it is
    // clear (module storage); GetAccountBanned returns its negation (SETZ at
    // ).
    bool m_bNotBanned;
    // +0x32: InitFromFolder sets it for an "admin" file (module storage);
    // GetAccountAdminRights returns it (module storage).
    bool m_bAdmin;
    // +0x34: the logged-in client, 0 when none (the routine module storage).
    unsigned int m_ClientID;
};

// ============================================================================
// PlayerData & PlayerDB
// ============================================================================
// PlayerData used to be declared in common_server.h a second time, as an opaque
// `unsigned char data[0x418]` blob, while common_server_playerdb.h carried the
// real record under the name PlayerSlot. PlayerDB::m_PlayerArray was calloc'd
// from the blob version and poked at baseline byte offsets, and everything else
// in the port wrote the other one; the two never met. There is one definition
// now and it is the one in common_server_playerdb.h.

class SERVER_API PlayerDB {
    // The port's own C accessor (common_server_playerdb.h) for the
    // allow-new-players flag at [this+0x0c]; not a baseline function, and a friend
    // declaration exports nothing.
    friend bool __cdecl PlayerDB_AllowsNewPlayers();
public:
    PlayerDB();
    PlayerDB(const PlayerDB& Other);
    ~PlayerDB();
    PlayerDB& operator=(const PlayerDB& Other);

    void init(unsigned int MaxPlayers, bool bAllowNewPlayers);
    void free();
    void cleanup(unsigned int SessionId);
    unsigned int GetMaxPlayerCount();
    unsigned int GetServerID();
    const char* GetServerSig();
    bool is_valid(const unsigned int& Id);
    // Argument order per the raw listing at 0x06ce1a40: the FIRST reference is
    // the PLAYER id (TEST EDX,EDX / CMP (id-1) against [this+0x4] /
    // [base+(id-1)*0x418+0x348]), the second is the ship flid. The declaration
    // here used to name them the other way round.
    bool is_valid_ship_owner(const unsigned int& PlayerId, const unsigned int& ShipFlid);
    PlayerData& operator[](const unsigned int& Id);
    PlayerData* traverse_active(PlayerData* Current) const;
    unsigned char login(const SLoginInfo& Info, unsigned int SessionId);
    void logout(unsigned int SessionId);
    void logout_all();
    const wchar_t* GetActiveCharacterName(unsigned int SessionId) const;
    unsigned int CountPlayersInSystem(int SystemId);
    unsigned int GetGroupID(unsigned int PlayerId);
    void SendGroupID(unsigned int PlayerId, unsigned int GroupId);
    void SendSystemID(unsigned int PlayerId, unsigned int SystemId);
    void BuildLocalUserDir();
    bool MakeLocalUserPath(char* OutPath, const char* FileName);
    CAccount* FindAccountFromClientID(unsigned int ClientId);
    bool create_restart_file(const char* FileName);
    bool create_new_character(const SCreateCharacterInfo& Info, unsigned int SessionId);

    // Name/id parameters are non-const references, exactly as baseline mangles
    // them (AAV?$basic_string@GUci_wchar_traits@@...; G is VC6's wchar_t).
    void UnlockAccountAccess(CiWString& CharName);
    void ReadCharacterName(const char* FilePath, CiWString& OutName);
    void SetMOTD(std::wstring& Message);
    std::wstring& GetMOTD();
    void DeleteCharacterFromID(CiString& CharId);
    bool DeleteCharacterFromName(CiWString& CharName);

    bool CreateAccount(CiWString& AccountName);
    void DeleteAccount(CiWString& AccountName);
    bool BanAccount(CiWString& AccountName, bool bBan);
    bool GetAccountBanned(CiWString& AccountName);
    bool SetAccountPassword(CiWString& AccountName, CiWString& Password);
    bool GetCharactersForAccount(CiWString& AccountName, std::list<CiWString>& OutCharacters);

    CAccount* FindAccountFromName(CiWString& Name);
    CAccount* FindAccountFromCharacterName(CiWString& CharName);
    CAccount* FindAccountFromCharacterID(CiString& CharId);
    bool GetAccountAdminRights(CiWString& Name);
    bool SetAccountAdminRights(CiWString& Name, bool bAdmin);
    void LockAccountAccess(CiWString& Name);

private:
    // baseline marks these three `private:` (?create_account@PlayerDB@@AAE...,
    // ?load_user_data@PlayerDB@@AAE..., ?to_index@PlayerDB@@AAE...).
    int create_account(const SLoginInfo& Info);
    unsigned char load_user_data(const SLoginInfo& Info, unsigned int SessionId);
    unsigned int to_index(unsigned int SessionId);

protected:
    // Member order follows baseline's, read off the ctor at 0x06d50b10 and
    // init at 0x06d510c0:
    //   +0x00  PlayerData*  slot array base   (ctor: MOV [ESI],EBX)
    //   +0x04  unsigned int MaxPlayers        (init: MOV [ESI+0x04],EDI)
    //   +0x08  unsigned int ServerID          (init: sscanf(sig,"%x",this+8);
    //                                          IServerImpl::GetServerID at
    // reads [0x06d90268],
    //                                          which is &Players + 0x8)
    //   +0x0c  bool         bAllowNewPlayers  (init: MOV [ESI+0x0c],AL; from
    //                                          SStartupInfo +0x211, gates account
    //                                          creation at module storage)
    //   +0x10  CPlayerAccount                 (ctor: LEA ECX,[ESI+0x10] / CALL
    //                                          COMMON.DLL::CPlayerAccount::
    //                                          CPlayerAccount)
    //   +0x1c  PlayerData*  slot array end    (init: MOV [ESI+0x1c],EDX)
    //   +0x20  std::wstring MOTD              (VC6 basic_string: allocator byte
    //                                          +0x20, _Ptr/_Len/_Res zeroed at
    //                                          +0x24/+0x28/+0x2c)
    //   +0x30  character-id container (0x14 bytes)
    //   +0x44  account-name container (0x14 bytes)
    // m_ServerSig was an invention -- baseline has no such field, +0x0c is the
    // allow-new-players flag and GetServerSig goes through the CPlayerAccount at
    // +0x10 (see GServerAccount in server_classes.cpp). m_Accounts was a third
    // map baseline has no room for and nothing in src/ ever referenced.
    PlayerData* m_PlayerArray;
    unsigned int m_MaxPlayers;
    unsigned int m_ServerID;
    bool m_bAllowNewPlayers;
    // baseline's CPlayerAccount subobject at PlayerDB+0x10 (sizeof 0x0C, so it
    // runs to +0x1c, exactly where the array-end pointer starts). It was a
    // translation-unit singleton `GServerAccount` in server_classes.cpp until
    // 2026-09-17; that was only ever a way to keep common_net.h out of this
    // header's include graph, and PlayerDB being a singleton made it work by
    // accident. It is the real member now, which is what GetServerSig
    // (0x06ce1ac0, ADD ECX,0x10) and GetServerID (0x06d453d0, reading
    // == &Players + 0x8) actually address.
    CPlayerAccount m_ServerAccount;
    PlayerData* m_PlayerArrayEnd;
    std::wstring m_MOTD;
    // Account lookup maps (baseline: char-ID map at this+0x30, name map at this+0x44),
    // ordered by the ci traits' compare exactly like baseline's std::less keys.
    // Values are bare CAccount* as in baseline (node+0x1c); the name map owns
    // them (PlayerDB::~PlayerDB deletes through it).
    std::map<CiString, CAccount*> m_CharacterIDMap;
public:
    // Public: OutpostServer.exe walks this map inline (Players+0x48 head, node value CAccount*,
    // accounts dialog module storage); PlayerDB exports no accessor for it.
    std::map<CiWString, CAccount*> m_AccountNameMap;
};

// baseline's one and only PlayerDB: `?Players@@3VPlayerDB@@A`, Server.dll ordinal
// 352, module storage. analysis prints its readers as `MOV EDX,[.data:Players]`
// (the slot array base, first member) and its method calls as
// `MOV ECX,.data:Players` (the object's address) -- for example 0x06ce904b/
// , which is `Players.operator[](...)`.
extern SERVER_API PlayerDB Players;
