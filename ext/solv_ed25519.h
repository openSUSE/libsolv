/*
 * Copyright (c) 2020, SUSE LLC.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/* simple and slow ed25519 verification code. */

#ifndef LIBSOLV_CHKSUM_H
#include "chksum.h"
#endif

#define MPED25519_LEN (32 / MP_T_BYTES)

#if MP_T_BYTES == 4
# define MPED25519_CONST1(x) (mp_t)x
#elif MP_T_BYTES == 1
# define MPED25519_CONST1(x) (mp_t)(x >> 0), (mp_t)(x >> 8), (mp_t)(x >> 16), (mp_t)(x >> 24)
#endif

#define MPED25519_CONST(a, b, c, d, e, f, g, h) { \
  MPED25519_CONST1(h), MPED25519_CONST1(g), MPED25519_CONST1(f), MPED25519_CONST1(e), \
  MPED25519_CONST1(d), MPED25519_CONST1(c), MPED25519_CONST1(b), MPED25519_CONST1(a) \
}

static mp_t ed25519_q[] =		/* mod prime */
  MPED25519_CONST(0x7FFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFED);
static mp_t ed25519_d[] = 		/* -(121665/121666) */
  MPED25519_CONST(0x52036CEE, 0x2B6FFE73, 0x8CC74079, 0x7779E898, 0x00700A4D, 0x4141D8AB, 0x75EB4DCA, 0x135978A3);
static mp_t ed25519_sqrtm1[] =		/* sqrt(-1) */
  MPED25519_CONST(0x2B832480, 0x4FC1DF0B, 0x2B4D0099, 0x3DFBD7A7, 0x2F431806, 0xAD2FE478, 0xC4EE1B27, 0x4A0EA0B0);
static mp_t ed25519_l[] = 		/* order of base point */
  MPED25519_CONST(0x10000000, 0x00000000, 0x00000000, 0x00000000, 0x14DEF9DE, 0xA2F79CD6, 0x5812631A, 0x5CF5D3ED);
static mp_t ed25519_gx[] = 		/* base point */
  MPED25519_CONST(0x216936D3, 0xCD6E53FE, 0xC0A4E231, 0xFDD6DC5C, 0x692CC760, 0x9525A7B2, 0xC9562D60, 0x8F25D51A);
static mp_t ed25519_gy[] = 		/* base point */
  MPED25519_CONST(0x66666666, 0x66666666, 0x66666666, 0x66666666, 0x66666666, 0x66666666, 0x66666666, 0x66666658);
static mp_t ed25519_one[] = 		/* 1 */
  MPED25519_CONST(0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000001);

/* small helpers to save some typing */
static inline void
mped25519_add(mp_t *target, mp_t *m1, mp_t *m2)
{
  mpadd(MPED25519_LEN, target, m1, m2, ed25519_q);
}

static inline void
mped25519_sub(mp_t *target, mp_t *m1, mp_t *m2)
{
  mpsub(MPED25519_LEN, target, m1, m2, ed25519_q);
}

/* target = 512 bit input modulo ed25519_q */
static void
mped25519_reduce512(mp_t *target, mp_t *a)
{
  int i;
  mp2_t x;

  for (x = 0, i = 0; i < MPED25519_LEN; i++)
    {
      x += (mp2_t)a[i] + (mp2_t)a[i + MPED25519_LEN] * 38;
      target[i] = x;
      x >>= MP_T_BITS;
    }
  x *= 38;
  if ((target[MPED25519_LEN - 1] & (1 << (MP_T_BITS - 1))) != 0)
    {
      x += 19;
      target[MPED25519_LEN - 1] -= 1 << (MP_T_BITS - 1);
    }
  for (i = 0; x && i < MPED25519_LEN; i++)
    {
      x += (mp2_t)target[i];
      target[i] = x;
      x >>= MP_T_BITS;
    }
  if (target[MPED25519_LEN - 1] > ed25519_q[MPED25519_LEN - 1] ||
      (target[MPED25519_LEN - 1] == ed25519_q[MPED25519_LEN - 1] && !mpisless(MPED25519_LEN - 1, target, ed25519_q)))
    mpsubmod(MPED25519_LEN, target, ed25519_q);
}

static void
mped25519_mul(mp_t *target, mp_t *m1, mp_t *m2)
{
  mp_t tmp[MPED25519_LEN * 2];
  int i, j;
  mp2_t x;

  mpzero(MPED25519_LEN * 2, tmp);
  for (i = 0; i < MPED25519_LEN; i++)
    {
      for (x = 0, j = 0; j < MPED25519_LEN; j++)
	{
	  x += (mp2_t)tmp[i + j] + (mp2_t)m1[i] * m2[j];
	  tmp[i + j] = x;
	  x >>= MP_T_BITS;
	}
      tmp[i + j] = x;
    }
  mped25519_reduce512(target, tmp);
}

