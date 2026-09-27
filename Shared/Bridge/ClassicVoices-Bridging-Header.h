// Exposes the C bridges to Swift in both targets: cv_bridge (the Sam / Mike / Mary engine), cv4n_bridge
// (Microsoft's SAPI 4 engine decompiled to C, Engine/sapi4; cv4_bridge, the same engine in the x86 interpreter, is kept
// for the tests only), cvt_bridge (L&H TruVoice on OpenTV), cva_bridge (Microsoft Anna) and cvo_bridge (Microsoft David,
// Zira and Mark) and cvn_bridge (the neural voices on Microsoft's embedded Speech SDK).
#include "cv_bridge.h"
#include "cv4n_bridge.h"
#include "cvt_bridge.h"
#include "cva_bridge.h"
#include "cvo_bridge.h"
#include "cvn_bridge.h"
