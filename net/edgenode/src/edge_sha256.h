#pragma once

#include <stddef.h>

#include <mbedtls/sha256.h>
#include <mbedtls/version.h>

static inline int edge_sha256_starts(mbedtls_sha256_context *context, int is224) {
#if MBEDTLS_VERSION_NUMBER < 0x03000000
    return mbedtls_sha256_starts_ret(context, is224);
#else
    return mbedtls_sha256_starts(context, is224);
#endif
}

static inline int edge_sha256_update(mbedtls_sha256_context *context,
                                     const unsigned char *input, size_t size) {
#if MBEDTLS_VERSION_NUMBER < 0x03000000
    return mbedtls_sha256_update_ret(context, input, size);
#else
    return mbedtls_sha256_update(context, input, size);
#endif
}

static inline int edge_sha256_finish(mbedtls_sha256_context *context,
                                     unsigned char output[32]) {
#if MBEDTLS_VERSION_NUMBER < 0x03000000
    return mbedtls_sha256_finish_ret(context, output);
#else
    return mbedtls_sha256_finish(context, output);
#endif
}

static inline int edge_sha256(const unsigned char *input, size_t size,
                              unsigned char output[32], int is224) {
#if MBEDTLS_VERSION_NUMBER < 0x03000000
    return mbedtls_sha256_ret(input, size, output, is224);
#else
    return mbedtls_sha256(input, size, output, is224);
#endif
}