static inline void
mped25519_sqr(mp_t *target, mp_t *m)
{
  mped25519_mul(target, m, m);
}

/* target = a ^ (2^252 - 4), a11 = a ^ 11 */
static void
mped25519_pow_252_4(mp_t *target, mp_t *a, mp_t *a_11)
{
  static const int todo[16] = { 0, 5, 1, 10, 2, 20, 3, 10, 2, 50, 5, 100, 6, 50, 5, 2 };
  mp_t data[9][MPED25519_LEN];
  int i, j;

  mpcpy(MPED25519_LEN, target, a);
  mped25519_sqr(target, target);
  mpcpy(MPED25519_LEN, a_11, target);
  mped25519_sqr(target, target);
  mped25519_sqr(target, target);
  mped25519_mul(data[0], target, a);		/* data[0] = 9 */
  mped25519_mul(a_11, data[0], a_11);		/* a_11 = 11 */
  mped25519_mul(target, a_11, a_11);		/* target = 22 */
  for (i = 0; i < 8; i++)
    {
      mped25519_mul(target, target, data[todo[i * 2]]);
      mpcpy(MPED25519_LEN, data[i + 1], target);
      for (j = todo[i * 2 + 1]; j-- > 0;)
        mped25519_sqr(target, target);
    }
}

/* target = a ^ (2^252 - 3) */
static void
mped25519_pow_252_3(mp_t *target, mp_t *a)
{
  mp_t t11[MPED25519_LEN];
  mped25519_pow_252_4(target, a, t11);
  mped25519_mul(target, target, a);
}

/* target = a ^ -1 = a ^ (2^255 - 21) */
static void
mped25519_inv(mp_t *target, mp_t *a)
{
  mp_t t[MPED25519_LEN], t11[MPED25519_LEN];
  mped25519_pow_252_4(t, a, t11);
  mped25519_sqr(t, t);
  mped25519_sqr(t, t);
  mped25519_sqr(t, t);		/* 2^255 - 32 */
  mped25519_mul(target, t, t11);
}

static void
mped25519_reduce512_l(mp_t *target, mp_t *a)
{
  mp_t tmp[MPED25519_LEN];
  mpzero(MPED25519_LEN, target);
  mpmul_add(MPED25519_LEN, target, ed25519_one, MPED25519_LEN * 2, a, tmp, ed25519_l);
}

/* recover x coordinate from y and sign */
static int
mped25519_recover_x(mp_t *x, mp_t *y, int sign)
{
  mp_t num[MPED25519_LEN], den[MPED25519_LEN];
  mp_t tmp1[MPED25519_LEN], tmp2[MPED25519_LEN];

  if (!mpisless(MPED25519_LEN, y, ed25519_q))
    return 0;
  /* calculate num=y^2-1 and den=dy^2+1 */
  mped25519_sqr(num, y);
  mped25519_mul(den, num, ed25519_d);
  mped25519_sub(num, num, ed25519_one);
  mped25519_add(den, den, ed25519_one);

  /* calculate x = num*den^3 * (num*den^7)^((q-5)/8) */
  mped25519_sqr(tmp1, den);		/* tmp1 = den^2 */
  mped25519_mul(tmp2, tmp1, den);	/* tmp2 = den^3 */
  mped25519_sqr(tmp1, tmp2);		/* tmp1 = den^6 */
  mped25519_mul(x, tmp1, den);		/* x = den^7 */
  mped25519_mul(tmp1, x, num);		/* tmp1 = num * den^7 */
  mped25519_pow_252_3(x, tmp1);		/* x = tmp1^((q-5)/8) */
  mped25519_mul(tmp1, x, tmp2);		/* tmp1 = x * den^3 */
  mped25519_mul(x, tmp1, num);		/* x = tmp1 * num */

  /* check if den*x^2 == num */
  mped25519_sqr(tmp2, x);
  mped25519_mul(tmp1, tmp2, den);
  if (!mpisequal(MPED25519_LEN, tmp1, num)) {
    mped25519_mul(x, x, ed25519_sqrtm1);	/* x = x * sqrt(-1) */
    mped25519_sqr(tmp2, x);
    mped25519_mul(tmp1, tmp2, den);
    if (!mpisequal(MPED25519_LEN, tmp1, num))
      return 0;
  }
  if (mpiszero(MPED25519_LEN, x))
    return 0;				/* (0,1) is bad */
  if ((x[0] & 1) != sign)
    mped25519_sub(x, ed25519_q, x);
  return 1;
}

