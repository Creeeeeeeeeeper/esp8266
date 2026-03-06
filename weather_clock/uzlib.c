/* uzlib source - combined for Arduino IDE compilation */
/* Original: uzlib by Paul Sokolovsky, Joergen Ibsen */

#include <assert.h>
#include <string.h>
#include "uzlib.h"

/* ============ tinflate.c ============ */

#define UZLIB_DUMP_ARRAY(heading, arr, size) \
    { \
        printf("%s", heading); \
        for (int i = 0; i < size; ++i) { \
            printf(" %d", (arr)[i]); \
        } \
        printf("\n"); \
    }

uint32_t tinf_get_le_uint32(TINF_DATA *d);
uint32_t tinf_get_be_uint32(TINF_DATA *d);

const unsigned char length_bits[30] = {
   0, 0, 0, 0, 0, 0, 0, 0,
   1, 1, 1, 1, 2, 2, 2, 2,
   3, 3, 3, 3, 4, 4, 4, 4,
   5, 5, 5, 5
};
const unsigned short length_base[30] = {
   3, 4, 5, 6, 7, 8, 9, 10,
   11, 13, 15, 17, 19, 23, 27, 31,
   35, 43, 51, 59, 67, 83, 99, 115,
   131, 163, 195, 227, 258
};

const unsigned char dist_bits[30] = {
   0, 0, 0, 0, 1, 1, 2, 2,
   3, 3, 4, 4, 5, 5, 6, 6,
   7, 7, 8, 8, 9, 9, 10, 10,
   11, 11, 12, 12, 13, 13
};
const unsigned short dist_base[30] = {
   1, 2, 3, 4, 5, 7, 9, 13,
   17, 25, 33, 49, 65, 97, 129, 193,
   257, 385, 513, 769, 1025, 1537, 2049, 3073,
   4097, 6145, 8193, 12289, 16385, 24577
};

const unsigned char clcidx[] = {
   16, 17, 18, 0, 8, 7, 9, 6,
   10, 5, 11, 4, 12, 3, 13, 2,
   14, 1, 15
};

unsigned char uzlib_get_byte(TINF_DATA *d)
{
    if (d->source < d->source_limit) {
        return *d->source++;
    }
    if (d->readSource && !d->eof) {
        int val = d->readSource(d);
        if (val >= 0) {
            return (unsigned char)val;
        }
    }
    d->eof = true;
    return 0;
}

uint32_t tinf_get_le_uint32(TINF_DATA *d)
{
    uint32_t val = 0;
    int i;
    for (i = 4; i--;) {
        val = val >> 8 | ((uint32_t)uzlib_get_byte(d)) << 24;
    }
    return val;
}

uint32_t tinf_get_be_uint32(TINF_DATA *d)
{
    uint32_t val = 0;
    int i;
    for (i = 4; i--;) {
        val = val << 8 | uzlib_get_byte(d);
    }
    return val;
}

static int tinf_getbit(TINF_DATA *d)
{
   unsigned int bit;
   if (!d->bitcount--)
   {
      d->tag = uzlib_get_byte(d);
      d->bitcount = 7;
   }
   bit = d->tag & 0x01;
   d->tag >>= 1;
   return bit;
}

static unsigned int tinf_read_bits(TINF_DATA *d, int num, int base)
{
   unsigned int val = 0;
   if (num)
   {
      unsigned int limit = 1 << (num);
      unsigned int mask;
      for (mask = 1; mask < limit; mask *= 2)
         if (tinf_getbit(d)) val += mask;
   }
   return val + base;
}

static void tinf_build_fixed_trees(TINF_TREE *lt, TINF_TREE *dt)
{
   int i;
   for (i = 0; i < 7; ++i) lt->table[i] = 0;
   lt->table[7] = 24;
   lt->table[8] = 152;
   lt->table[9] = 112;
   for (i = 0; i < 24; ++i) lt->trans[i] = 256 + i;
   for (i = 0; i < 144; ++i) lt->trans[24 + i] = i;
   for (i = 0; i < 8; ++i) lt->trans[24 + 144 + i] = 280 + i;
   for (i = 0; i < 112; ++i) lt->trans[24 + 144 + 8 + i] = 144 + i;
   for (i = 0; i < 5; ++i) dt->table[i] = 0;
   dt->table[5] = 32;
   for (i = 0; i < 32; ++i) dt->trans[i] = i;
}

