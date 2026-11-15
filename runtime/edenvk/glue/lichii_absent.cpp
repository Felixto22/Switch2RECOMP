/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
   Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen. */
/* the Lichii renderer's entry points (runtime/lichiivk/glue/lichii_glue.cpp): libedenvk is Eden's renderer, not Lichii's */
#include "common/common_types.h"
extern "C" {
int g_lichii_on;
extern const int g_lichii_lib = 0;
void lichii_submit(u32, const u64*, u32, u32) {}
void lichii_set_shim(u64) {}
void lichii_syncpt_external(u32) {}
}
