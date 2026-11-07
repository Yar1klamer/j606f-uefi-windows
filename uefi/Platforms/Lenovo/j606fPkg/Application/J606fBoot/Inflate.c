// Small raw-DEFLATE / gzip decoder (RFC 1951/1952) for the J606F boot menu: unpacks the gzip'ed arm64 kernel Image of the pmOS boot image.
// Written for this project after the structure of Mark Adler's puff.c (canonical Huffman decoding by code-length counts).
// No allocation, no recursion; returns 0 on success.
#include "Inflate.h"

typedef struct {
  unsigned char       *out;
  unsigned long long  outlen, outcnt;
  const unsigned char *in;
  unsigned long long  inlen, incnt;
  unsigned int        bitbuf, bitcnt;
} ISTATE;

typedef struct { short *count; short *symbol; } HUFF;

#define MAXBITS 15
#define MAXLCODES 286
#define MAXDCODES 30
#define FIXLCODES 288

static int bits(ISTATE *s, int need, int *err)
{
  unsigned long val = s->bitbuf;
  while (s->bitcnt < (unsigned)need) {
    if (s->incnt == s->inlen) { *err = 2; return 0; }
    val |= (unsigned long)s->in[s->incnt++] << s->bitcnt;
    s->bitcnt += 8;
  }
  s->bitbuf = (unsigned int)(val >> need);
  s->bitcnt -= need;
  return (int)(val & ((1UL << need) - 1));
}

static int stored(ISTATE *s)
{
  unsigned len;
  s->bitbuf = 0; s->bitcnt = 0;
  if (s->incnt + 4 > s->inlen) return 2;
  len = s->in[s->incnt] | (s->in[s->incnt + 1] << 8);
  if (s->in[s->incnt + 2] != (~len & 0xff) || s->in[s->incnt + 3] != ((~len >> 8) & 0xff)) return -2;
  s->incnt += 4;
  if (s->incnt + len > s->inlen) return 2;
  if (s->outcnt + len > s->outlen) return 1;
  while (len--) s->out[s->outcnt++] = s->in[s->incnt++];
  return 0;
}

static int decode(ISTATE *s, const HUFF *h, int *err)
{
  int code = 0, first = 0, index = 0, len, count;
  for (len = 1; len <= MAXBITS; len++) {
    code |= bits(s, 1, err);
    if (*err) return -1;
    count = h->count[len];
    if (code - count < first) return h->symbol[index + (code - first)];
    index += count;
    first += count;
    first <<= 1;
    code <<= 1;
  }
  *err = -10;
  return -1;
}

static int construct(HUFF *h, const short *length, int n)
{
  int symbol, len, left;
  short offs[MAXBITS + 1];
  for (len = 0; len <= MAXBITS; len++) h->count[len] = 0;
  for (symbol = 0; symbol < n; symbol++) h->count[length[symbol]]++;
  if (h->count[0] == n) return 0;
  left = 1;
  for (len = 1; len <= MAXBITS; len++) {
    left <<= 1;
    left -= h->count[len];
    if (left < 0) return left;
  }
  offs[1] = 0;
  for (len = 1; len < MAXBITS; len++) offs[len + 1] = offs[len] + h->count[len];
  for (symbol = 0; symbol < n; symbol++)
    if (length[symbol] != 0) h->symbol[offs[length[symbol]]++] = (short)symbol;
  return left;
}

static const short lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const short lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const short dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
                                 6145, 8193, 12289, 16385, 24577 };
static const short dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static int codes(ISTATE *s, const HUFF *lencode, const HUFF *distcode)
{
  int symbol, len, err = 0;
  unsigned long long dist;
  for (;;) {
    symbol = decode(s, lencode, &err);
    if (err) return err;
    if (symbol < 256) {
      if (s->outcnt == s->outlen) return 1;
      s->out[s->outcnt++] = (unsigned char)symbol;
    } else if (symbol > 256) {
      symbol -= 257;
      if (symbol >= 29) return -10;
      len = lbase[symbol] + bits(s, lext[symbol], &err);
      if (err) return err;
      symbol = decode(s, distcode, &err);
      if (err) return err;
      if (symbol < 0 || symbol >= 30) return -10;
      dist = (unsigned long long)dbase[symbol] + (unsigned)bits(s, dext[symbol], &err);
      if (err) return err;
      if (dist > s->outcnt) return -11;
      if (s->outcnt + len > s->outlen) return 1;
      while (len--) {
        s->out[s->outcnt] = s->out[s->outcnt - dist];
        s->outcnt++;
      }
    } else {
      return 0;
    }
  }
}