static void tinf_build_tree(TINF_TREE *t, const unsigned char *lengths, unsigned int num)
{
   unsigned short offs[16];
   unsigned int i, sum;
   for (i = 0; i < 16; ++i) t->table[i] = 0;
   for (i = 0; i < num; ++i) t->table[lengths[i]]++;
   t->table[0] = 0;
   for (sum = 0, i = 0; i < 16; ++i)
   {
      offs[i] = sum;
      sum += t->table[i];
   }
   for (i = 0; i < num; ++i)
   {
      if (lengths[i]) t->trans[offs[lengths[i]]++] = i;
   }
}

static int tinf_decode_symbol(TINF_DATA *d, TINF_TREE *t)
{
   int sum = 0, cur = 0, len = 0;
   do {
      cur = 2*cur + tinf_getbit(d);
      if (++len == TINF_ARRAY_SIZE(t->table)) {
         return TINF_DATA_ERROR;
      }
      sum += t->table[len];
      cur -= t->table[len];
   } while (cur >= 0);
   sum += cur;
   return t->trans[sum];
}

static int tinf_decode_trees(TINF_DATA *d, TINF_TREE *lt, TINF_TREE *dt)
{
   unsigned char lengths[288+32];
   unsigned int hlit, hdist, hclen, hlimit;
   unsigned int i, num, length;

   hlit = tinf_read_bits(d, 5, 257);
   hdist = tinf_read_bits(d, 5, 1);
   hclen = tinf_read_bits(d, 4, 4);

   for (i = 0; i < 19; ++i) lengths[i] = 0;
   for (i = 0; i < hclen; ++i)
   {
      unsigned int clen = tinf_read_bits(d, 3, 0);
      lengths[clcidx[i]] = clen;
   }
   tinf_build_tree(lt, lengths, 19);

   hlimit = hlit + hdist;
   for (num = 0; num < hlimit; )
   {
      int sym = tinf_decode_symbol(d, lt);
      unsigned char fill_value = 0;
      int lbits, lbase = 3;

      if (sym < 0) return sym;

      switch (sym)
      {
      case 16:
         if (num == 0) return TINF_DATA_ERROR;
         fill_value = lengths[num - 1];
         lbits = 2;
         break;
      case 17:
         lbits = 3;
         break;
      case 18:
         lbits = 7;
         lbase = 11;
         break;
      default:
         lengths[num++] = sym;
         continue;
      }

      length = tinf_read_bits(d, lbits, lbase);
      if (num + length > hlimit) return TINF_DATA_ERROR;
      for (; length; --length)
      {
         lengths[num++] = fill_value;
      }
   }

   tinf_build_tree(lt, lengths, hlit);
   tinf_build_tree(dt, lengths + hlit, hdist);

   return TINF_OK;
}

