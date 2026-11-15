// Author: Giperion.

#pragma once

// Private DX9RENDERER hardware-quirk database for flconfigdatabase.txt: a copy of src/DX8RENDERER/Mgsdb.h
// (mirror DX8RENDERER fixes here). The adapter identifier and caps keep their D3D8 shape
// (Dx9RendererD3d8Types.h), so the database sees exactly the fields DX8RENDERER gives it.

#include "Dx9RendererD3d8Types.h"

#include <map>
#include <string>
#include <vector>

class FMgsdbDatabase;

// One 0x20-byte record of a record set: name string +0x00, value string +0x10.
struct FMgsdbRecord
{
    std::string Name;
    std::string Value;
};

// (operator new(0x14)): a vector of records (+0x4..+0xC) and its owner (+0x10).
// The database's live set sits at object+0x18; every "propertyset" definition gets its own.
struct FMgsdbRecordSet
{
    explicit FMgsdbRecordSet(FMgsdbDatabase* InOwner);

    // overwrite the value of the record whose name _stricmp-matches, else append.
    void Insert(const char* Name, const char* Value);
    // strncpy the value of the record whose name _stricmp-matches into Out.
    bool Find(const char* Name, char* Out, size_t OutSize) const;

    std::vector<FMgsdbRecord> Records;
    FMgsdbDatabase* Owner;
};

// The 0x584-byte match object (vtable 06d5eaf4). Field offsets are the baseline ones.
class FMgsdbDatabase
{
public:
    // operator new(0x584) + the routine.
    static FMgsdbDatabase* Create();

    FMgsdbDatabase();  
    ~FMgsdbDatabase(); 

    // Slot 0, the routine (thiscall, RET 0x18).
    bool Load(const char* Path, const FDx9RendererIdentifier8* Ident, const FDx9RendererCaps8* Caps,
              unsigned long Ram, unsigned long VideoRam, unsigned long CpuSpeed);
    // Slot 1, the routine: destroy + operator delete.
    void Destroy();
    // Slot 2, the routine.
    FMgsdbRecordSet* GetRecords();
    // Slot 3, the routine: the record set of a propertyset definition, or null.
    FMgsdbRecordSet* FindPropertySet(const char* Name);
    // Slot 7, the routine: +0x30 (written by the device match of the routine).
    const char* GetDeviceName() const;
    // Slot 8, the routine: +0x40 (written by the vendor match of the routine).
    const char* GetVendorName() const;
    // Slot 9, the routine: +0x1C.
    bool HasError() const;
    // Slot 10, the routine: +0x20.
    const char* GetError() const;

private:
    // Scanner primitives.
    void SkipLine();                          
    void SkipWhite();                         
    void SetError(const char* Message);       
    int ReadHexDigit();                       
    unsigned long ReadNumber();               
    const char* ReadQuoted();                 
    const char* ReadFlag(FMgsdbRecordSet* Target); 
    unsigned long ReadGuidWord();             
    unsigned long ReadGuidWordSwapped();      
    bool Match(const char* Keyword) const;    
    const char* EvaluateIf();                 

    // Passes of Load.
    bool ReadPropertySets();                  
    bool ReadLeadingApplyToAll();             
    bool ReadVendorDevice();                  
    bool ReadTrailingApplyToAll();            
    bool ReadBlock(FMgsdbRecordSet* Target);  

    char* Buffer_;                  // +0x04
    const char* Cursor_;            // +0x08
    const char* End_;               // +0x0C
    const char* LineStart_;         // +0x10
    unsigned long Line_;            // +0x14
    FMgsdbRecordSet* Records_;      // +0x18
    bool bError_;                   // +0x1C
    std::string Error_;             // +0x20
    std::string DeviceName_;        // +0x30
    std::string VendorName_;        // +0x40
    unsigned long CpuSpeed_;        // +0x50
    unsigned long Ram_;             // +0x54
    unsigned long VideoRam_;        // +0x58
    FDx9RendererIdentifier8 Ident_;         // +0x5C (D3DADAPTER_IDENTIFIER8 shape)
    FDx9RendererCaps8 Caps_;                // +0x488 (D3DCAPS8 shape)
    // +0x55C: propertyset name -> its record set (owned; deleted by the destructor).
    std::map<std::string, FMgsdbRecordSet*> PropertySets_;
    // +0x570: OverallGraphicDetail value -> the record set that set it (not owned).
    std::map<std::string, FMgsdbRecordSet*> DetailSets_;
};

// (cdecl): every record of Source is inserted into Target (the routine).
void MgsdbReplay(FMgsdbRecordSet* Target, const FMgsdbRecordSet* Source);
