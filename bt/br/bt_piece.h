/* SPDX-License-Identifier: GPL-3.0-only OR MPL-2.0 */
/* One baseband packet of an audio L2CAP frame (bt_page.c bp_tx_media), apart from the hardware so a host test can run it:
 * a device that lets the FM-1 one slot (its LMP_max_slot 1: the Galaxy Buds Pro) takes the frame in DH1 pieces of 27 bytes,
 * an L2CAP start (LLID 2) and then continuations (LLID 1); any other device takes it whole, as always. */
#ifndef BT_PIECE_H
#define BT_PIECE_H
#include <stdint.h>
struct bp_piece {
    uint32_t n;                                    /* the bytes of this piece */
    uint32_t llid;                                 /* 2: the start of the L2CAP frame, 1: a continuation */
    uint32_t type;                                 /* the baseband packet type: 15 DH5, 11 DH3, 4 DH1 (BR §5.2) */
    uint32_t next;                                 /* the offset of the next piece */
    uint32_t done;                                 /* this piece ends the frame */
};
static inline struct bp_piece bp_piece_next(uint32_t whole, uint32_t off, uint32_t peer_slots)
{
    struct bp_piece p;
    p.n = whole - off;
    if (peer_slots == 1u && p.n > 27u)
        p.n = 27u;
    p.llid = off ? 1u : 2u;
    p.type = p.n > 183u ? 15u : p.n > 27u ? 11u : 4u;
    p.next = off + p.n;
    p.done = p.next >= whole;
    return p;
}
#endif
