// Author: Giperion.

#pragma once

// baseline UIFastTable (RTTI .?AUUIFastTable@@ : BaseRichTextWin, vftable 0x005e511c + the
// IDataViewOwner face 0x005e5110) and UIScrollTable (.?AUUIScrollTable@@ : UIFastTable, vftable
// + face 0x005e5240), plus the nested reference-counted row payload
// UIFastTable::RefCounter. Source path in the listing: Client\Interface\
// UIFastTable.cpp.

// A UIFastTable is ONE rich-text surface (the BaseRichTextWin image) laid out as a grid: every
// cell keeps a source RenderDisplayList, and Relayout (slot 45) lays each cell's list into the
// cell's pixel rectangle and chains all of them into the base class's LayoutList. Row and column
// edges are draggable, rows can be selected / hovered, and tool tips can be keyed per column, row
// or cell. UITable (ui_table.h) is the older sibling built from one BaseRichTextWin per cell.

// Factories: WINFACTORY<UIFastTable> the field (static init the branch -> the routine
// "UIFastTable", atexit the branch; Create the routine = operator_new(0x578) + the routine +
// slot 9) and WINFACTORY<UIScrollTable> the field (the branch -> the routine
// "UIScrollTable", atexit the branch; Create the routine = operator_new(0x58c) + the routine
// + the UIScrollTable vftables and five zeroed fields, then slot 9).

// UIFastTable vftable 0x005e511c overrides of BaseRichTextWin 0x005e459c:
//   0 the routine (dtor the routine), 4 the branch HitTest, 5 the branch SetPosition,
//   8 the branch SetTransform, 9 the routine Create, 11 the routine Update,
//   13 the branch Draw, 16 the branch MouseLeave, 19 the branch MouseEvent,
//   45 the routine Relayout, 46 the branch GetToolTipHoverInfo.
// UIScrollTable vftable 0x005e524c: 0 the routine (dtor the routine = the UIFastTable dtor
//   inlined), 9 the routine Create, 23 the branch Slot23; every other slot is UIFastTable's.

// Layout (after BaseRichTextWin's 0x488; the port keeps the members, not the VC6 containers):
//   +0x488 Width, +0x48c Height (payload [0], [1])
//   +0x490 vector<FUiFastTableCell> (rows * columns, index columns * row + column)
//   +0x4a0 vector<float> row edges (y, 0 at the top, going negative)
//   +0x4b0 vector<float> column edges (x, 0 at the left)
//   +0x4c0 vector<FUiFastTableRow> (one per row)
//   +0x4d0..+0x4d7 not touched by any UIFastTable function
//   +0x4d8 bSelected, +0x4dc selected row, +0x4e0 hovered column, +0x4e4 hovered row (the cell
//          under the mouse, -1 when none), +0x4e8 bHovered, +0x4ec hovered (highlighted) row
//   +0x4f0 frame RGBA (the field), +0x4f4 selected-row RGBA (the field), +0x4f8
//          hovered-row RGBA (the field)
//   +0x4fc edge grab distance 0.004f, +0x500 bColumnsResizable, +0x501 bRowsResizable
//   +0x504 inverse of the +0x3c transform (12 floats); +0x534..+0x53f not touched
//   +0x540 bDragArmed, +0x541 bDragging, +0x542 bDragColumn, +0x544 drag edge, +0x548 offset
//   +0x54c LayoutRichText's bool argument, +0x54d row lines, +0x54e column lines, +0x54f border,
//   +0x550..+0x553 border top / bottom / left / right, +0x554 row highlight quads
//   +0x558 header row count, +0x55c tool tip mode, +0x560 vector<FUiFastTableTip>
//   +0x570 horizontal cell margin, +0x574 vertical cell margin (pixels)
//   size 0x578
// UIScrollTable adds +0x578 callback, +0x57c item count, +0x580 fixed rows, +0x584 callback
// context, +0x588 the vertical UIScroll; size 0x58c.

#include "rich_text_win.h"
#include "tooltip_hover.h"

