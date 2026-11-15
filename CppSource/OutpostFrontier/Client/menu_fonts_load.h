// Author: Giperion.

#pragma once

// (rich_fonts.ini). fonts.ini (the routine) is font_imp.h LoadFonts; the
// TrueType fonts it builds are found by nickname with FindMenuFontImp (the routine).

struct IFileSystem;

// , cdecl(value, filesystem): GetFileSysPath(path, RelPath, Filesys) first
// (0x0057e341..0x0057e34b). the routine passes NeuronRuntime::Data. [TrueType] "font" lines go to
// COMMON SetStandardFont, each [Style] becomes a RenderDisplayList handed to
// StyleCollection::set_style under the CRC-16 of its "name".
void LoadRichFontsFromIni(char const* RelPath, IFileSystem* Filesys);