/* see https://hyperelliptic.org/EFD/g1p/auto-twisted-projective.html */
/* M=7 add=6 */
static void
mped25519_ptdouble(mp_t *p_x, mp_t *p_y, mp_t *p_z)
{
  mp_t B[MPED25519_LEN], C[MPED25519_LEN], D[MPED25519_LEN];
  mp_t F[MPED25519_LEN], H[MPED25519_LEN], J[MPED25519_LEN];
  
  mped25519_add(C, p_x, p_y);
  mped25519_sqr(B, C);
  mped25519_sqr(C, p_x);
  mped25519_sqr(D, p_y);
  mped25519_sub(F, C, D);
  mped25519_sqr(H, p_z);
  mped25519_add(H, H, H);
  mped25519_add(J, F, H);
  mped25519_add(H, C, D);
  mped25519_mul(p_y, H, F);
  mped25519_mul(p_z, J, F);
  mped25519_sub(H, H, B);
  mped25519_mul(p_x, J, H);
}

/* M=12 add=7 */
static void
mped25519_ptadd(mp_t *p_x, mp_t *p_y, mp_t *p_z, mp_t *q_x, mp_t *q_y, mp_t *q_z)
{
  mp_t A[MPED25519_LEN], B[MPED25519_LEN], C[MPED25519_LEN];
  mp_t D[MPED25519_LEN], E[MPED25519_LEN], F[MPED25519_LEN];
  mp_t G[MPED25519_LEN], H[MPED25519_LEN], J[MPED25519_LEN];
  
  mped25519_mul(A, p_z, q_z);
  mped25519_sqr(B, A);
  mped25519_mul(C, p_x, q_x);
  mped25519_mul(D, p_y, q_y);
  mped25519_mul(F, ed25519_d, C);
  mped25519_mul(E, F, D);
  mped25519_sub(F, B, E);
  mped25519_add(G, B, E);
  mped25519_add(H, p_x, p_y);
  mped25519_add(J, q_x, q_y);
  mped25519_mul(p_x, H, J);
  mped25519_sub(p_x, p_x, C);
  mped25519_sub(p_x, p_x, D);
  mped25519_mul(H, p_x, F);
  mped25519_mul(p_x, H, A);
  mped25519_add(H, D, C);
  mped25519_mul(J, H, G);
  mped25519_mul(p_y, J, A);
  mped25519_mul(p_z, F, G);
}

static inline void
mped25519_ptcpy(mp_t *p_x, mp_t *p_y, mp_t *p_z, mp_t *q_x, mp_t *q_y, mp_t *q_z)
{
  mpcpy(MPED25519_LEN, p_x, q_x);
  mpcpy(MPED25519_LEN, p_y, q_y);
  mpcpy(MPED25519_LEN, p_z, q_z);
}

#if 0
static void
mped25519_mpdump(mp_t *p, char *s, int c)
{
  if (c)
    fprintf(stderr, "%c.", c);
  mpdump(MPED25519_LEN, p, s);
}

static void
mped25519_ptdump(mp_t *p_x, mp_t *p_y, mp_t *p_z, char *s)
{
  mp_t zi[MPED25519_LEN], px[MPED25519_LEN], py[MPED25519_LEN];
  mped25519_mpdump(p_x, s, 'X');
  mped25519_mpdump(p_y, s, 'Y');
  mped25519_mpdump(p_z, s, 'Z');
  mped25519_inv(zi, p_z);
  mped25519_mul(px, p_x, zi);
  mped25519_mul(py, p_y, zi);
  mped25519_mpdump(px, s, 'x');
  mped25519_mpdump(py, s, 'y');
}
#endif


