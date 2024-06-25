#include "util-rand.h"
#include <string.h>

typedef struct  {
    unsigned char buf[64];
    uint32_t state[16];
    size_t partial;
} myrand_t;

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#define ROTL32(number, count) (((number) << (count)) | ((number) >> (32 - (count))))
#define READ32LE(p) \
  ((((uint32_t)(p)[0])      ) | \
   (((uint32_t)(p)[1]) <<  8) | \
   (((uint32_t)(p)[2]) << 16) | \
   (((uint32_t)(p)[3]) << 24))
#define QUARTERROUND(x, a, b, c, d) \
  x[a] += x[b]; x[d] = ROTL32(x[d] ^ x[a], 16); \
  x[c] += x[d]; x[b] = ROTL32(x[b] ^ x[c], 12); \
  x[a] += x[b]; x[d] = ROTL32(x[d] ^ x[a],  8); \
  x[c] += x[d]; x[b] = ROTL32(x[b] ^ x[c],  7)

static void 
chacha20_cryptomagic(unsigned char keystream[64], const uint32_t state[16])
{
    uint32_t x[16];
    size_t i;

    memcpy(x, state, sizeof(x[0]) * 16);
    for (i=0; i<10; i++) {
        QUARTERROUND(x, 0, 4, 8,12);
        QUARTERROUND(x, 1, 5, 9,13);
        QUARTERROUND(x, 2, 6,10,14);
        QUARTERROUND(x, 3, 7,11,15);
        QUARTERROUND(x, 0, 5,10,15);
        QUARTERROUND(x, 1, 6,11,12);
        QUARTERROUND(x, 2, 7, 8,13);
        QUARTERROUND(x, 3, 4, 9,14);
    }
    for (i=0; i<16; i++)
        x[i] += state[i];
    for (i=0; i<16; i++) {
        keystream[i * 4 + 0] = (unsigned char)(x[i] >> 0);
        keystream[i * 4 + 1] = (unsigned char)(x[i] >> 8);
        keystream[i * 4 + 2] = (unsigned char)(x[i] >>16);
        keystream[i * 4 + 3] = (unsigned char)(x[i] >>24);
    }
}

static void 
chacha20_init(myrand_t *ctx, const unsigned char key[32], const unsigned char nonce[8])
{
    ctx->state[0] = 0x61707865;
    ctx->state[1] = 0x3320646e;
    ctx->state[2] = 0x79622d32;
    ctx->state[3] = 0x6b206574;
    ctx->state[4] = READ32LE(key + 0);
    ctx->state[5] = READ32LE(key + 4);
    ctx->state[6] = READ32LE(key + 8);
    ctx->state[7] = READ32LE(key + 12);
    ctx->state[8] = READ32LE(key + 16);
    ctx->state[9] = READ32LE(key + 20);
    ctx->state[10] = READ32LE(key + 24);
    ctx->state[11] = READ32LE(key + 28);
    ctx->state[12] = 0;
    ctx->state[13] = 0;
    ctx->state[14] = READ32LE(nonce + 0);
    ctx->state[15] = READ32LE(nonce + 4);
    ctx->partial = 0;
}


/* For securely wiping memory, prevents compilers from removing this
 * function due to optimizations. */
typedef void* (*memset_t)(void*, int, size_t);
static volatile memset_t secure_memset = memset;

/* The standard MIN()/MAX() macro */
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

/**
 * Rotate right a 64-bit number by 'count' bits.
 */
#define ROTR64(number, count) \
    (((number) >> (count)) | ((number) << (64 - (count))))

 /**
  * Read a buffer of 8 bytes into the  bit-endian format to form
  * a 64-bit integer.
  */
static uint64_t
READ64BE(const unsigned char* p)
{
    return (((uint64_t)((p)[0] & 0xFF)) << 56)
        | (((uint64_t)((p)[1] & 0xFF)) << 48)
        | (((uint64_t)((p)[2] & 0xFF)) << 40)
        | (((uint64_t)((p)[3] & 0xFF)) << 32)
        | (((uint64_t)((p)[4] & 0xFF)) << 24)
        | (((uint64_t)((p)[5] & 0xFF)) << 16)
        | (((uint64_t)((p)[6] & 0xFF)) << 8)
        | (((uint64_t)((p)[7] & 0xFF)) << 0);
}