#include "common_text.h"
#include "x86math.h"

#include <vector>

class UiScroll;

// baseline UIFastTable::RefCounter (RTTI .?AVRefCounter@UIFastTable@@). vftable 0x005cfd94 has one
// slot, the scalar deleting destructor the branch; the destructor body the routine only
// restores the vftable. Layout: +0x00 vftable, +0x04 reference count. The constructor is always
// inlined (0x005712a2: vftable, then +0x04 = 1).
class UiFastTableRefCounter
{
public:
    UiFastTableRefCounter()
        : RefCount(1)
    {
    }

    // slot 0 -- the branch / the routine
    virtual ~UiFastTableRefCounter();

    // Inlined in every pointer copy (0x0059043a, 0x00599a49): INC [obj+4].
    void AddRef()
    {
        ++RefCount;
    }

    // (thiscall): if the count is non-zero decrement it; if it is then zero, set it
    // back to 1 and call vftable slot 0 with 1 (delete this) and return 0, else return the count.
    unsigned Release();

    unsigned RefCount; // +0x04

private:
    UiFastTableRefCounter(UiFastTableRefCounter const&) = delete;
    UiFastTableRefCounter& operator=(UiFastTableRefCounter const&) = delete;
};

// The one-pointer handle rows keep to a RefCounter. baseline has no class name for it in RTTI
// (it is not polymorphic); its members are the functions below.
class UiFastTableRefPtr
{
public:
    UiFastTableRefPtr()
        : Object(nullptr)
    {
    }

    // The pointer half of the routine / the routine: zero, then the inlined assignment (the
    // release of the just-zeroed pointer is dead), i.e. copy the pointer and AddRef it.
    UiFastTableRefPtr(UiFastTableRefPtr const& Other);

    // / the pointer half of the routine and the routine -- Reset().
    ~UiFastTableRefPtr();

    // (RET 4): release the held object first (the Release() body inline, then
    // pointer = 0), then take Other's pointer and AddRef it. No self-assignment test: assigning a
    // pointer to itself while it holds the last reference deletes the object first, as in baseline.
    UiFastTableRefPtr& operator=(UiFastTableRefPtr const& Other);

    // (RET 4) -- nothing when Raw is already held; otherwise the held object
    // released (Release() inline, pointer = 0), Raw taken and AddRef'd when not null. Returns
    // the held pointer. Callers: 0x00569648, 0x0056ecf6 (Raw = 0), 0x005711e1.
    UiFastTableRefCounter* Assign(UiFastTableRefCounter* Raw);

    // (thiscall): held object -> Release(), pointer = 0.
    void Reset();

    UiFastTableRefCounter* Object; // +0x00
};

// One +0x490 element (0x1c bytes). Copy construction is the routine (two list copies and the
// byte), assignment the routine, destruction the routine (the +0x0c list first, then +0x00):
// exactly the implicit members over these three fields.
struct FUiFastTableCell
{
    // Create's prototype cell (0x0058e120..0x0058e13c): two empty lists, byte 0.
    FUiFastTableCell()
        : bSpanToEnd(0)
    {
    }

    RenderDisplayList Source;   // +0x00 -- what the owner puts in the cell (GetCell / ClearCell)
    RenderDisplayList Laid;     // +0x0c -- Relayout's output: a ClipNode, then the laid text
    unsigned char bSpanToEnd;   // +0x18 -- the routine; Relayout clips to the last column edge
};

// One +0x4c0 element (12 bytes).
struct FUiFastTableRow
{
    // Create's prototype row (0x0058e1c4..0x0058e1d9): {1, 1, 1, 0, null}.
    FUiFastTableRow()
        : bHoverable(1)
        , bSelectable(1)
        , bRowLine(1)
        , GroupId(0)
    {
    }

