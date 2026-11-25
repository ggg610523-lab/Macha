/*
 * CommonCrypto surface for the clean-room libSystem shim.
 *
 * Darwin's CC_* entry points are implemented with the host OpenSSL EVP/HMAC
 * API. The context objects are caller-allocated Darwin structs; we store an
 * EVP_MD_CTX pointer in the first bytes, which is safe because every Darwin
 * context is larger than a pointer.
 */
#define _GNU_SOURCE
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <stdint.h>
#include <string.h>

typedef uint32_t cc_long;

struct cc_state {
    EVP_MD_CTX *ctx;
    uint32_t magic;
};

static int cc_init(void *p, const EVP_MD *md)
{
    struct cc_state *st = p;
    if (!st)
        return 0;
    st->ctx = EVP_MD_CTX_new();
    if (!st->ctx)
        return 0;
    if (EVP_DigestInit_ex(st->ctx, md, NULL) != 1) {
        EVP_MD_CTX_free(st->ctx);
        st->ctx = NULL;
        return 0;
    }
    st->magic = 0x43434d44u;
    return 1;
}

static int cc_update(void *p, const void *data, size_t len)
{
    struct cc_state *st = p;
    if (!st || !st->ctx)
        return 0;
    return EVP_DigestUpdate(st->ctx, data, len) == 1;
}

static int cc_final(void *p, unsigned char *md)
{
    struct cc_state *st = p;
    unsigned int len = 0;
    int ok;
    if (!st || !st->ctx)
        return 0;
    ok = EVP_DigestFinal_ex(st->ctx, md, &len) == 1;
    EVP_MD_CTX_free(st->ctx);
    st->ctx = NULL;
    return ok;
}

static unsigned char *cc_digest(const void *data, cc_long len, unsigned char *md,
                               const EVP_MD *type)
{
    unsigned int out = 0;
    if (!md)
        return NULL;
    if (EVP_Digest(data, (size_t)len, md, &out, type, NULL) != 1)
        return NULL;
    return md;
}

unsigned char *plsr_CC_MD5(const void *data, cc_long len, unsigned char *md)
    __asm__("_CC_MD5");
unsigned char *plsr_CC_MD5(const void *data, cc_long len, unsigned char *md)
{
    return cc_digest(data, len, md, EVP_md5());
}

unsigned char *plsr_CC_SHA1(const void *data, cc_long len, unsigned char *md)
    __asm__("_CC_SHA1");
unsigned char *plsr_CC_SHA1(const void *data, cc_long len, unsigned char *md)
{
    return cc_digest(data, len, md, EVP_sha1());
}

unsigned char *plsr_CC_SHA224(const void *data, cc_long len, unsigned char *md)
    __asm__("_CC_SHA224");
unsigned char *plsr_CC_SHA224(const void *data, cc_long len, unsigned char *md)
{
    return cc_digest(data, len, md, EVP_sha224());
}

unsigned char *plsr_CC_SHA256(const void *data, cc_long len, unsigned char *md)
    __asm__("_CC_SHA256");
unsigned char *plsr_CC_SHA256(const void *data, cc_long len, unsigned char *md)
{
    return cc_digest(data, len, md, EVP_sha256());
}

unsigned char *plsr_CC_SHA384(const void *data, cc_long len, unsigned char *md)
    __asm__("_CC_SHA384");
unsigned char *plsr_CC_SHA384(const void *data, cc_long len, unsigned char *md)
{
    return cc_digest(data, len, md, EVP_sha384());
}

unsigned char *plsr_CC_SHA512(const void *data, cc_long len, unsigned char *md)
    __asm__("_CC_SHA512");
unsigned char *plsr_CC_SHA512(const void *data, cc_long len, unsigned char *md)
{
    return cc_digest(data, len, md, EVP_sha512());
}

