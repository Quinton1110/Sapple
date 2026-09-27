/*
 * The one place the original reaches into its own object by raw offset
 * rather than by member (only reachable with pathological input).
 *
 * Both builds give the object the original's layout -- the offsets are
 * asserted wherever pointers are four bytes wide -- so the arithmetic is
 * exact in both.  A build with wider pointers would have to map the offset
 * through the generated accessor instead.
 */
#include "engine.h"

void Engine_ZeroDwordIfMinus1(Engine *self, uint32_t off32)
{
    int32_t *p = (int32_t *)((uint8_t *)self + off32);

    if (off32 + 4 <= sizeof(Engine) && *p == -1)
        *p = 0;
}
