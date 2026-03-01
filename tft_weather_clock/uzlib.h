/* uzlib.h - standalone header for project-local uzlib build */
#ifndef UZLIB_H_INCLUDED
#define UZLIB_H_INCLUDED

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

struct Outbuf {
    unsigned char *outbuf;
    int outlen, outsize;
    unsigned long outbits;
    int noutbits;
    int comp_disabled;
};

#ifndef UZLIB_CONF_DEBUG_LOG
#define UZLIB_CONF_DEBUG_LOG 0
#endif
#ifndef UZLIB_CONF_PARANOID_CHECKS
#define UZLIB_CONF_PARANOID_CHECKS 0
#endif
#ifndef UZLIB_CONF_USE_MEMCPY
#define UZLIB_CONF_USE_MEMCPY 0
#endif

#ifndef TINFCC
#define TINFCC
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define TINF_OK             0
#define TINF_DONE           1
#define TINF_DATA_ERROR    (-3)
#define TINF_CHKSUM_ERROR  (-4)
#define TINF_DICT_ERROR    (-5)

#define TINF_CHKSUM_NONE  0
#define TINF_CHKSUM_ADLER 1
#define TINF_CHKSUM_CRC   2

#define TINF_ARRAY_SIZE(arr) (sizeof(arr) / sizeof(*(arr)))

typedef struct {
   unsigned short table[16];
   unsigned short trans[288];
} TINF_TREE;

struct uzlib_uncomp {
    const unsigned char *source;
    const unsigned char *source_limit;
    int (*source_read_cb)(struct uzlib_uncomp *uncomp);

    unsigned int tag;
    unsigned int bitcount;

    unsigned char *dest_start;
    unsigned char *dest;
    unsigned char *dest_limit;

    unsigned int checksum;
    char checksum_type;
    bool eof;

    int btype;
    int bfinal;
    unsigned int curlen;
    int lzOff;
    unsigned char *dict_ring;
    unsigned int dict_size;
    unsigned int dict_idx;

    TINF_TREE ltree;
    TINF_TREE dtree;
};

/* compat defines */
#define TINF_DATA struct uzlib_uncomp
#define destSize dest_size
#define destStart dest_start
#define readSource source_read_cb

#define TINF_PUT(d, c) \
    { \
        *d->dest++ = c; \
        if (d->dict_ring) { d->dict_ring[d->dict_idx++] = c; if (d->dict_idx == d->dict_size) d->dict_idx = 0; } \
    }

unsigned char TINFCC uzlib_get_byte(TINF_DATA *d);

void TINFCC uzlib_init(void);
void TINFCC uzlib_uncompress_init(TINF_DATA *d, void *dict, unsigned int dictLen);
int  TINFCC uzlib_uncompress(TINF_DATA *d);
int  TINFCC uzlib_uncompress_chksum(TINF_DATA *d);

int TINFCC uzlib_zlib_parse_header(TINF_DATA *d);
int TINFCC uzlib_gzip_parse_header(TINF_DATA *d);

uint32_t TINFCC uzlib_adler32(const void *data, unsigned int length, uint32_t prev_sum);
uint32_t TINFCC uzlib_crc32(const void *data, unsigned int length, uint32_t crc);

#ifdef __cplusplus
}
#endif

#endif /* UZLIB_H_INCLUDED */
