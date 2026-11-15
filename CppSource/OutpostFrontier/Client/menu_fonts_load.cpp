// Author: Giperion.

#include "menu_fonts_load.h"

#include "common_ini.h"
#include "common_misc.h" // GetFileSysPath
#include "common_text.h"
#include "globals.h"                   // CopyBoundedString (the routine)
#include "ids_rdl.h"                   // RdlAppendNode

#include <string.h>
#include <windows.h>

namespace
{
    // the style id is a CRC-16/CCITT of the "name" value, seeded 0xffff,
    // byte-wise, with no final xor. EAX carries the running value and only AX is replaced by the
    // byte (MOVZX AX), so the upper half accumulates bits pushed up by the SHL 12 -- they never
    // reach the low word, and StyleCollection::set_style takes an unsigned short.
    unsigned int HashStyleName(char const* Name)
    {
        unsigned int Crc = 0xffff;
        size_t Length = strlen(Name);
        unsigned char const* Cursor = reinterpret_cast<unsigned char const*>(Name);
        while (Length != 0)
        {
            unsigned int const Swapped = ((Crc & 0xffu) << 8) | ((Crc >> 8) & 0xffu); // CL=AH, CH=AL
            Crc = (Crc & 0xffff0000u) | *Cursor;                                         // MOVZX AX
            Crc ^= Swapped;
            Crc ^= (Crc & 0xffu) >> 4;
            Crc ^= Crc << 12;
            Crc ^= (Crc & 0xffu) << 5;
            ++Cursor;
            --Length;
        }
        return Crc;
    }

    // one JustifyNode on the stack, its clone appended to the style list,
    // then the stack node destroyed.
    void AppendJustify(RenderDisplayList& StyleList, TextJustify Mode)
    {
        JustifyNode Node(Mode);
        RdlAppendNode(StyleList, Node.Clone());
    }
}