static int tinf_inflate_block_data(TINF_DATA *d, TINF_TREE *lt, TINF_TREE *dt)
{
    if (d->curlen == 0) {
        unsigned int offs;
        int dist;
        int sym = tinf_decode_symbol(d, lt);

        if (d->eof) {
            return TINF_DATA_ERROR;
        }

        if (sym < 256) {
            TINF_PUT(d, sym);
            return TINF_OK;
        }

        if (sym == 256) {
            return TINF_DONE;
        }

        sym -= 257;
        if (sym >= 29) {
            return TINF_DATA_ERROR;
        }

        d->curlen = tinf_read_bits(d, length_bits[sym], length_base[sym]);

        dist = tinf_decode_symbol(d, dt);
        if (dist >= 30) {
            return TINF_DATA_ERROR;
        }

        offs = tinf_read_bits(d, dist_bits[dist], dist_base[dist]);

        if (d->dict_ring) {
            if (offs > d->dict_size) {
                return TINF_DICT_ERROR;
            }
            d->lzOff = d->dict_idx - offs;
            if (d->lzOff < 0) {
                d->lzOff += d->dict_size;
            }
        } else {
            if (offs > (unsigned int)(d->dest - d->destStart)) {
                return TINF_DATA_ERROR;
            }
            d->lzOff = -offs;
        }
    }

    if (d->dict_ring) {
        TINF_PUT(d, d->dict_ring[d->lzOff]);
        if ((unsigned)++d->lzOff == d->dict_size) {
            d->lzOff = 0;
        }
    } else {
        d->dest[0] = d->dest[d->lzOff];
        d->dest++;
    }
    d->curlen--;
    return TINF_OK;
}

static int tinf_inflate_uncompressed_block(TINF_DATA *d)
{
    if (d->curlen == 0) {
        unsigned int length, invlength;
        length = uzlib_get_byte(d);
        length += 256 * uzlib_get_byte(d);
        invlength = uzlib_get_byte(d);
        invlength += 256 * uzlib_get_byte(d);
        if (length != (~invlength & 0x0000ffff)) return TINF_DATA_ERROR;
        d->curlen = length + 1;
        d->bitcount = 0;
    }

    if (--d->curlen == 0) {
        return TINF_DONE;
    }

    unsigned char c = uzlib_get_byte(d);
    TINF_PUT(d, c);
    return TINF_OK;
}

void uzlib_init(void)
{
}

void uzlib_uncompress_init(TINF_DATA *d, void *dict, unsigned int dictLen)
{
   d->eof = 0;
   d->bitcount = 0;
   d->bfinal = 0;
   d->btype = -1;
   d->dict_size = dictLen;
   d->dict_ring = dict;
   d->dict_idx = 0;
   d->curlen = 0;
}

int uzlib_uncompress(TINF_DATA *d)
{
    do {
        int res;

        if (d->btype == -1) {
            int old_btype;
next_blk:
            old_btype = d->btype;
            d->bfinal = tinf_getbit(d);
            d->btype = tinf_read_bits(d, 2, 0);

            if (d->btype == 1 && old_btype != 1) {
                tinf_build_fixed_trees(&d->ltree, &d->dtree);
            } else if (d->btype == 2) {
                res = tinf_decode_trees(d, &d->ltree, &d->dtree);
                if (res != TINF_OK) {
                    return res;
                }
            }
        }

        switch (d->btype)
        {
        case 0:
            res = tinf_inflate_uncompressed_block(d);
            break;
        case 1:
        case 2:
            res = tinf_inflate_block_data(d, &d->ltree, &d->dtree);
            break;
        default:
            return TINF_DATA_ERROR;
        }

        if (res == TINF_DONE && !d->bfinal) {
            goto next_blk;
        }

        if (res != TINF_OK) {
            return res;
        }

    } while (d->dest < d->dest_limit);

    return TINF_OK;
}

int uzlib_uncompress_chksum(TINF_DATA *d)
{
    int res;
    unsigned char *data = d->dest;

    res = uzlib_uncompress(d);

    if (res < 0) return res;

    switch (d->checksum_type) {
    case TINF_CHKSUM_ADLER:
        d->checksum = uzlib_adler32(data, d->dest - data, d->checksum);
        break;
    case TINF_CHKSUM_CRC:
        d->checksum = uzlib_crc32(data, d->dest - data, d->checksum);
        break;
    }

    if (res == TINF_DONE) {
        unsigned int val;
        switch (d->checksum_type) {
        case TINF_CHKSUM_ADLER:
            val = tinf_get_be_uint32(d);
            if (d->checksum != val) {
                return TINF_CHKSUM_ERROR;
            }
            break;
        case TINF_CHKSUM_CRC:
            val = tinf_get_le_uint32(d);
            if (~d->checksum != val) {
                return TINF_CHKSUM_ERROR;
            }
            val = tinf_get_le_uint32(d);
            break;
        }
    }

    return res;
}