static int fixed(ISTATE *s)
{
  static short lencnt[MAXBITS + 1], lensym[FIXLCODES], distcnt[MAXBITS + 1], distsym[MAXDCODES];
  static HUFF lencode, distcode;
  static int virgin = 1;
  if (virgin) {
    short lengths[FIXLCODES];
    int symbol;
    lencode.count = lencnt; lencode.symbol = lensym;
    distcode.count = distcnt; distcode.symbol = distsym;
    for (symbol = 0; symbol < 144; symbol++) lengths[symbol] = 8;
    for (; symbol < 256; symbol++) lengths[symbol] = 9;
    for (; symbol < 280; symbol++) lengths[symbol] = 7;
    for (; symbol < FIXLCODES; symbol++) lengths[symbol] = 8;
    construct(&lencode, lengths, FIXLCODES);
    for (symbol = 0; symbol < MAXDCODES; symbol++) lengths[symbol] = 5;
    construct(&distcode, lengths, MAXDCODES);
    virgin = 0;
  }
  return codes(s, &lencode, &distcode);
}

static int dynamic(ISTATE *s)
{
  int nlen, ndist, ncode, index, err = 0, symbol, len;
  short lengths[MAXLCODES + MAXDCODES];
  short lencnt[MAXBITS + 1], lensym[MAXLCODES], distcnt[MAXBITS + 1], distsym[MAXDCODES];
  HUFF lencode, distcode;
  static const short order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };

  lencode.count = lencnt; lencode.symbol = lensym;
  distcode.count = distcnt; distcode.symbol = distsym;
  nlen = bits(s, 5, &err) + 257;
  ndist = bits(s, 5, &err) + 1;
  ncode = bits(s, 4, &err) + 4;
  if (err) return err;
  if (nlen > MAXLCODES || ndist > MAXDCODES) return -3;
  for (index = 0; index < ncode; index++) {
    lengths[order[index]] = (short)bits(s, 3, &err);
    if (err) return err;
  }
  for (; index < 19; index++) lengths[order[index]] = 0;
  if (construct(&lencode, lengths, 19) != 0) return -4;
  index = 0;
  while (index < nlen + ndist) {
    symbol = decode(s, &lencode, &err);
    if (err) return err;
    if (symbol < 0) return symbol;
    if (symbol < 16) {
      lengths[index++] = (short)symbol;
    } else {
      len = 0;
      if (symbol == 16) {
        if (index == 0) return -5;
        len = lengths[index - 1];
        symbol = 3 + bits(s, 2, &err);
      } else if (symbol == 17) {
        symbol = 3 + bits(s, 3, &err);
      } else {
        symbol = 11 + bits(s, 7, &err);
      }
      if (err) return err;
      if (index + symbol > nlen + ndist) return -6;
      while (symbol--) lengths[index++] = (short)len;
    }
  }
  if (lengths[256] == 0) return -9;
  err = construct(&lencode, lengths, nlen);
  if (err && (err < 0 || nlen != lencode.count[0] + lencode.count[1])) return -7;
  err = construct(&distcode, lengths + nlen, ndist);
  if (err && (err < 0 || ndist != distcode.count[0] + distcode.count[1])) return -8;
  return codes(s, &lencode, &distcode);
}

int RawInflate(unsigned char *dest, unsigned long long *destlen, const unsigned char *source, unsigned long long *sourcelen)
{
  ISTATE s;
  int last, type, err = 0;

  s.out = dest; s.outlen = *destlen; s.outcnt = 0;
  s.in = source; s.inlen = *sourcelen; s.incnt = 0;
  s.bitbuf = 0; s.bitcnt = 0;
  do {
    last = bits(&s, 1, &err);
    type = bits(&s, 2, &err);
    if (err) break;
    err = type == 0 ? stored(&s) : type == 1 ? fixed(&s) : type == 2 ? dynamic(&s) : -1;
    if (err != 0) break;
  } while (!last);
  *destlen = s.outcnt;
  *sourcelen = s.incnt;
  return err;
}

// gzip member: header (RFC 1952), raw deflate, CRC32 + ISIZE (CRC is checked by the caller if wanted)
unsigned long long GzipSize(const unsigned char *src, unsigned long long len)
{
  if (len < 18 || src[0] != 0x1f || src[1] != 0x8b) return 0;
  return (unsigned long long)src[len - 4] | ((unsigned long long)src[len - 3] << 8) | ((unsigned long long)src[len - 2] << 16) |
         ((unsigned long long)src[len - 1] << 24);
}

int GunzipBuf(unsigned char *dest, unsigned long long *destlen, const unsigned char *src, unsigned long long len)
{
  unsigned long long p = 10, n;
  unsigned flg;

  if (len < 18 || src[0] != 0x1f || src[1] != 0x8b || src[2] != 8) return -20;
  flg = src[3];
  if (flg & 4) { if (p + 2 > len) return -21; p += 2 + (src[p] | (src[p + 1] << 8)); }
  if (flg & 8) { while (p < len && src[p]) p++; p++; }
  if (flg & 16) { while (p < len && src[p]) p++; p++; }
  if (flg & 2) p += 2;
  if (p >= len) return -22;
  n = len - p;
  return RawInflate(dest, destlen, src + p, &n);
}

unsigned int Crc32(const unsigned char *p, unsigned long long n)
{
  unsigned int c = 0xffffffffu;
  int k;
  while (n--) {
    c ^= *p++;
    for (k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
  }
  return ~c;
}
