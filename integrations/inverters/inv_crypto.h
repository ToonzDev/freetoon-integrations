#ifndef INV_CRYPTO_H
#define INV_CRYPTO_H
#include <stddef.h>
#include <stdint.h>

/* MD5, SHA-1, HMAC-SHA1 and Base64: just what the SolisCloud request
 * signature needs (Content-MD5 + "API keyId:base64(hmac_sha1(...))").
 * Plain C, no OpenSSL, so the binary runs on both Toons as is. */
void inv_md5(const void * data, size_t len, uint8_t out[16]);
void inv_sha1(const void * data, size_t len, uint8_t out[20]);
void inv_hmac_sha1(const void * key, size_t klen, const void * msg, size_t mlen, uint8_t out[20]);
/* Base64 of in[0..n) into out (NUL-terminated); out needs 4*((n+2)/3)+1. */
void inv_b64(const uint8_t * in, size_t n, char * out);
/* SHA-256 (GoodWe SEMS+ X-Signature). */
void inv_sha256(const void * data, size_t len, uint8_t out[32]);

#endif
