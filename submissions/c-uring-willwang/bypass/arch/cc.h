#ifndef CHALLENGE_LWIP_ARCH_CC_H
#define CHALLENGE_LWIP_ARCH_CC_H

#include <endian.h>
#include <stdio.h>
#include <stdlib.h>

#define LWIP_PLATFORM_ASSERT(message) do { \
    fprintf(stderr, "lwIP assertion at %s:%d: %s\n", __FILE__, __LINE__, (message)); \
    abort(); \
} while (0)

#endif