/* scalar multiplication and add: r = s1*p1 + s2*p2 */
/* needs about 13 + (256 - 32) / 2 = 125 point additions */
static int
mped25519_scmult2(mp_t *r_x, mp_t *r_y, mp_t *s1, mp_t *p1_x, mp_t * p1_y, mp_t *s2, mp_t *p2_x, mp_t * p2_y)
{
  mp_t p_x[MPED25519_LEN], p_y[MPED25519_LEN], p_z[MPED25519_LEN];
  mp_t pi_z[MPED25519_LEN];
  mp_t tabx[16][MPED25519_LEN], taby[16][MPED25519_LEN], tabz[16][MPED25519_LEN];
  int i, x, dodouble = 0;

  mpzero(MPED25519_LEN, p_x);
  mpzero(MPED25519_LEN, p_y);
  mpzero(MPED25519_LEN, p_z);
  p_y[0] = p_z[0] = 1;
  
  /* setup table: tab[i * 4 + j] = i * p1 + j * p2 */
  mped25519_ptcpy(tabx[0], taby[0], tabz[0], p_x, p_y, p_z);
  for (i = 4; i < 16; i += 4)
    mped25519_ptcpy(tabx[i], taby[i], tabz[i], p1_x, p1_y, ed25519_one);
  mped25519_ptdouble(tabx[8], taby[8], tabz[8]);
  mped25519_ptadd(tabx[12], taby[12], tabz[12], tabx[8], taby[8], tabz[8]);
  mped25519_ptcpy(tabx[1], taby[1], tabz[1], p2_x, p2_y, ed25519_one);
  for (i = 2; i < 16; i++)
    {
      if ((i & 3) == 0)
	continue;
      mped25519_ptcpy(tabx[i], taby[i], tabz[i], tabx[i - 1], taby[i - 1], tabz[i - 1]);
      mped25519_ptadd(tabx[i], taby[i], tabz[i], p2_x, p2_y, ed25519_one);
    }
  /* now do the scalar multiplication */
  for (i = 255; i >= 0; i--)
    {
      if (dodouble)
        mped25519_ptdouble(p_x, p_y, p_z);
      x  = s1[i / MP_T_BITS] & (1 << (i % MP_T_BITS)) ? 4 : 0;
      x |= s2[i / MP_T_BITS] & (1 << (i % MP_T_BITS)) ? 1 : 0;
      if (!x)
	continue;
      if (i > 0)
	{
	  i--;
	  if (dodouble)
	    mped25519_ptdouble(p_x, p_y, p_z);
	  x <<= 1;
	  x |= s1[i / MP_T_BITS] & (1 << (i % MP_T_BITS)) ? 4 : 0;
	  x |= s2[i / MP_T_BITS] & (1 << (i % MP_T_BITS)) ? 1 : 0;
	}
      mped25519_ptadd(p_x, p_y, p_z, tabx[x], taby[x], tabz[x]);
      dodouble = 1;
    }
#if 0
  mped25519_ptdump(p_x, p_y, p_z, "r   = ");
#endif
  mped25519_inv(pi_z, p_z);
  mped25519_mul(r_x, p_x, pi_z);
  mped25519_mul(r_y, p_y, pi_z);
  return mpiszero(MPED25519_LEN, p_z) ? 0 : 1;
}

static void
mped25519_setfromle(int len, mp_t *out, const unsigned char *buf, int bufl, int highmask)
{
  unsigned char bebuf[64];	/* bufl must be <= 64 */
  int i;
  for (i = 0; i < bufl; i++)
    bebuf[bufl - 1 - i] = buf[i];
  bebuf[0] &= highmask;
  mpsetfrombe(len, out, bebuf, bufl);
}

static int
mped25519(const unsigned char *pub, const unsigned char *sig, const unsigned char *data, unsigned int datal)
{
  unsigned char hbuf[64], rbuf[32];
  int i;
  mp_t pub_x[MPED25519_LEN], pub_y[MPED25519_LEN];
  mp_t h[MPED25519_LEN], s[MPED25519_LEN], h2[MPED25519_LEN * 2];
  mp_t r_x[MPED25519_LEN], r_y[MPED25519_LEN];
  Chksum *chk;

  mped25519_setfromle(MPED25519_LEN, s, sig + 32, 32, 0xff);
  if (!mpisless(MPED25519_LEN, s, ed25519_l))
    return 0;		/* bad s */
  /* uncompress pubkey, we invert the sign to get -pub */
  mped25519_setfromle(MPED25519_LEN, pub_y, pub, 32, 0x7f);
  if (!mped25519_recover_x(pub_x, pub_y, pub[31] & 0x80 ? 0 : 1))
    return 0;		/* bad pubkey */
#if 0
  mped25519_mpdump(pub_x, "-pubx = ", 0);
  mped25519_mpdump(pub_y, "puby  = ", 0);
#endif
  /* calculate h = H(sig[0..31]|pub|data) mod l */
  chk = solv_chksum_create(REPOKEY_TYPE_SHA512);
  if (!chk)
    return 0;
  solv_chksum_add(chk, sig, 32);
  solv_chksum_add(chk, pub, 32);
  solv_chksum_add(chk, data, datal);
  solv_chksum_free(chk, hbuf);
  mped25519_setfromle(MPED25519_LEN * 2, h2, hbuf, 64, 0xff);
  mped25519_reduce512_l(h, h2);
#if 0
  mped25519_mpdump(s, "s     = ", 0);
  mped25519_mpdump(h, "h     = ", 0);
#endif
  /* calculate r = s * G - h * pub */
  if (!mped25519_scmult2(r_x, r_y, s, ed25519_gx, ed25519_gy, h, pub_x, pub_y))
    return 0;
  /* compress r into rbuf and verify that it matches sig */
  for (i = 0; i < 32; i++)
    rbuf[i] = r_y[i / MP_T_BYTES] >> (8 * (i % MP_T_BYTES));
  if ((r_x[0] & 1) != 0)
    rbuf[31] |= 0x80;
  return memcmp(sig, rbuf, 32) == 0 ? 1 : 0;
}