/* ============ tinfgzip.c ============ */

#define FTEXT    1
#define FHCRC    2
#define FEXTRA   4
#define FNAME    8
#define FCOMMENT 16

static void tinf_skip_bytes(TINF_DATA *d, int num)
{
    while (num--) uzlib_get_byte(d);
}

static uint16_t tinf_get_uint16(TINF_DATA *d)
{
    unsigned int v = uzlib_get_byte(d);
    v = (uzlib_get_byte(d) << 8) | v;
    return v;
}

int uzlib_gzip_parse_header(TINF_DATA *d)
{
    unsigned char flg;

    if (uzlib_get_byte(d) != 0x1f || uzlib_get_byte(d) != 0x8b) return TINF_DATA_ERROR;
    if (uzlib_get_byte(d) != 8) return TINF_DATA_ERROR;

    flg = uzlib_get_byte(d);
    if (flg & 0xe0) return TINF_DATA_ERROR;

    tinf_skip_bytes(d, 6);

    if (flg & FEXTRA)
    {
       unsigned int xlen = tinf_get_uint16(d);
       tinf_skip_bytes(d, xlen);
    }

    if (flg & FNAME) { while (uzlib_get_byte(d)); }
    if (flg & FCOMMENT) { while (uzlib_get_byte(d)); }

    if (flg & FHCRC)
    {
       tinf_get_uint16(d);
    }

    d->checksum_type = TINF_CHKSUM_CRC;
    d->checksum = ~0;

    return TINF_OK;
}

/* ============ crc32.c ============ */

static const uint32_t tinf_crc32tab[16] = {
   0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190,
   0x6b6b51f4, 0x4db26158, 0x5005713c, 0xedb88320, 0xf00f9344,
   0xd6d6a3e8, 0xcb61b38c, 0x9b64c2b0, 0x86d3d2d4, 0xa00ae278,
   0xbdbdf21c
};

uint32_t uzlib_crc32(const void *data, unsigned int length, uint32_t crc)
{
   const unsigned char *buf = (const unsigned char *)data;
   unsigned int i;

   for (i = 0; i < length; ++i)
   {
      crc ^= buf[i];
      crc = tinf_crc32tab[crc & 0x0f] ^ (crc >> 4);
      crc = tinf_crc32tab[crc & 0x0f] ^ (crc >> 4);
   }

   return crc;
}

/* ============ adler32.c ============ */

#define A32_BASE 65521
#define A32_NMAX 5552

uint32_t uzlib_adler32(const void *data, unsigned int length, uint32_t prev_sum)
{
   const unsigned char *buf = (const unsigned char *)data;

   unsigned int s1 = prev_sum & 0xffff;
   unsigned int s2 = prev_sum >> 16;

   while (length > 0)
   {
      int k = length < A32_NMAX ? length : A32_NMAX;
      int i;

      for (i = k / 16; i; --i, buf += 16)
      {
         s1 += buf[0];  s2 += s1; s1 += buf[1];  s2 += s1;
         s1 += buf[2];  s2 += s1; s1 += buf[3];  s2 += s1;
         s1 += buf[4];  s2 += s1; s1 += buf[5];  s2 += s1;
         s1 += buf[6];  s2 += s1; s1 += buf[7];  s2 += s1;

         s1 += buf[8];  s2 += s1; s1 += buf[9];  s2 += s1;
         s1 += buf[10]; s2 += s1; s1 += buf[11]; s2 += s1;
         s1 += buf[12]; s2 += s1; s1 += buf[13]; s2 += s1;
         s1 += buf[14]; s2 += s1; s1 += buf[15]; s2 += s1;
      }

      for (i = k % 16; i; --i) { s1 += *buf++; s2 += s1; }

      s1 %= A32_BASE;
      s2 %= A32_BASE;

      length -= k;
   }

   return ((uint32_t)s2 << 16) | s1;
}
