// SPDX-License-Identifier: GPL-3.0-or-later
//
// Private seams between PyxisService.cpp and PyxisCall.cpp (same
// library, separate translation units). Not for use outside
// lib/pyxis_core.
#pragma once

#include "PyxisService.h"

namespace LXMF { class LXMRouter; }

// Post into the service's event queue (PyxisService.cpp's post_event).
// aspect: 0 = lxmf.delivery, 1 = lxst.telephony (ANNOUNCE events only).
void pyxis_internal_post_event(PyxisEvent::Kind kind, const char* hash_hex,
                               const char* name, const char* text,
                               uint8_t aspect = 0);
// The LXMF router (owns our identity for link.identify()).
LXMF::LXMRouter* pyxis_internal_router();
