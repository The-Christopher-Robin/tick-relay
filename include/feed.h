#ifndef TICKRELAY_FEED_H
#define TICKRELAY_FEED_H

#include <stdint.h>

/*
 * Wire format for a single feed frame. Fixed 64 bytes so we can use
 * fixed-stride I/O and keep one message per cache line.
 *
 * All fields are little-endian. Producers (replay driver, real feeds)
 * leave ingress_tsc and egress_tsc at zero; the server fills them in.
 */

#define FEED_MAGIC      0xABCD1234u
#define FEED_MSG_SIZE   64

enum feed_msg_type {
    FEED_MSG_TRADE     = 1,
    FEED_MSG_QUOTE     = 2,
    FEED_MSG_HEARTBEAT = 3,
};

enum feed_side {
    FEED_SIDE_BID = 0,
    FEED_SIDE_ASK = 1,
};

struct feed_msg {
    uint32_t magic;           /*  0  */
    uint8_t  msg_type;        /*  4  */
    uint8_t  side;            /*  5  */
    uint16_t reserved0;       /*  6  */
    uint32_t symbol_id;       /*  8  */
    uint32_t qty;             /* 12  */
    uint64_t seq;             /* 16  */
    uint64_t price_cents;     /* 24  */
    uint64_t exchange_ts_ns;  /* 32  */
    uint64_t ingress_tsc;     /* 40  server fills */
    uint64_t egress_tsc;      /* 48  worker fills */
    uint32_t checksum;        /* 56  */
    uint32_t flags;           /* 60  */
} __attribute__((aligned(64)));

typedef struct feed_msg feed_msg_t;

_Static_assert(sizeof(feed_msg_t) == FEED_MSG_SIZE,
               "feed_msg must be exactly 64 bytes on the wire");

/* Simple xor-fold checksum over the first 56 bytes (everything up to but
 * not including the checksum field). Cheap enough to run on the hot path
 * and good enough to catch framing bugs in the replay driver. */
uint32_t feed_checksum(const feed_msg_t *m);

/* Validates magic, type, size invariants. Returns 0 on ok, -1 on bad frame. */
int feed_validate(const feed_msg_t *m);

#endif /* TICKRELAY_FEED_H */