/**
 * Convert an internal 64-bit integer into an array of 8 bytes
 * in big-endian format.
 */
static void
WRITE64BE(uint64_t x, unsigned char* p)
{
    p[0] = (unsigned char)(((x) >> 56) & 0xFF);
    p[1] = (unsigned char)(((x) >> 48) & 0xFF);
    p[2] = (unsigned char)(((x) >> 40) & 0xFF);
    p[3] = (unsigned char)(((x) >> 32) & 0xFF);
    p[4] = (unsigned char)(((x) >> 24) & 0xFF);
    p[5] = (unsigned char)(((x) >> 16) & 0xFF);
    p[6] = (unsigned char)(((x) >> 8) & 0xFF);
    p[7] = (unsigned char)(((x) >> 0) & 0xFF);
}

/* Round constants, usedin the macro "ROUND" */
static const uint64_t K[80] = { 0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL,
    0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL, 0x3956c25bf348b538ULL,
    0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL,
    0x550c7dc3d5ffb4e2ULL, 0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL,
    0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL, 0xe49b69c19ef14ad2ULL,
    0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL,
    0x76f988da831153b5ULL, 0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL,
    0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL, 0xc6e00bf33da88fc2ULL,
    0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL,
    0x53380d139d95b3dfULL, 0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL,
    0x81c2c92e47edaee6ULL, 0x92722c851482353bULL, 0xa2bfe8a14cf10364ULL,
    0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL,
    0x106aa07032bbd1b8ULL, 0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL,
    0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL, 0x391c0cb3c5c95a63ULL,
    0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL,
    0x8cc702081a6439ecULL, 0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL,
    0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL, 0xca273eceea26619cULL,
    0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL,
    0x1b710b35131c471bULL, 0x28db77f523047d84ULL, 0x32caab7b40c72493ULL,
    0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL, 0x4cc5d4becb3e42b6ULL,
    0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL };

#define BLOCK_SIZE 128

#define Ch(x, y, z) (z ^ (x & (y ^ z)))
#define Maj(x, y, z) (((x | y) & z) | (x & y))
#define Sigma0(x) (ROTR64(x, 28) ^ ROTR64(x, 34) ^ ROTR64(x, 39))
#define Sigma1(x) (ROTR64(x, 14) ^ ROTR64(x, 18) ^ ROTR64(x, 41))
#define Gamma0(x) (ROTR64(x, 1) ^ ROTR64(x, 8) ^ (x >> 7))
#define Gamma1(x) (ROTR64(x, 19) ^ ROTR64(x, 61) ^ (x >> 6))

#define ROUND(t0, t1, a, b, c, d, e, f, g, h, i)    \
    t0 = h + Sigma1(e) + Ch(e, f, g) + K[i] + W[i]; \
    t1 = Sigma0(a) + Maj(a, b, c);                  \
    d += t0;                                        \
    h = t0 + t1;

/**
 * This structure holds the 'state' or 'context'. To hash data, this context
 * is first initialized, then multiple updates are done with sequential
 * chunks of data, then at end the 'finalize()' function is called.
 */
typedef struct util_sha512_t {
    uint8_t buf[128];
    uint64_t state[8];
    uint64_t length;
    size_t partial;
} util_sha512_t;

/*
 * This is where all the crpytography happens. It's a typical
 * crypto algorithm that shuffles around the bits and adds
 * them together in ways that can't easily be reversed.
 * This is often called a "compress" function because it logically
 * contains all the information that's added to it. Even after
 * adding terabytes of data, any change in any of the bits will
 * cause the result to be different. Of course it's not real compression,
 * as it can't be uncompressed.
 */
