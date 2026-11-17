// MaterialAnimation.cpp of Shading.dll compiled into the oracle harness, so that its file-local
// ParseMaKeys can be called by oracle_shading.cpp (work package P1.7).

#include "oracle_support.h"

#include "../Shading/MaterialAnimation.cpp"

void OracleParseMaKeys(IFileSystem* FileSystem, FMaKeyBlock* Block)
{
    ParseMaKeys(FileSystem, Block);
}