    // (construct-in-place): the three bytes and the handle are copied, GroupId is
    // NOT -- it is written 0 (0x00590af2 MOV [ESI+4],EBX with EBX = 0). So every row a resize adds,
    // and every row a reallocation moves, starts with GroupId 0.
    FUiFastTableRow(FUiFastTableRow const& Other)
        : bHoverable(Other.bHoverable)
        , bSelectable(Other.bSelectable)
        , bRowLine(Other.bRowLine)
        , GroupId(0)
        , Payload(Other.Payload)
    {
    }

    // the three bytes, GroupId, then the handle (the routine inline).
    FUiFastTableRow& operator=(FUiFastTableRow const& Other)
    {
        bHoverable = Other.bHoverable;
        bSelectable = Other.bSelectable;
        bRowLine = Other.bRowLine;
        GroupId = Other.GroupId;
        Payload = Other.Payload;
        return *this;
    }

    // the handle's release (Release() inline, pointer = 0).
    ~FUiFastTableRow()
    {
    }

    unsigned char bHoverable;  // +0x00 -- Update hovers only rows with this set
    unsigned char bSelectable; // +0x01 -- selection needs this and +0x00
    unsigned char bRowLine;    // +0x02 -- Draw's row separator above this row (+0x54d)
    unsigned GroupId;          // +0x04 -- Draw outlines the hovered row's run of equal ids
    UiFastTableRefPtr Payload; // +0x08
};

// baseline FTableEdges: what GetEdges (the routine) fills and SetEdges (the routine) reads --
// the row edges at +0x00 and the column edges at +0x10 (two VC6 vector<float>).
struct FRegKeyStack;

struct FUiFastTableEdges
{
    // (RET 4) -- the column edges as REG_BINARY "Columns" and the row edges as
    // "Rows" under Stack's top key (either skipped when its vector is empty). Always true.
    // Callers: PageCharacterSelect (0x00567445), PageServerSelect the routine (0x0056db95).
    bool SaveToRegistry(FRegKeyStack* Stack) const;
    // (RET 4) -- read both values back when the second query returns the size the
    // first one reported (a failed query counts as size -1 both times); each vector is emptied,
    // resized to size / 4 and filled. Always true. Callers: 0x00567b0b, 0x0056ea37.
    bool LoadFromRegistry(FRegKeyStack* Stack);

    std::vector<float> Rows;    // +0x00
    std::vector<float> Cols;    // +0x10
};

// One +0x560 element: a ToolTip hover record (0x64 bytes, tooltip_hover.h) with the copy and
// teardown the vector instantiation gives it.
struct FUiFastTableTip
{
    // SetTipMode's prototype (0x0058d647..0x0058d677 and the two sibling blocks): caption buffer
    // empty, the Font::String (the routine), then the routine (key -1, IdsAlt 0, AuxField 0,
    // live 0). baseline leaves +0x48..+0x5f (WorldPos, HalfExtents) as stack garbage and every
    // copy inherits it; the port zeroes them.
    FUiFastTableTip();

    // (construct-in-place): key and IdsAlt, a deep copy of the caption (wcslen,
    // malloc, wcsncpy), the Font::String copied member by member (its text deep-copied through
    // , its child slots copied with an AddRef each through the routine /
    // ), then AuxField, WorldPos, HalfExtents and the live byte.
    FUiFastTableTip(FUiFastTableTip const& Other);

    // the Font::String teardown (the routine, the child releases, the child
    // vector and text freed), then the caption freed.
    ~FUiFastTableTip();

    FToolTipHoverInfo Info;

private:
    // baseline's assignment (the inlined record copy around the routine in the routine /
    // ) only runs when elements shift inside the vector; UIFastTable only ever
    // erases the whole vector and grows it from empty (the routine), so no shift happens.
    FUiFastTableTip& operator=(FUiFastTableTip const&) = delete;
};

class UiFastTable : public BaseRichTextWin
{
public:

    UiFastTable();
    // (scalar deleting the routine)
    ~UiFastTable() override;