static void
sha512_cryptomagic(util_sha512_t* ctx, const unsigned char* buf)
{
    uint64_t S[8];
    uint64_t W[80];
    uint64_t t0;
    uint64_t t1;
    unsigned i;

    /* Make temporary copy of the state */
    memcpy(S, ctx->state, 8 * sizeof(S[0]));

    /* Copy the plaintext block into a series of 64-bit integers W[0..15],
     * doing a big-endian translation */
    for (i = 0; i < 16; i++) {
        W[i] = READ64BE(buf + (8 * i));
    }

    /* Fill in the remainder of W, W[16..79] */
    for (i = 16; i < 80; i++) {
        W[i] = Gamma1(W[i - 2]) + W[i - 7] + Gamma0(W[i - 15]) + W[i - 16];
    }

    /* Do the compress function */
    for (i = 0; i < 80; i += 8) {
        ROUND(t0, t1, S[0], S[1], S[2], S[3], S[4], S[5], S[6], S[7], i + 0);
        ROUND(t0, t1, S[7], S[0], S[1], S[2], S[3], S[4], S[5], S[6], i + 1);
        ROUND(t0, t1, S[6], S[7], S[0], S[1], S[2], S[3], S[4], S[5], i + 2);
        ROUND(t0, t1, S[5], S[6], S[7], S[0], S[1], S[2], S[3], S[4], i + 3);
        ROUND(t0, t1, S[4], S[5], S[6], S[7], S[0], S[1], S[2], S[3], i + 4);
        ROUND(t0, t1, S[3], S[4], S[5], S[6], S[7], S[0], S[1], S[2], i + 5);
        ROUND(t0, t1, S[2], S[3], S[4], S[5], S[6], S[7], S[0], S[1], i + 6);
        ROUND(t0, t1, S[1], S[2], S[3], S[4], S[5], S[6], S[7], S[0], i + 7);
    }

    /* Do the feedbck step */
    for (i = 0; i < 8; i++) {
        ctx->state[i] += S[i];
    }
}

static void
util_sha512_init(util_sha512_t* ctx)
{
    /* Wipe out whatever might've already existed in this structure */
    memset(ctx, 0, sizeof(*ctx));

    /* Start with a hard-coded "initialization vector" (IV). Because we
     * are always worried about the algorithm creators choosing constants
     * that are a secret backdoor, these constants were chosen to be
     * the fractional portion of the square-roots of the first primes. */
    ctx->state[0] = 0x6a09e667f3bcc908ULL; /* frac(sqrt(2)) */
    ctx->state[1] = 0xbb67ae8584caa73bULL; /* frac(sqrt(3)) */
    ctx->state[2] = 0x3c6ef372fe94f82bULL; /* frac(sqrt(5)) */
    ctx->state[3] = 0xa54ff53a5f1d36f1ULL; /* frac(sqrt(7)) */
    ctx->state[4] = 0x510e527fade682d1ULL; /* frac(sqrt(11)) */
    ctx->state[5] = 0x9b05688c2b3e6c1fULL; /* frac(sqrt(13)) */
    ctx->state[6] = 0x1f83d9abfb41bd6bULL; /* frac(sqrt(17)) */
    ctx->state[7] = 0x5be0cd19137e2179ULL; /* frac(sqrt(19)) */
}

/*
 * This is called iteratively. If the input data isn't aligned on an
 * even block, we must buffer the partial data until either more data
 * is added that will complete the block, or until the 'final()'
 * function is called that will pad the final block.
 */
static void
util_sha512_update(util_sha512_t* ctx, const void* vbuf, size_t length)
{
    const unsigned char* buf = vbuf;
    size_t offset = 0;

    /* Handle any remaining data left over from a previous call */
    if (ctx->partial) {
        size_t n = MIN(length, (BLOCK_SIZE - ctx->partial));
        memcpy(ctx->buf + ctx->partial, buf, n);
        ctx->partial += n;
        offset += n;

        /* If we've filled up a block, then process it, otherwise
         * return without doing any more processing. */
        if (ctx->partial == BLOCK_SIZE) {
            sha512_cryptomagic(ctx, ctx->buf);
            ctx->length += 8 * BLOCK_SIZE;
            ctx->partial = 0;
        }
    }

    /*
     * Process full blocks. This is where this code spends 99% of its time
     */
    while (length - offset >= BLOCK_SIZE) {
        sha512_cryptomagic(ctx, buf + offset);
        ctx->length += BLOCK_SIZE * 8;
        offset += BLOCK_SIZE;
    }

    /* If the last chunk of data isn't a complete block,
     * then buffer it, to be processed in future calls
     * to this function, or during the finalize function */
    if (length - offset) {
        size_t n = MIN(length - offset, (BLOCK_SIZE - ctx->partial));
        memcpy(ctx->buf + ctx->partial, buf + offset, n);
        ctx->partial += n;
    }
}

