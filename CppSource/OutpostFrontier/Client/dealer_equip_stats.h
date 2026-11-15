// Author: Giperion.

#pragma once

// — Equipment/Ship Dealer item-stat / tip text builder.
// Full branch table: implementation notes

#include "common_text.h"

// OutRdl receives one AppendTextMaybeXmlRdl (the routine) per finished line.
// bEmitLabels / bEmitValues match baseline param_3 / param_4 (caller splits label vs value columns).
void BuildDealerEquipmentStatText(RenderDisplayList& OutRdl, unsigned ArchId, bool bEmitLabels,
                                  bool bEmitValues);
