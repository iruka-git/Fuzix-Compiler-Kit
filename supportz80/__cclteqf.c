#include "libfp.h"

unsigned _cclteqf(uint32_t a2, uint32_t a1)
{
    if (a1 & a2 & 0x80000000UL) {
        if (a2 > a1)
            return 0;
        return 1;
    }
    if (a1 > a2)
        return 0;
    return 1;
}
