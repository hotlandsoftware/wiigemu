#include "gemu/sha256.h"
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#define RR(x,n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x,y,z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x,y,z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x)  (RR(x, 2) ^ RR(x,13) ^ RR(x,22))
#define EP1(x)  (RR(x, 6) ^ RR(x,11) ^ RR(x,25))
#define SIG0(x) (RR(x, 7) ^ RR(x,18) ^ ((x) >>  3))
#define SIG1(x) (RR(x,17) ^ RR(x,19) ^ ((x) >> 10))

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static void sha256_transform(uint32_t s[8], const uint8_t b[64]) {
    uint32_t w[64], a, b2, c, d, e, f, g, h, t1, t2;
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)b[i*4]<<24) | ((uint32_t)b[i*4+1]<<16)
             | ((uint32_t)b[i*4+2]<<8) | b[i*4+3];
    for (int i = 16; i < 64; i++)
        w[i] = SIG1(w[i-2]) + w[i-7] + SIG0(w[i-15]) + w[i-16];
    a=s[0]; b2=s[1]; c=s[2]; d=s[3]; e=s[4]; f=s[5]; g=s[6]; h=s[7];
    for (int i = 0; i < 64; i++) {
        t1 = h + EP1(e) + CH(e,f,g) + K[i] + w[i];
        t2 = EP0(a) + MAJ(a,b2,c);
        h=g; g=f; f=e; e=d+t1; d=c; c=b2; b2=a; a=t1+t2;
    }
    s[0]+=a; s[1]+=b2; s[2]+=c; s[3]+=d;
    s[4]+=e; s[5]+=f;  s[6]+=g; s[7]+=h;
}

void gemu_sha256_init(GemuSha256Ctx *ctx) {
    ctx->state[0]=0x6a09e667; ctx->state[1]=0xbb67ae85;
    ctx->state[2]=0x3c6ef372; ctx->state[3]=0xa54ff53a;
    ctx->state[4]=0x510e527f; ctx->state[5]=0x9b05688c;
    ctx->state[6]=0x1f83d9ab; ctx->state[7]=0x5be0cd19;
    ctx->count = 0;
    memset(ctx->buf, 0, 64);
}

void gemu_sha256_update(GemuSha256Ctx *ctx, const uint8_t *data, size_t len) {
    size_t used = (size_t)(ctx->count & 63);
    ctx->count += (uint64_t)len;
    if (used) {
        size_t room = 64 - used;
        if (len < room) { memcpy(ctx->buf + used, data, len); return; }
        memcpy(ctx->buf + used, data, room);
        sha256_transform(ctx->state, ctx->buf);
        data += room; len -= room;
    }
    while (len >= 64) {
        sha256_transform(ctx->state, data);
        data += 64; len -= 64;
    }
    memcpy(ctx->buf, data, len);
}

void gemu_sha256_final(GemuSha256Ctx *ctx, uint8_t digest[GEMU_SHA256_DIGEST_LEN]) {
    size_t used = (size_t)(ctx->count & 63);
    ctx->buf[used++] = 0x80;
    if (used > 56) {
        memset(ctx->buf + used, 0, 64 - used);
        sha256_transform(ctx->state, ctx->buf);
        used = 0;
    }
    memset(ctx->buf + used, 0, 56 - used);
    uint64_t bits = ctx->count * 8;
    ctx->buf[56] = (uint8_t)(bits >> 56); ctx->buf[57] = (uint8_t)(bits >> 48);
    ctx->buf[58] = (uint8_t)(bits >> 40); ctx->buf[59] = (uint8_t)(bits >> 32);
    ctx->buf[60] = (uint8_t)(bits >> 24); ctx->buf[61] = (uint8_t)(bits >> 16);
    ctx->buf[62] = (uint8_t)(bits >>  8); ctx->buf[63] = (uint8_t)(bits);
    sha256_transform(ctx->state, ctx->buf);
    for (int i = 0; i < 8; i++) {
        digest[i*4+0] = (uint8_t)(ctx->state[i] >> 24);
        digest[i*4+1] = (uint8_t)(ctx->state[i] >> 16);
        digest[i*4+2] = (uint8_t)(ctx->state[i] >>  8);
        digest[i*4+3] = (uint8_t)(ctx->state[i]      );
    }
}

bool gemu_sha256_file(const char *path, uint8_t digest[GEMU_SHA256_DIGEST_LEN]) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    GemuSha256Ctx ctx;
    gemu_sha256_init(&ctx);
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        gemu_sha256_update(&ctx, buf, n);
    bool ok = !ferror(f);
    fclose(f);
    if (ok) gemu_sha256_final(&ctx, digest);
    return ok;
}

void gemu_sha256_hex(const uint8_t digest[GEMU_SHA256_DIGEST_LEN], char hex[65]) {
    static const char h[] = "0123456789abcdef";
    for (int i = 0; i < GEMU_SHA256_DIGEST_LEN; i++) {
        hex[i*2+0] = h[digest[i] >> 4];
        hex[i*2+1] = h[digest[i] & 0xf];
    }
    hex[64] = '\0';
}
