/* RECEIVER — XOR-FEC recovery + immediate forward (no reorder buffering).
 *
 * Ports (all 127.0.0.1):
 *   bind 47002  <- media from our sender, via the hostile relay (our wire
 *                  format: see sender.c)
 *   send 47020  -> harness player. MUST be: 4-byte big-endian seq +
 *                  160-byte payload. Frame i counts only if it arrives
 *                  before its deadline t0 + DELAY_MS + i*20ms.
 *   47003 (feedback-out) is left unused, matching the sender's decision
 *   not to do retransmission.
 *
 * There is no deliberate jitter buffer here: the harness player already
 * enforces the deadline itself (first arrival wins, late arrival = miss),
 * so the only thing worth doing on our side is (a) forward every frame the
 * instant we have it and (b) reconstruct any missing frame from its FEC
 * block as soon as that becomes possible (i.e. as soon as the block's
 * parity AND all-but-one of its data members have arrived). That
 * reconstruction latency — up to (G-1) frame periods after the lost frame,
 * since the parity is only sent once the block's last frame is sent — is
 * the only "buffering" delay this design adds, and it is a byproduct of the
 * FEC group size, not an explicit hold-and-wait.
 *
 * Arrays are sized from DURATION_S (env var) so we can hold the whole
 * run's worth of sequence numbers for block bookkeeping.
 */
#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PAYLOAD_LEN 160
#define PLAYER_PKT_LEN (4 + PAYLOAD_LEN)
#define MAX_G 32

static int group_size_from_env(void) {
    const char *e = getenv("FEC_G");
    int g = e ? atoi(e) : 3;   /* default group size: tuned in RUNLOG.md */
    if (g < 1) g = 1;
    if (g > MAX_G) g = MAX_G;
    return g;
}

static uint32_t n_frames_from_env(void) {
    const char *d = getenv("DURATION_S");
    double dur = d ? atof(d) : 60.0;
    uint32_t n = (uint32_t)(dur * 1000.0 / 20.0) + 64; /* pad for safety */
    if (n < 256) n = 256;
    return n;
}

int main(void) {
    int G = group_size_from_env();
    uint32_t N = n_frames_from_env();
    uint32_t n_groups = N / (uint32_t)G + 2;

    unsigned char *payload = calloc((size_t)N, PAYLOAD_LEN);
    unsigned char *have = calloc(N, 1);
    unsigned char *delivered = calloc(N, 1);
    unsigned char *parity = calloc((size_t)n_groups, PAYLOAD_LEN);
    unsigned char *parity_have = calloc(n_groups, 1);
    if (!payload || !have || !delivered || !parity || !parity_have) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in in_addr = {0};
    in_addr.sin_family = AF_INET;
    in_addr.sin_port = htons(47002);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(in_fd, (struct sockaddr *)&in_addr, sizeof in_addr) < 0) {
        perror("bind 47002");
        return 1;
    }

    int out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in player = {0};
    player.sin_family = AF_INET;
    player.sin_port = htons(47020);
    player.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(out_fd, (struct sockaddr *)&player, sizeof player) < 0) {
        perror("connect player 47020");
        return 1;
    }

    unsigned char in_buf[2048];
    unsigned char out_buf[PLAYER_PKT_LEN];

    for (;;) {
        ssize_t n = recvfrom(in_fd, in_buf, sizeof in_buf, 0, NULL, NULL);
        if (n < 1) continue;
        unsigned char type = in_buf[0];

        if (type == 1) {
            if (n < 5 + PAYLOAD_LEN) continue;
            uint32_t seq_net;
            memcpy(&seq_net, in_buf + 1, 4);
            uint32_t seq = ntohl(seq_net);
            if (seq >= N) continue; /* beyond our sized window; drop safely */

            if (!have[seq]) {
                memcpy(payload + (size_t)seq * PAYLOAD_LEN, in_buf + 5, PAYLOAD_LEN);
                have[seq] = 1;
            }
            if (!delivered[seq]) {
                uint32_t seq_be = htonl(seq);
                memcpy(out_buf, &seq_be, 4);
                memcpy(out_buf + 4, payload + (size_t)seq * PAYLOAD_LEN, PAYLOAD_LEN);
                send(out_fd, out_buf, PLAYER_PKT_LEN, 0);
                delivered[seq] = 1;
            }

            uint32_t gid = seq / (uint32_t)G;
            uint32_t base = gid * (uint32_t)G;
            if (parity_have[gid]) {
                int missing = -1, miss_count = 0;
                for (int k = 0; k < G; k++) {
                    uint32_t s = base + (uint32_t)k;
                    if (s < N && !have[s]) { missing = (int)s; miss_count++; }
                }
                if (miss_count == 1) {
                    unsigned char rec[PAYLOAD_LEN];
                    memcpy(rec, parity + (size_t)gid * PAYLOAD_LEN, PAYLOAD_LEN);
                    for (int k = 0; k < G; k++) {
                        uint32_t s = base + (uint32_t)k;
                        if (s < N && (int)s != missing)
                            for (int b = 0; b < PAYLOAD_LEN; b++)
                                rec[b] ^= payload[(size_t)s * PAYLOAD_LEN + b];
                    }
                    memcpy(payload + (size_t)missing * PAYLOAD_LEN, rec, PAYLOAD_LEN);
                    have[missing] = 1;
                    if (!delivered[missing]) {
                        uint32_t seq_be = htonl((uint32_t)missing);
                        memcpy(out_buf, &seq_be, 4);
                        memcpy(out_buf + 4, rec, PAYLOAD_LEN);
                        send(out_fd, out_buf, PLAYER_PKT_LEN, 0);
                        delivered[missing] = 1;
                    }
                }
            }
        } else if (type == 2) {
            if (n < 6 + PAYLOAD_LEN) continue;
            uint32_t base_net;
            memcpy(&base_net, in_buf + 1, 4);
            uint32_t base = ntohl(base_net);
            int gsize = in_buf[5];
            uint32_t gid = base / (uint32_t)G;
            if (gid >= n_groups) continue;

            if (!parity_have[gid]) {
                memcpy(parity + (size_t)gid * PAYLOAD_LEN, in_buf + 6, PAYLOAD_LEN);
                parity_have[gid] = 1;
            }

            int missing = -1, miss_count = 0;
            for (int k = 0; k < gsize; k++) {
                uint32_t s = base + (uint32_t)k;
                if (s < N && !have[s]) { missing = (int)s; miss_count++; }
            }
            if (miss_count == 1) {
                unsigned char rec[PAYLOAD_LEN];
                memcpy(rec, parity + (size_t)gid * PAYLOAD_LEN, PAYLOAD_LEN);
                for (int k = 0; k < gsize; k++) {
                    uint32_t s = base + (uint32_t)k;
                    if (s < N && (int)s != missing)
                        for (int b = 0; b < PAYLOAD_LEN; b++)
                            rec[b] ^= payload[(size_t)s * PAYLOAD_LEN + b];
                }
                memcpy(payload + (size_t)missing * PAYLOAD_LEN, rec, PAYLOAD_LEN);
                have[missing] = 1;
                if (!delivered[missing]) {
                    uint32_t seq_be = htonl((uint32_t)missing);
                    memcpy(out_buf, &seq_be, 4);
                    memcpy(out_buf + 4, rec, PAYLOAD_LEN);
                    send(out_fd, out_buf, PLAYER_PKT_LEN, 0);
                    delivered[missing] = 1;
                }
            }
        }
    }
    return 0;
}
