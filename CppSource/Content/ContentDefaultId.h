// Author: Giperion.

#pragma once

#include "common_id.h"

// Content.dll "default" static ID. A second header, included after ContentStaticIds.h by the
// baseline translation units that carry one more _initterm thunk writing CreateID("default")
// into a TU-private global (among them TU0-14 06ea1200..06ea58c0, TU26 06eeec30, TU27
// , TU33 06efeae0, TU35 06f041a0, TU38 06f0e200). Internal linkage, so each including
// source file gets its own copy, as in baseline.

static unsigned int const GIdDefault = CreateID("default");