    // slot 4 the branch (RET 4)
    bool HitTest(float const* Ray) override;
    // slot 5 the branch (RET 4)
    void SetPosition(float const* NewPosition) override;
    // slot 8 the branch (RET 4)
    void SetTransform(float const* Transform) override;
    // slot 9 the routine (RET 8); Data = FUiPayloadTable {Width, Height, rows, columns}.
    bool Create(Win3D* NewParent, void* Data) override;
    // slot 11 the routine
    void Update() override;
    // slot 13 the branch
    void Draw() override;
    // slot 16 the branch (RET 4)
    void MouseLeave(float const* Ray) override;
    // slot 19 the branch (RET 4)
    bool MouseEvent(FWin3DMouseEvent const* Event) override;
    // slot 45 the routine
    bool Relayout() override;
    // slot 46 the branch -- &+0x40c in tip mode 0, else null.
    FToolTipHoverInfo* GetToolTipHoverInfo() override;

    // (RET 4) -- +0x54e.
    void SetDrawColumnLines(bool bDraw);
    // (RET 0x10) -- +0x552 = Left, +0x553 = Right, +0x550 = Top, +0x551 = Bottom.
    void SetBorders(bool bLeft, bool bRight, bool bTop, bool bBottom);
    // (RET 4) -- +0x558 = Rows when Rows < row count.
    void SetHeaderRows(unsigned Rows);
    // (RET 4) -- +0x554.
    void SetRowHighlights(bool bQuads);
    // (RET 8) -- empty the cell's source list and dirty the layout.
    bool ClearCell(unsigned Row, unsigned Column);
    // (RET 0xc)
    bool GetCell(unsigned Row, unsigned Column, FUiFastTableCell** OutCell);
    // (RET 8) -- dirty the layout when (Row, Column) is inside the grid.
    void InvalidateCell(unsigned Row, unsigned Column);
    // (RET 0xc) -- +0x18 of the cell.
    void SetCellSpanToEnd(unsigned Row, unsigned Column, bool bSpan);
    // (RET 8) -- move column edge Index (1..count-1) to X, clamped between its
    // neighbours; false for edge 0 or an index out of range.
    bool SetColumnEdge(unsigned Index, float X);
    // (RET 8) -- the same for row edge Index and Y.
    bool SetRowEdge(unsigned Index, float Y);
    // (RET 4) -- &Rows[Row], or null out of range.
    FUiFastTableRow* GetRow(unsigned Row);
    // (RET 4)
    void GetEdges(FUiFastTableEdges* Out);
    // (RET 4) -- FDUMP notice and no change when the shape or the first column
    // edge differs.
    void SetEdges(FUiFastTableEdges const* In);

    void ClearSelection();
    // (RET 4)
    bool SelectRow(unsigned Row);
    // (RET 4)
    bool GetSelectedRow(unsigned* OutRow);
    // (RET 4) -- +0x4f0.
    void SetFrameColor(unsigned long const* Rgba);
    // (RET 4) -- only while the current distance is not negative.
    void SetGrabDistance(float Distance);
    // (RET 8) -- +0x501 = bRows, +0x500 = bColumns.
    void SetResizable(bool bRows, bool bColumns);
    // (RET 8) -- +0x570 = X, +0x574 = Y.
    void SetCellMargins(int X, int Y);
    // (RET 4) -- +0x54c.
    void SetLayoutFlag(bool bFlag);
    // (RET 4) -- 0 none (the base record), 1 per column, 2 per row, 3 per cell.
    void SetTipMode(int Mode);
    // (RET 4) -- mode 2 only.
    FToolTipHoverInfo* GetRowTip(unsigned Row);
    // (RET 8) -- mode 3 only.
    FToolTipHoverInfo* GetCellTip(unsigned Row, unsigned Column);

