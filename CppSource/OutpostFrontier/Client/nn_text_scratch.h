// Author: Giperion.

#pragma once

// baseline's two shared wide-text scratch buffers and their capacity global. Many text builders
// fill one of them with an IDS string (the routine with the capacity the field), format or
// concatenate into it, and hand it to a control or a tooltip record in the same call:
// NN_Inventory's SetProperty 0x4f (0x00479fec..0x0047a046), the dealer's stat text
// , the HUD status / target panels, NN_MainButton (0x0045af16..) and the rollover
// (0x004ba08b..). Nothing keeps a pointer into them across calls.

// .bss  0x1000 wchar_t (ends at 0x0066fc60)
// .bss  0x1000 wchar_t (ends at 0x00671c60)
// .data int = 0x1000, the count every caller passes for either buffer

extern wchar_t G_NnTextScratchA[0x1000];
extern wchar_t G_NnTextScratchB[0x1000];
extern int G_NnTextScratchCapacity;
