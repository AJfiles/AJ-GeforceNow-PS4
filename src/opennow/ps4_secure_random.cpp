#include "ps4_secure_random.h"

#include <orbis/Random.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>

extern "C" void opennow_log_app_lifecycle_from_c(const char* event, const char* detail);

extern "C" int ps4_secure_random(void* buffer, size_t size) {
    if (size != 0 && buffer == nullptr) {
        errno = EINVAL;
        return -1;
    }

    unsigned char* out = static_cast<unsigned char*>(buffer);
    size_t offset = 0;
    while (offset < size) {
        // The PS4 sceRandom API rejects large requests (observed EINVAL at 128 bytes).
        // Keep each syscall within the 16-byte size verified during app startup.
        const size_t chunk = std::min<size_t>(size - offset, 16);
        const int rc = sceRandomGetRandomNumber(out + offset, chunk);
        if (rc != 0) {
            errno = EIO;
            char detail[96];
            std::snprintf(detail, sizeof(detail), "orbis_rc=%d requested=%zu offset=%zu", rc, chunk, offset);
            opennow_log_app_lifecycle_from_c("PS4_RANDOM_FAILED", detail);
            return rc;
        }
        offset += chunk;
    }
    return 0;
}
