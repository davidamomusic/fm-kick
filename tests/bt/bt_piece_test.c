/* SPDX-License-Identifier: GPL-3.0-only OR MPL-2.0 */
/* The pieces an audio L2CAP frame goes out in (bt/br/bt_piece.h, bt_page.c bp_tx_media): whole when the device lets the FM-1
 * more than one slot, in DH1 pieces of 27 bytes (an L2CAP start, then continuations) when it lets it one (the Galaxy Buds Pro's
 * LMP_max_slot 1, 2026-11-07), and the air that costs at the SBC rates the stack runs then. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../../bt/br/bt_piece.h"

static int fails;
static void check(const char *what, int ok)
{
    printf("bt_piece: %-100s %s\n", what, ok ? "ok" : "FAIL");
    fails += !ok;
}
static uint32_t frame_len(uint32_t bitpool)         /* bt_sbc_frame_len, joint stereo, 8 subbands, 16 blocks */
{
    return 12u + (8u + 16u * bitpool + 7u) / 8u;
}

int main(void)
{
    static const uint32_t WHOLE[] = {1, 17, 26, 27, 28, 54, 182, 183, 184, 242, 262, 332};
    static const uint32_t PEER[] = {0, 1, 3, 5};
    uint32_t w, pi, ok_all = 1, one_all = 1;
    for (pi = 0; pi < 4; pi++) {
        for (w = 0; w < sizeof WHOLE / sizeof WHOLE[0]; w++) {
            uint32_t off = 0, pieces = 0, total = 0, llid_ok = 1, type_ok = 1, cap_ok = 1;
            for (;;) {
                struct bp_piece p = bp_piece_next(WHOLE[w], off, PEER[pi]);
                uint32_t cap = p.type == 15 ? 339u : p.type == 11 ? 183u : 27u;
                llid_ok &= p.llid == (pieces ? 1u : 2u);
                type_ok &= p.type == 15u || p.type == 11u || p.type == 4u;
                cap_ok &= p.n >= 1u && p.n <= cap && (PEER[pi] != 1u || (p.n <= 27u && p.type == 4u));
                total += p.n;
                pieces++;
                if (p.done)
                    break;
                off = p.next;
                if (pieces > 100u)
                    break;
            }
            ok_all &= total == WHOLE[w] && llid_ok && type_ok && cap_ok && pieces <= 100u;
            if (PEER[pi] != 1u)
                one_all &= pieces == 1u;                       /* a device with more than one slot: the frame whole, as before */
            else
                one_all &= pieces == (WHOLE[w] + 26u) / 27u;   /* one slot: ceil(frame / 27) pieces */
        }
    }
    check("every frame length x every limit: the pieces add up to the frame, the first LLID 2 and the rest 1, sizes within their type", ok_all);
    check("limit 0 (not told), 3 and 5: one piece, the frame whole; limit 1: ceil(length / 27) pieces of DH1", one_all);
    {
        struct bp_piece p = bp_piece_next(242u, 0u, 1u);
        check("242 B at limit 1: first piece 27 B, DH1 (type 4), LLID 2", p.n == 27u && p.type == 4u && p.llid == 2u && !p.done);
        p = bp_piece_next(242u, 216u, 1u);
        check("... the last piece (offset 216) 26 B, LLID 1, done", p.n == 26u && p.llid == 1u && p.done);
        p = bp_piece_next(332u, 0u, 5u);
        check("332 B at limit 5: one DH5, LLID 2, done", p.n == 332u && p.type == 15u && p.llid == 2u && p.done);
        p = bp_piece_next(183u, 0u, 3u);
        check("183 B at limit 3: one DH3 (the most a DH3 holds), done", p.n == 183u && p.type == 11u && p.done);
        p = bp_piece_next(300u, 100u, 5u);
        check("a limit that rises between the pieces: the rest goes whole (200 B, DH5, LLID 1)", p.n == 200u && p.type == 15u && p.llid == 1u && p.done);
    }
    {   /* the air one slot costs: a DH1 is 27 B in 2 slots (the device's answer takes the second), 800 a second at the most */
        static const uint32_t BP[3] = {14, 12, 10};
        uint32_t t;
        for (t = 0; t < 3; t++) {
            uint32_t frames = 6u, bytes = 4u + 12u + 1u + frames * frame_len(BP[t]);   /* L2CAP + RTP + media header + frames */
            uint32_t pkts_s = 44100u * 1000u / (128u * frames);                       /* packets a second x 1000 */
            uint32_t pieces = (bytes + 26u) / 27u;
            uint32_t slots_s = pkts_s * pieces * 2u / 1000u;                           /* slots a second */
            char what[160];
            snprintf(what, sizeof what, "bitpool %u: 6 frames, %u B a packet in %u DH1, %u of the 1600 slots a second (%u %%), under 75 %%", BP[t], bytes, pieces, slots_s, slots_s * 100u / 1600u);
            check(what, bytes <= 332u && slots_s * 100u / 1600u < 75u);
        }
    }
    printf("bt_piece: %d failed\n", fails);
    return fails != 0;
}
