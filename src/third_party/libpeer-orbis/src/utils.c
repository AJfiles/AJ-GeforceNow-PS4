#include "utils.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "mbedtls/md.h"
#ifdef __ORBIS__
extern int ps4_secure_random(void* buffer, size_t size);
#endif

int utils_random_string(char* s, const int len) {
  if (s == NULL || len < 0) return -1;

  static const char alphanum[] =
      "0123456789"
      "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
      "abcdefghijklmnopqrstuvwxyz";

#ifdef __ORBIS__
  int written = 0;
  while (written < len) {
    uint8_t random_bytes[32];
    const int remaining = len - written;
    const size_t count = remaining < (int)sizeof(random_bytes) ? (size_t)remaining : sizeof(random_bytes);
    if (ps4_secure_random(random_bytes, count) != 0) {
      memset(s, 0, (size_t)len + 1);
      return -1;
    }
    for (size_t j = 0; j < count && written < len; ++j) {
      /* 248 is the largest multiple of 62 below 256; reject to avoid bias. */
      if (random_bytes[j] >= 248) continue;
      s[written++] = alphanum[random_bytes[j] % 62];
    }
  }
#else
  int i;
  srand(time(NULL));
  for (i = 0; i < len; ++i) {
    s[i] = alphanum[rand() % (sizeof(alphanum) - 1)];
  }
#endif

  s[len] = '\0';
  return 0;
}

void utils_get_hmac_sha1(const char* input, size_t input_len, const char* key, size_t key_len, unsigned char* output) {
  mbedtls_md_context_t ctx;
  mbedtls_md_type_t md_type = MBEDTLS_MD_SHA1;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(md_type), 1);
  mbedtls_md_hmac_starts(&ctx, (const unsigned char*)key, key_len);
  mbedtls_md_hmac_update(&ctx, (const unsigned char*)input, input_len);
  mbedtls_md_hmac_finish(&ctx, output);
  mbedtls_md_free(&ctx);
}

void utils_get_md5(const char* input, size_t input_len, unsigned char* output) {
  mbedtls_md_context_t ctx;
  mbedtls_md_type_t md_type = MBEDTLS_MD_MD5;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(md_type), 1);
  mbedtls_md_starts(&ctx);
  mbedtls_md_update(&ctx, (const unsigned char*)input, input_len);
  mbedtls_md_finish(&ctx, output);
  mbedtls_md_free(&ctx);
}
