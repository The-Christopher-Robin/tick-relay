#include "feed.h"

#include <string.h>

uint32_t feed_checksum(const feed_msg_t *m) {
    /* Fold the first 56 bytes (everything before the checksum field)
     * into a single 32-bit word. Straightforward XOR over uint32_t
     * chunks is plenty for catching framing/truncation bugs in tests. */
    const uint8_t *p = (const uint8_t *)m;
    uint32_t acc = 0;
    for (size_t i = 0; i < 56; i += 4) {
        uint32_t word;
        memcpy(&word, p + i, sizeof(word));
        acc ^= word;
    }
    /* Mix in a constant so an all-zero frame doesn't checksum to 0. */
    acc ^= 0x9E3779B9u;
    return acc;
}

int feed_validate(const feed_msg_t *m) {
    if (m->magic != FEED_MAGIC) return -1;
    switch (m->msg_type) {
        case FEED_MSG_TRADE:
        case FEED_MSG_QUOTE:
        case FEED_MSG_HEARTBEAT:
            break;
        default:
            return -1;
    }
    if (m->checksum != feed_checksum(m)) return -1;
    return 0;
}