static void
util_sha512_final(
    util_sha512_t* ctx, unsigned char* digest, size_t digest_length)
{
    size_t i;

    /* Increase the length of the message */
    ctx->length += ctx->partial * 8ULL;

    /* Append the '1' bit */
    ctx->buf[ctx->partial++] = 0x80;

    /* If the length is currently above 112 bytes we append zeros
     * then compress.  Then we can fall back to padding zeros and length
     * encoding like normal. */
    if (ctx->partial > 112) {
        while (ctx->partial < 128) {
            ctx->buf[ctx->partial++] = (uint8_t)0;
        }
        sha512_cryptomagic(ctx, ctx->buf);
        ctx->partial = 0;
    }

    /* Pad up to 120 bytes of zeroes */
    while (ctx->partial < 120) {
        ctx->buf[ctx->partial++] = (uint8_t)0;
    }

    /* Store length */
    WRITE64BE(ctx->length, ctx->buf + 120);
    sha512_cryptomagic(ctx, ctx->buf);

    /* Copy output */
    for (i = 0; i < 64 && i < digest_length; i += 8) {
        WRITE64BE(ctx->state[i / 8], digest + i);
    }

    secure_memset(ctx, 0, sizeof(*ctx));
}

static void
util_sha512(
    const void* buf, size_t length, unsigned char* digest, size_t digest_length)
{
    util_sha512_t ctx;
    util_sha512_init(&ctx);
    util_sha512_update(&ctx, buf, length);
    util_sha512_final(&ctx, digest, digest_length);
}

static unsigned
TEST(const char* buf, size_t length, size_t repeat, unsigned long long x0,
    unsigned long long x1, unsigned long long x2, unsigned long long x3,
    unsigned long long x4, unsigned long long x5, unsigned long long x6,
    unsigned long long x7)
{
    unsigned count = 0;
    unsigned char digest[64];
    unsigned long long x;
    util_sha512_t ctx;
    size_t i;

    util_sha512_init(&ctx);
    for (i = 0; i < repeat; i++)
        util_sha512_update(&ctx, buf, length);
    util_sha512_final(&ctx, digest, 64);

    /* for (i=0; i<64; i += 8) {
        fprintf(stderr, "0x%016llxULL, ", READ64BE(*digest + i));
    }
    fprintf(stderr, "\n");*/
    x = READ64BE(digest + 0);
    count += (x == x0) ? 0 : 1;
    x = READ64BE(digest + 8);
    count += (x == x1) ? 0 : 1;
    x = READ64BE(digest + 16);
    count += (x == x2) ? 0 : 1;
    x = READ64BE(digest + 24);
    count += (x == x3) ? 0 : 1;
    x = READ64BE(digest + 32);
    count += (x == x4) ? 0 : 1;
    x = READ64BE(digest + 40);
    count += (x == x5) ? 0 : 1;
    x = READ64BE(digest + 48);
    count += (x == x6) ? 0 : 1;
    x = READ64BE(digest + 56);
    count += (x == x7) ? 0 : 1;

    return count;
}

