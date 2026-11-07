#pragma once

#include <libultraship/libultra.h>

#ifdef __cplusplus
extern "C" {
#endif

s32 Anchor_UseSharedBossPortal(void);
s32 Anchor_GetBossWarpRequest(void);
s32 Anchor_StartBossWarp(s32 story);
s32 Anchor_CommitBossWarp(void);

#ifdef __cplusplus
}
#endif