    float Width;                            // +0x488
    float Height;                           // +0x48c
    std::vector<FUiFastTableCell> Cells;    // +0x490
    std::vector<float> RowEdges;            // +0x4a0
    std::vector<float> ColEdges;            // +0x4b0
    std::vector<FUiFastTableRow> Rows;      // +0x4c0
    unsigned char bSelected;                // +0x4d8
    unsigned SelectedRow;                   // +0x4dc
    unsigned HoverColumn;                   // +0x4e0
    unsigned HoverRow;                      // +0x4e4
    unsigned char bHovered;                 // +0x4e8
    unsigned HoveredRow;                    // +0x4ec
    unsigned long FrameRgba;                // +0x4f0
    unsigned long SelectedRgba;             // +0x4f4
    unsigned long HoveredRgba;              // +0x4f8
    float GrabDistance;                     // +0x4fc
    unsigned char bColumnsResizable;        // +0x500
    unsigned char bRowsResizable;           // +0x501
    float InverseTransform[12];             // +0x504
    unsigned char bDragArmed;               // +0x540
    unsigned char bDragging;                // +0x541
    unsigned char bDragColumn;              // +0x542
    unsigned DragEdge;                      // +0x544
    float DragOffset;                       // +0x548
    unsigned char bLayoutFlag;              // +0x54c
    unsigned char bDrawRowLines;            // +0x54d
    unsigned char bDrawColumnLines;         // +0x54e
    unsigned char bDrawBorder;              // +0x54f
    unsigned char bBorderTop;               // +0x550
    unsigned char bBorderBottom;            // +0x551
    unsigned char bBorderLeft;              // +0x552
    unsigned char bBorderRight;             // +0x553
    unsigned char bRowHighlights;           // +0x554
    unsigned HeaderRows;                    // +0x558
    int TipMode;                            // +0x55c
    std::vector<FUiFastTableTip> Tips;      // +0x560
    int CellMarginX;                        // +0x570
    int CellMarginY;                        // +0x574

protected:
    // The mouse ray scaled onto the table's z plane, then into table space through +0x504
    // (0x0058e5d1..0x0058e61e, 0x0058e69f..0x0058e6ec, 0x0058f3ec..0x0058f448).
    FVector RayToLocal(float const* Ray) const;
    // Draw's row highlight quad (0x0058ebf5..0x0058ecdf / 0x0058ecf2..0x0058eddc).
    void DrawRowQuad(unsigned long const* Rgba, unsigned Row) const;

private:
    UiFastTable(UiFastTable const&) = delete;
    UiFastTable& operator=(UiFastTable const&) = delete;
};

// +0x578: called (cdecl) with the context, ftol(scrollbar value) and the visible row count
// (rows - fixed rows) whenever the scrollbar moves (Slot23) and once when it is installed
// (the routine). The return value is not read.
typedef void (*FUiScrollTableCallback)(void* Context, int FirstItem, unsigned VisibleRows);

class UiScrollTable : public UiFastTable
{
public:
    // 's inline construction after the routine: +0x578, +0x584, +0x57c, +0x580,
    // +0x588 = 0.
    UiScrollTable();
    // nothing of its own; the UIFastTable teardown.
    ~UiScrollTable() override;

    // slot 9 the routine (RET 8)
    bool Create(Win3D* NewParent, void* Data) override;
    // slot 23 the branch (RET 0xc)
    bool Slot23(unsigned Msg, unsigned A, unsigned B) override;

    // (RET 8) -- the scrollbar's range for ItemCount items with FixedRows rows
    // not scrolled; hidden (value 0) when everything fits.
    void SetItemCount(unsigned ItemCount, unsigned FixedRows);
    // ftol(scrollbar value).
    int GetFirstItem();
    // (RET 8) -- install the callback and call it once when it is not null.
    void SetCallback(FUiScrollTableCallback Callback, void* Context);
    // (RET 4) -- +0x3fc, then re-place the scrollbar beside the table.
    void SetScrollBarGap(float Gap);

    FUiScrollTableCallback Callback;    // +0x578
    unsigned ItemCount;                 // +0x57c
    unsigned FixedRows;                 // +0x580
    void* CallbackContext;              // +0x584
    UiScroll* ScrollBar;                // +0x588

private:
    UiScrollTable(UiScrollTable const&) = delete;
    UiScrollTable& operator=(UiScrollTable const&) = delete;
};