int
util_sha512_selftest(void)
{
    unsigned count = 0;

    /* First, test the test. This forces a deliberate failure to
     * make sure it'll return the correct *digest. */
    count += !TEST("abc", 3, 2, 1ULL, 1ULL, 1ULL, 1ULL, 1ULL, 1ULL, 1ULL, 1ULL);

    /* Test the empty string of no input */
    count += TEST("", 0, 1, 0xcf83e1357eefb8bdULL, 0xf1542850d66d8007ULL,
        0xd620e4050b5715dcULL, 0x83f4a921d36ce9ceULL, 0x47d0d13c5d85f2b0ULL,
        0xff8318d2877eec2fULL, 0x63b931bd47417a81ULL, 0xa538327af927da3eULL);
    count += TEST("abc", 3, 1, 0xddaf35a193617abaULL, 0xcc417349ae204131ULL,
        0x12e6fa4e89a97ea2ULL, 0x0a9eeee64b55d39aULL, 0x2192992a274fc1a8ULL,
        0x36ba3c23a3feebbdULL, 0x454d4423643ce80eULL, 0x2a9ac94fa54ca49fULL);

    count += TEST("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
        56, 1, 0x204a8fc6dda82f0aULL, 0x0ced7beb8e08a416ULL,
        0x57c16ef468b228a8ULL, 0x279be331a703c335ULL, 0x96fd15c13b1b07f9ULL,
        0xaa1d3bea57789ca0ULL, 0x31ad85c7a71dd703ULL, 0x54ec631238ca3445ULL);

    count += TEST("a", 1, 1000000, 0xe718483d0ce76964ULL, 0x4e2e42c7bc15b463ULL,
        0x8e1f98b13b204428ULL, 0x5632a803afa973ebULL, 0xde0ff244877ea60aULL,
        0x4cb0432ce577c31bULL, 0xeb009c5c2c49aa2eULL, 0x4eadb217ad8cc09bULL);

    /* Test deliberately misaligned data. */
    count += TEST("abcdefg", 7, 1000, 0x72d01dde5b253701ULL,
        0xc64947b6cb4015f6ULL, 0xf76a0b181f340bc9ULL, 0x02caeadcf740c3d9ULL,
        0x10a7747964fa1dafULL, 0x276603719f0db6baULL, 0xa7236d3662cda042ULL,
        0x55c06216419230c7ULL);

    /* Input size = 33 bits */
#if 0
    count += TEST("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno",
        64,
        16777216,
        0xb47c933421ea2db1LL, 0x49ad6e10fce6c7f9LL,
        0x3d0752380180ffd7LL, 0xf4629a712134831dLL,
        0x77be6091b819ed35LL, 0x2c2967a2e2d4fa50LL,
        0x50723c9630691f1aLL, 0x05a7281dbe6c1086LL
    );
#endif

    if (count)
        return 0; /* failure */
    else
        return 1; /* success */
}



void util_rand_seed(util_rand_t *vctx, const void *vseed, size_t seed_length)
{
    unsigned char digest[64];
    myrand_t *ctx = (myrand_t *)vctx;
    const unsigned char *seed = (const unsigned char *)vseed;

    util_sha512(seed, seed_length, digest, sizeof(digest));
    chacha20_init(ctx, digest, digest+32);
    chacha20_cryptomagic(ctx->buf, ctx->state);
}

void util_rand_stir(util_rand_t *vctx, const void *seed, size_t seed_length)
{
    myrand_t *ctx = (myrand_t *)vctx;
    unsigned char digest[64];
    unsigned char *key = digest + 0;
    unsigned char *nonce = digest + 32;

    /* Convert however many bytes the caller specified, which may
     * be zero, or may be billions, into a hash. This will either
     * distribute the bits across the entire 64-byte space if given
     * a small buffer (like from clock_getttime()), or reduce a large
     * buffer down to 64-bytes in case it's very large. */
    util_sha512(seed, seed_length, digest, sizeof(digest));

    /* Now we XOR the new data with the old data, which is the
     * 'nonce' and 'key' in the ChaCha20 encryption algorithm.
     * Because we are using XOR, we won't make this state any
     * less random. In other words, if the caller specifies 
     * clealry non-random data, it won't convert random seed
     * into non-random seed. */
    ctx->state[4] ^= READ32LE(key + 0);
    ctx->state[5] ^= READ32LE(key + 4);
    ctx->state[6] ^= READ32LE(key + 8);
    ctx->state[7] ^= READ32LE(key + 12);
    ctx->state[8] ^= READ32LE(key + 16);
    ctx->state[9] ^= READ32LE(key + 20);
    ctx->state[10] ^= READ32LE(key + 24);
    ctx->state[11] ^= READ32LE(key + 28);
    ctx->state[14] ^= READ32LE(nonce + 0);
    ctx->state[15] ^= READ32LE(nonce + 4);
}


