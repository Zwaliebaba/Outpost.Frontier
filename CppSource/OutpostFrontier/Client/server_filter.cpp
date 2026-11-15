// Author: Giperion.

#include "server_filter.h"

#include "globals.h"   // g_client.packed_version (the field)
#include "login_mgr.h" // FServerInfo, G_LoginMgr (the field)

// .
ServerFilter::ServerFilter()
    : bFilterPing(0)
    , bFilterKnown(0)
    , bFilterVersion(0)
    , bFilterField95(0)
    , bFilterPassword(0)
    , bFilterField3e(0)
    , PingMin(0)
    , PingMax(1000)
    , KnownLogin(1)
    , VersionMin(g_client.packed_version)
    , VersionMax(g_client.packed_version)
    , Field95(1)
    , Password(0)
    , Field3e(1)
{
}

// .
ServerFilter& ServerFilter::operator=(ServerFilter const& Other)
{
    bFilterPing = Other.bFilterPing;
    bFilterKnown = Other.bFilterKnown;
    bFilterVersion = Other.bFilterVersion;
    bFilterField95 = Other.bFilterField95;
    bFilterPassword = Other.bFilterPassword;
    bFilterField3e = Other.bFilterField3e;
    PingMin = Other.PingMin;
    PingMax = Other.PingMax;
    KnownLogin = Other.KnownLogin;
    VersionMin = Other.VersionMin;
    VersionMax = Other.VersionMax;
    Field95 = Other.Field95;
    Password = Other.Password;
    Field3e = Other.Field3e;
    return *this;
}

// .
bool ServerFilter::Match(FServerInfo const& Server) const
{
    // JA past the maximum, JNC from the minimum up.
    if (bFilterPing)
    {
        unsigned const Ping = static_cast<unsigned>(Server.Ping);
        if (Ping > PingMax || Ping < PingMin)
            return false;
    }
    // the known-login byte compared with +0x14.
    if (bFilterKnown)
    {
        unsigned char const bKnown = G_LoginMgr.IsKnownLogin(Server) ? 1 : 0;
        if (bKnown != KnownLogin)
            return false;
    }
    if (bFilterVersion)
    {
        if (Server.Version > VersionMax || Server.Version < VersionMin)
            return false;
    }
    if (bFilterField95 && Server.Field_0x95 != Field95)
        return false;
    if (bFilterPassword && Server.bPassword != Password)
        return false;
    if (bFilterField3e && Server.Field_0x3e != Field3e)
        return false;
    return true;
}