void LoadRichFontsFromIni(char const* RelPath, IFileSystem* Filesys)
{
    // .
    char FullPath[MAX_PATH];
    GetFileSysPath(FullPath, RelPath, Filesys);

    INI_Reader Reader;                                           // [ESP+0xfc]
    if (!Reader.open(FullPath, false))
        return;
    if (!Reader.read_header())
        return;

    // [ESP+0x78]/[ESP+0x7c]: the face name as a length-prefixed 0x80-byte string. Cleared when a
    // [TrueType] section starts; the length it stores is never read again.
    unsigned int FaceLength = 0;
    char FaceName[0x80];

    // [ESP+0x38]: the style id. Written only by a "name" line and never reset per section.
    // PORT: before the first "name" baseline reads whatever the stack slot holds (a [Style] with
    // no "name" registers under the previous section's id, or under garbage if it is the
    // first); the port starts it at 0.
    unsigned int StyleId = 0;

    do
    {
        if (Reader.is_header("TrueType"))
        {
            FaceLength = 0;
            FaceName[0] = '\0';
            if (!Reader.read_value())
                continue;
            do
            {
                if (!Reader.is_value("font"))
                    continue;

                unsigned int const Index = static_cast<unsigned int>(Reader.get_value_int(0));
                char const* const Face = Reader.get_value_string(1);
                if (Face == nullptr)
                {
                    FaceLength = 0;
                    FaceName[0] = '\0';
                }
                else
                {
                    FaceLength = CopyBoundedString(FaceName, 0x80, Face);
                }

                // {name, size, bold, italic, underline} on the stack --
                // the name is always the local buffer, the three style bytes always 0.
                TextRenderContext::FontDesc Desc = {};
                Desc.Name = FaceName;
                Desc.Size = static_cast<unsigned long>(Reader.get_value_int(2));
                Desc.bBold = 0;
                Desc.bItalic = 0;
                Desc.bUnderline = 0;
                if (Index < 0x20)                                // CMP ESI,0x20 / JNC
                    SetStandardFont(static_cast<int>(Index), Desc);
            } while (Reader.read_value());
            (void)FaceLength;
        }
        else if (Reader.is_header("Style"))
        {
            // attributes = {flags 0, R/G/B 0xff}, the "given" mask 0,
            // and a fresh list (its sentinel from the routine(0, 0), count 0).
            unsigned int Packed = 0xffffff00u;
            unsigned int Mask = 0;
            RenderDisplayList StyleList;

            if (Reader.read_value())
            {
                do
                {
                    if (Reader.is_value("name"))
                    {
                        StyleId = HashStyleName(Reader.get_value_string());
                    }
                    else if (Reader.is_value("font"))
                    {
                        // the font index goes into bits 3..7 of the flag
                        // byte, the three style bits are kept.
                        unsigned char const Font = static_cast<unsigned char>(Reader.get_value_int(0));
                        unsigned char const Flags = static_cast<unsigned char>(Packed);
                        unsigned char const NewFlags =
                            static_cast<unsigned char>((Font << 3) ^ (Flags & 7));
                        Packed = (Packed & 0xffffff00u) | NewFlags;
                        Mask |= 0xf8;
                    }
                    else if (Reader.is_value("bold"))
                    {
                        unsigned char const Value = static_cast<unsigned char>(Reader.get_value_bool(0));
                        unsigned char const Flags = static_cast<unsigned char>(Packed);
                        unsigned char const NewFlags =
                            static_cast<unsigned char>(Flags ^ ((Value ^ Flags) & 1));
                        Packed = (Packed & 0xffffff00u) | NewFlags;
                        Mask |= 1;
                    }
                    else if (Reader.is_value("italic"))
                    {
                        unsigned char const Value = static_cast<unsigned char>(Reader.get_value_bool(0));
                        unsigned char const Flags = static_cast<unsigned char>(Packed);
                        unsigned char const NewFlags =
                            static_cast<unsigned char>(Flags ^ (((Value << 1) ^ Flags) & 2));
                        Packed = (Packed & 0xffffff00u) | NewFlags;
                        Mask |= 2;
                    }
                    else if (Reader.is_value("underline"))
                    {
                        unsigned char const Value = static_cast<unsigned char>(Reader.get_value_bool(0));
                        unsigned char const Flags = static_cast<unsigned char>(Packed);
                        unsigned char const NewFlags =
                            static_cast<unsigned char>(Flags ^ (((Value << 2) ^ Flags) & 4));
                        Packed = (Packed & 0xffffff00u) | NewFlags;
                        Mask |= 4;
                    }
                    else if (Reader.is_value("color"))
                    {
                        // R, G, B into bytes 1, 2, 3.
                        unsigned int const Red = static_cast<unsigned char>(Reader.get_value_int(0));
                        unsigned int const Green = static_cast<unsigned char>(Reader.get_value_int(1));
                        unsigned int const Blue = static_cast<unsigned char>(Reader.get_value_int(2));
                        Packed = (Packed & 0xffu) | (Red << 8) | (Green << 16) | (Blue << 24);
                        Mask |= 0xffffff00u;
                    }
                    else if (Reader.is_value("justify"))
                    {
                        char const* const Justify = Reader.get_value_string();
                        if (_stricmp(Justify, "left") == 0)          // NEURONCOMPONENTS stricmp, 0x0057e6b6
                            AppendJustify(StyleList, TextJustifyLeft);
                        else if (_stricmp(Justify, "center") == 0)
                            AppendJustify(StyleList, TextJustifyCenter);
                        else if (_stricmp(Justify, "right") == 0)
                            AppendJustify(StyleList, TextJustifyRight);
                    }
                } while (Reader.read_value());
            }

            // TRANode(attributes, mask, 0), its clone appended after
            // any justify nodes, the stack node destroyed.
            {
                TextRenderAttributes Attrs;
                Attrs.Packed = Packed;
                TRANode Node(Attrs, Mask, 0);
                RdlAppendNode(StyleList, Node.Clone());
            }

            // the list is copied into the registry; 0x0057e941..0x0057e95d then
            // erases every node and frees the sentinel (StyleList's destructor).
            StyleCollection::set_style(static_cast<unsigned short>(StyleId), StyleList);
        }
    } while (Reader.read_header());

    // the INI_Reader's destructor, inlined.
}