void util_rand_bytes(util_rand_t *vctx, void *vbuf, size_t length)
{
    myrand_t *ctx = (myrand_t*)vctx;
    size_t i;
    unsigned char *buf = (unsigned char *)vbuf;

    for (i=0; i<length; ) {
        size_t j;
        size_t jlen;
        
        /* Get as many bytes from the pre-calculated buffer as
         * until we've exhausted those. */
        jlen = MIN(64, ctx->partial + length - i);
        for (j=ctx->partial; j<jlen; j++)
            buf[i++] = ctx->buf[j];
        ctx->partial = j;

        /* Once we reach the end of the current pre-calculated numbers,
         * grab the next buffer of numbers. */
        if (ctx->partial >= 64) {
            /* Increment the ChaCha20 block counter. In this code, however
             * once we've incremented past the 64-bit counter boundary,
             * we start incrementing the nonce, giving us a 96-bit length
             * instead of 64-bit length. */
            ctx->state[12]++;
            if (ctx->state[12] == 0) {
                ctx->state[13]++;
                if (ctx->state[13] == 0)
                    ctx->state[14]++;
            }
            chacha20_cryptomagic(ctx->buf, ctx->state);
            ctx->partial = 0;
        }
    }
}

uint64_t util_rand(util_rand_t *ctx)
{
    uint64_t result;
    util_rand_bytes(ctx, &result, sizeof(result));
    return result;
}

uint32_t util_rand32(util_rand_t *ctx)
{
    uint32_t result;
    util_rand_bytes(ctx, &result, sizeof(result));
    return result;
}

uint16_t util_rand16(util_rand_t *ctx)
{
    uint16_t result;
    util_rand_bytes(ctx, &result, sizeof(result));
    return result;
}

unsigned char util_rand8(util_rand_t *ctx)
{
    unsigned char result;
    util_rand_bytes(ctx, &result, sizeof(result));
    return result;
}

/*
 * This is a traditional way of making uniform non-binary numbers out
 * of binary numbers, by continuing to call the rand() function until
 * it delivers a value that'll give a uniform distribution.
 */
uint64_t util_rand_uniform(util_rand_t *ctx, uint64_t upper_bound)
{
    uint64_t threshold;

    if (upper_bound <= 1)
        return 0;
    
    threshold =  -(int64_t)upper_bound % (int64_t)upper_bound;
    for (;;) {
        uint64_t result = util_rand(ctx);
        if (result >= threshold)
            return result % upper_bound;
    }
}

/* see `util_rand_uniform()` for more info */
uint32_t util_rand32_uniform(util_rand_t *ctx, uint32_t upper_bound)
{
    uint32_t threshold;

    if (upper_bound <= 1)
        return 0;
    
    threshold = -(int)upper_bound % (int)upper_bound;
    for (;;) {
        uint32_t result = util_rand32(ctx);
        if (result >= threshold)
            return result % upper_bound;
    }
}

/* see `util_rand_uniform()` for more info */
uint16_t util_rand16_uniform(util_rand_t *ctx, uint16_t upper_bound)
{
    uint32_t threshold;

    if (upper_bound <= 1)
        return 0;
    
    threshold = -(int)upper_bound % (int)upper_bound;
    for (;;) {
        uint32_t result = util_rand16(ctx);
        if (result >= threshold)
            return result % upper_bound;
    }
}

/* see `util_rand_uniform()` for more info */
unsigned char util_rand8_uniform(util_rand_t *ctx, unsigned char upper_bound)
{
    uint32_t threshold;

    if (upper_bound <= 1)
        return 0;
    
    threshold = -upper_bound % upper_bound;
    for (;;) {
        uint32_t result = util_rand8(ctx);
        if (result >= threshold)
            return result % upper_bound;
    }
}
