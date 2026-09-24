// Umbrella include for re4_boot's generated/manual stub .cpp files (docs/port-boot.md). Rather
// than tracking down the exact header each individual undefined symbol's type comes from, this
// pulls in every header touched by the undefined-symbol survey once, in one place -- these stub
// TUs have no matching/byte-identity concerns (src/port/ is outside the original build, confirmed
// against configure.py), so a broad include list here costs nothing but a slightly slower stub
// build.
#ifndef RE4_PORT_STUB_COMMON_H
#define RE4_PORT_STUB_COMMON_H
#ifdef TARGET_PC

#include "types.h"
#include "vec.h"

#include "act_btn.h"
#include "at_mod.h"
#include "block.h"
#include "cam_qfps.h"
#include "camera.h"
#include "card.h"
#include "cloth.h"
#include "datactrl.h"
#include "db_cam.h"
#include "dolphin/ax.h"
#include "dolphin/os/OSError.h"
#include "dolphin/os/OSFont.h"
#include "dolphin/os/OSSemaphore.h"
#include "dolphin/os/OSThread.h"
#include "dolphin/vi/vitypes.h"
#include "em.h"
#include "item.h"
#include "sce_at.h"
#include "em2c.h"
#include "esp.h"
#include "espgen.h"
#include "game.h"
#include "gx.h"
#include "joy.h"
#include "main.h"
#include "mercenaries.h"
#include "model.h"
#include "mwply.h"
#include "objRobo.h"
#include "player.h"
#include "pl_sub.h"
#include "pl_wep.h"
#include "read.h"
#include "scroll.h"
#include "snd.h"
#include "snd_sdk.h"

#include <cstdio>

#endif // TARGET_PC
#endif // RE4_PORT_STUB_COMMON_H