#define CC_CTX_FUNCS(name, md)                                                \
    int plsr_CC_##name##_Init(void *c) __asm__("_CC_" #name "_Init");         \
    int plsr_CC_##name##_Init(void *c) { return cc_init(c, md); }             \
    int plsr_CC_##name##_Update(void *c, const void *d, cc_long n)            \
        __asm__("_CC_" #name "_Update");                                      \
    int plsr_CC_##name##_Update(void *c, const void *d, cc_long n)            \
    { return cc_update(c, d, n); }                                            \
    int plsr_CC_##name##_Final(unsigned char *md, void *c)                    \
        __asm__("_CC_" #name "_Final");                                       \
    int plsr_CC_##name##_Final(unsigned char *md, void *c)                    \
    { return cc_final(c, md); }

CC_CTX_FUNCS(MD5, EVP_md5())
CC_CTX_FUNCS(SHA1, EVP_sha1())
CC_CTX_FUNCS(SHA224, EVP_sha224())
CC_CTX_FUNCS(SHA256, EVP_sha256())
CC_CTX_FUNCS(SHA384, EVP_sha384())
CC_CTX_FUNCS(SHA512, EVP_sha512())

void plsr_CCHmac(int algorithm, const void *key, size_t key_len,
                 const void *data, size_t data_len, void *mac_out)
    __asm__("_CCHmac");
void plsr_CCHmac(int algorithm, const void *key, size_t key_len,
                 const void *data, size_t data_len, void *mac_out)
{
    const EVP_MD *md = EVP_sha256();
    unsigned int out = 0;
    switch (algorithm) {
    case 0: md = EVP_sha1(); break;
    case 1: md = EVP_md5(); break;
    case 2: md = EVP_sha256(); break;
    case 3: md = EVP_sha384(); break;
    case 4: md = EVP_sha512(); break;
    case 5: md = EVP_sha224(); break;
    default: break;
    }
    HMAC(md, key, (int)key_len, data, data_len, mac_out, &out);
}

/* CCCrypt: 0 AES128, 1 AES192, 2 AES256, 3 DES, 4 3DES, 5 CAST, 6 RC4,
 * 7 RC2, 8 Blowfish. options bit 0 = PKCS7, bit 1 = ECB. */
int plsr_CCCrypt(int op, int alg, uint32_t options, const void *key,
                 size_t key_len, const void *iv, const void *in, size_t in_len,
                 void *out, size_t out_available, size_t *out_moved)
    __asm__("_CCCrypt");
int plsr_CCCrypt(int op, int alg, uint32_t options, const void *key,
                 size_t key_len, const void *iv, const void *in, size_t in_len,
                 void *out, size_t out_available, size_t *out_moved)
{
    const EVP_CIPHER *cipher = NULL;
    EVP_CIPHER_CTX *ctx;
    int len = 0, final_len = 0, ok;
    int encrypt = op == 0;
    (void)key_len;
    if (out_moved)
        *out_moved = 0;
    switch (alg) {
    case 0: cipher = (options & 2) ? EVP_aes_128_ecb() : EVP_aes_128_cbc(); break;
    case 1: cipher = (options & 2) ? EVP_aes_192_ecb() : EVP_aes_192_cbc(); break;
    case 2: cipher = (options & 2) ? EVP_aes_256_ecb() : EVP_aes_256_cbc(); break;
    case 3: cipher = (options & 2) ? EVP_des_ecb() : EVP_des_cbc(); break;
    case 4: cipher = (options & 2) ? EVP_des_ede3_ecb() : EVP_des_ede3_cbc(); break;
    case 5: cipher = (options & 2) ? EVP_cast5_ecb() : EVP_cast5_cbc(); break;
    case 6: cipher = EVP_rc4(); break;
    case 7: cipher = (options & 2) ? EVP_rc2_ecb() : EVP_rc2_cbc(); break;
    case 8: cipher = (options & 2) ? EVP_bf_ecb() : EVP_bf_cbc(); break;
    default: return -1;
    }
    if (!cipher || in_len > out_available)
        return -1;
    ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return -1;
    ok = EVP_CipherInit_ex(ctx, cipher, NULL, key, iv, encrypt) == 1;
    if (ok)
        ok = EVP_CIPHER_CTX_set_padding(ctx, (options & 1) ? 1 : 0) == 1;
    if (ok)
        ok = EVP_CipherUpdate(ctx, out, &len, in, (int)in_len) == 1;
    if (ok)
        ok = EVP_CipherFinal_ex(ctx, (unsigned char *)out + len, &final_len) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok)
        return -1;
    if (out_moved)
        *out_moved = (size_t)(len + final_len);
    return 0;
}

int plsr_CCRandomGenerateBytes(void *bytes, size_t count)
    __asm__("_CCRandomGenerateBytes");
int plsr_CCRandomGenerateBytes(void *bytes, size_t count)
{
    return RAND_bytes(bytes, (int)count) == 1 ? 0 : -1;
}

__attribute__((visibility("default"), used))
const char *const plsr_crypto_imports[] = { "/usr/lib/libcrypto.dylib", NULL };
