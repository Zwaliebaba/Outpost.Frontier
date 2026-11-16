// Author: Giperion.

#pragma once

// TU41 system connections; CNT-POP extends with the routine.

// Content.dll TU41 (06f29160-06f2c3xx): the jump-gate / jump-hole connection helpers over
// pub::System::EnumerateConnections, and the three system-path tables
// every connection        (ConnectionType 0, CONN_TYPE_ALL)
// jump gates only         (ConnectionType 1, CONN_TYPE_JUMP_GATE)
// jump holes only         (ConnectionType 2, CONN_TYPE_JUMP_HOLE)
// each a map<from system, map<to system, list<system id>>> holding the system path from
// "from" to "to". The tables are filled only by the routine (CNT-POP, not ported yet), so
// until then every path lookup below finds nothing -- the baseline readers are ported 1:1 and
// the filler is the open gap.

#include "common_ini.h"     // Vector, INI_Reader
#include "common_server.h"  // ConnectionType

#include <list>
#include <map>

// map<to system, list<system id>> -- the inner node's value list is the path itself.
typedef std::map<unsigned int, std::list<unsigned int>> FSystemPathsFrom;
// map<from system, FSystemPathsFrom>.
typedef std::map<unsigned int, FSystemPathsFrom> FSystemPathTable;

extern FSystemPathTable GSystemPathsAll;   // (type 0)
extern FSystemPathTable GSystemPathsGate;  // (type 1)
extern FSystemPathTable GSystemPathsHole;  // (type 2)

// size of the From->To path in the table for Type, -1 if none.
int GetSystemPathLength(unsigned int FromSystemId, unsigned int ToSystemId, int Type);

// the connection object in FromSystemId leading to the second system of the
// From->To path (0 when there is none).
unsigned int GetNextHopConnectionObject(unsigned int FromSystemId, unsigned int ToSystemId, int Type);

// copy of the From->To path in the table for Type (empty if none).
std::list<unsigned int> GetSystemPath(unsigned int FromSystemId, unsigned int ToSystemId, int Type);

// position of connection object ObjectId in SystemId; 0 found, -2 not.
int GetConnectionPosition(unsigned int SystemId, unsigned int ObjectId, Vector* OutPosition);

// destination system of connection object ObjectId in SystemId; 0 found,
// -2 not.
int GetConnectionGotoSystem(unsigned int SystemId, unsigned int ObjectId, unsigned int* OutGotoSystemId);

// ---------------------------------------------------------------------------------------
// Filling the tables (lane CNT-POP).
// ---------------------------------------------------------------------------------------

// (cdecl): DATA\Universe\systems_shortest_path.ini exists -> LoadSystemPathTables,
// else BuildSystemPathTables(true). Caller GameSupport msg 0x40 (06effe92).
void __cdecl InitSystemPathTables();

// (cdecl): the three tables are emptied and read back from
// systems_shortest_path.ini (every connection), shortest_legal_path.ini (jump gates) and
// shortest_illegal_path.ini (jump holes).
void __cdecl LoadSystemPathTables();

// (cdecl): one [SystemConnections] section into Table.
void __cdecl LoadSystemPathSection(INI_Reader* Reader, FSystemPathTable* Table);

// (cdecl): the tables computed from the universe (a shortest-path search per
// system and connection type, locked gates left out); runs once per process (the field).
// bWrite: the three ini files are written afterwards (the routine).
void __cdecl BuildSystemPathTables(bool bWrite);

// (cdecl): the three tables into data\Universe\*.ini, systems by nickname.
void __cdecl WriteSystemPathTables();

// (cdecl): Out += the destination systems of SystemId's connections of Type
// whose object is not locked for everyone (TU53 the routine with player 0).
void __cdecl CollectOpenConnectionSystems(unsigned int SystemId, std::list<unsigned int>* Out,
                                          ConnectionType Type);

// (cdecl): Out = the first of the shortest paths of Paths (Out untouched when
// Paths is empty); returns its length, -1 when Paths is empty.
int __cdecl CopyShortestPath(std::list<std::list<unsigned int>>* Paths, std::list<unsigned int>* Out);
