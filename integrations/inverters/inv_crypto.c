/* MD5 (RFC 1321), SHA-1 (FIPS 180-4), HMAC (RFC 2104), Base64 (RFC 4648). */
#include "inv_crypto.h"
#include <string.h>

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

/* ---------------------------------------------------------------- MD5 */
static const uint32_t md5_k[64] = {
    0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
    0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
    0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
    0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
    0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
    0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
    0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
    0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391 };
static const uint8_t md5_r[64] = {
    7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22, 5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
    4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23, 6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21 };

static void md5_block(uint32_t h[4], const uint8_t * b) {
    uint32_t w[16];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)b[i*4] | (uint32_t)b[i*4+1] << 8 | (uint32_t)b[i*4+2] << 16 | (uint32_t)b[i*4+3] << 24;
    uint32_t a = h[0], bb = h[1], c = h[2], d = h[3];
    for (int i = 0; i < 64; i++) {
        uint32_t f; int g;
        if (i < 16)      { f = (bb & c) | (~bb & d); g = i; }
        else if (i < 32) { f = (d & bb) | (~d & c);  g = (5 * i + 1) % 16; }
        else if (i < 48) { f = bb ^ c ^ d;           g = (3 * i + 5) % 16; }
        else             { f = c ^ (bb | ~d);        g = (7 * i) % 16; }
        uint32_t t = d; d = c; c = bb;
        bb = bb + ROL(a + f + md5_k[i] + w[g], md5_r[i]);
        a = t;
    }
    h[0] += a; h[1] += bb; h[2] += c; h[3] += d;
}

void inv_md5(const void * data, size_t len, uint8_t out[16]) {
    uint32_t h[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
    const uint8_t * p = data;
    size_t n = len;
    for (; n >= 64; n -= 64, p += 64) md5_block(h, p);
    uint8_t tail[128] = {0};
    memcpy(tail, p, n);
    tail[n] = 0x80;
    size_t tl = n < 56 ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) tail[tl - 8 + i] = (uint8_t)(bits >> (8 * i));
    md5_block(h, tail);
    if (tl == 128) md5_block(h, tail + 64);
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) out[i*4+j] = (uint8_t)(h[i] >> (8 * j));
}

/* -------------------------------------------------------------- SHA-1 */
static void sha1_block(uint32_t h[5], const uint8_t * b) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)b[i*4] << 24 | (uint32_t)b[i*4+1] << 16 | (uint32_t)b[i*4+2] << 8 | (uint32_t)b[i*4+3];
    for (int i = 16; i < 80; i++) w[i] = ROL(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    uint32_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (bb & c) | (~bb & d);           k = 0x5a827999; }
        else if (i < 40) { f = bb ^ c ^ d;                     k = 0x6ed9eba1; }
        else if (i < 60) { f = (bb & c) | (bb & d) | (c & d);  k = 0x8f1bbcdc; }
        else             { f = bb ^ c ^ d;                     k = 0xca62c1d6; }
        uint32_t t = ROL(a, 5) + f + e + k + w[i];
        e = d; d = c; c = ROL(bb, 30); bb = a; a = t;
    }
    h[0] += a; h[1] += bb; h[2] += c; h[3] += d; h[4] += e;
}

void inv_sha1(const void * data, size_t len, uint8_t out[20]) {
    uint32_t h[5] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0 };
    const uint8_t * p = data;
    size_t n = len;
    for (; n >= 64; n -= 64, p += 64) sha1_block(h, p);
    uint8_t tail[128] = {0};
    memcpy(tail, p, n);
    tail[n] = 0x80;
    size_t tl = n < 56 ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) tail[tl - 1 - i] = (uint8_t)(bits >> (8 * i));
    sha1_block(h, tail);
    if (tl == 128) sha1_block(h, tail + 64);
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 4; j++) out[i*4+j] = (uint8_t)(h[i] >> (24 - 8 * j));
}

/* --------------------------------------------------------- HMAC-SHA1 */
void inv_hmac_sha1(const void * key, size_t klen, const void * msg, size_t mlen, uint8_t out[20]) {
    uint8_t k[64] = {0}, ipad[64], opad[64], inner[20];
    if (klen > 64) inv_sha1(key, klen, k);
    else memcpy(k, key, klen);
    for (int i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }
    /* inner = sha1(ipad || msg): one buffer keeps this simple; messages are small */
    uint8_t buf[64 + 1024];
    if (mlen > 1024) mlen = 1024;
    memcpy(buf, ipad, 64);
    memcpy(buf + 64, msg, mlen);
    inv_sha1(buf, 64 + mlen, inner);
    memcpy(buf, opad, 64);
    memcpy(buf + 64, inner, 20);
    inv_sha1(buf, 84, out);
}

/* ------------------------------------------------------------- Base64 */
void inv_b64(const uint8_t * in, size_t n, char * out) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)in[i+1] << 8;
        if (i + 2 < n) v |= in[i+2];
        out[o++] = T[(v >> 18) & 63];
        out[o++] = T[(v >> 12) & 63];
        out[o++] = i + 1 < n ? T[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? T[v & 63] : '=';
    }
    out[o] = 0;
}
