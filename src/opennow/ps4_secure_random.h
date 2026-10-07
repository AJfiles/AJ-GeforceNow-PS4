#ifndef OPENNOW_PS4_SECURE_RANDOM_H
#define OPENNOW_PS4_SECURE_RANDOM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns 0 only when Orbis' libSceRandom filled the entire buffer. */
int ps4_secure_random(void* buffer, size_t size);

#ifdef __cplusplus
}
#endif

#endif
