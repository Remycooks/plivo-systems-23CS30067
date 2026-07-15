/* SENDER — forward-only XOR FEC (no retransmission, no feedback).
 *
 * Ports (all 127.0.0.1):
 *   bind 47010  <- harness source delivers frame i at t0 + i*20ms
 *                  (harness format: 4-byte big-endian seq + 160-byte payload)
 *   send 47001  -> relay uplink toward the receiver (OUR wire format below)
 *   47004 (feedback-in) is left unused: a NACK/resend round trip costs two
 *   hostile-network crossings, which at these delays is worse than simply
 *   sending a little extra data ahead of time. We spend the overhead budget
 *   on forward error correction instead.
 *
 * Wire format sender->receiver (our own design):
 *   DATA   : u8 type=1 | u32be seq                      | 160B payload   (165B)
 *   PARITY : u8 type=2 | u32be base_seq | u8 group_size  | 160B XOR       (166B)
 *
 * FEC scheme: frames are grouped contiguously into blocks of G (env FEC_G,
 * default 4). After the G-th frame of a block is sent, we transmit one
 * parity packet = XOR of the G payloads in that block. Any single lost
 * frame in a block is recoverable from the other G-1 frames + the parity,
 * at a bandwidth cost of only 1/G extra (vs. 1/1 for full duplication).
 * Smaller G -> lower overhead but only tolerates less loss per block, and
 * a real physical burst of >=2 drops inside the same block is unrecoverable
 * regardless of G (documented in NOTES.md).
 *
 * Env vars: T0, DURATION_S, DELAY_MS (informational only for this design,
 * since we forward every frame the instant it's available and never delay
 * on purpose). FEC_G overrides the group size for experiments.
 */
#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PAYLOAD_LEN 160
#define HARNESS_PKT_LEN (4 + PAYLOAD_LEN)
#define MAX_G 32

static int default_group_size(void) {
    const char *e = getenv("FEC_G");
    int g = e ? atoi(e) : 3;   /* default group size: tuned in RUNLOG.md */
    if (g < 1) g = 1;
    if (g > MAX_G) g = MAX_G;
    return g;
}

int main(void) {
    int G = default_group_size();

    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in in_addr = {0};
    in_addr.sin_family = AF_INET;
    in_addr.sin_port = htons(47010);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(in_fd, (struct sockaddr *)&in_addr, sizeof in_addr) < 0) {
        perror("bind 47010");
        return 1;
    }

    int out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in relay = {0};
    relay.sin_family = AF_INET;
    relay.sin_port = htons(47001);
    relay.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(out_fd, (struct sockaddr *)&relay, sizeof relay) < 0) {
        perror("connect relay 47001");
        return 1;
    }

    unsigned char group_buf[MAX_G][PAYLOAD_LEN];
    unsigned char in_buf[2048];
    unsigned char out_buf[8 + PAYLOAD_LEN];

    for (;;) {
        ssize_t n = recvfrom(in_fd, in_buf, sizeof in_buf, 0, NULL, NULL);
        if (n < HARNESS_PKT_LEN) continue;

        uint32_t seq_be;
        memcpy(&seq_be, in_buf, 4);
        uint32_t seq = ntohl(seq_be);
        const unsigned char *payload = in_buf + 4;

        int slot = (int)(seq % (uint32_t)G);
        memcpy(group_buf[slot], payload, PAYLOAD_LEN);

        /* DATA packet: type(1) + seq(4) + payload(160) */
        out_buf[0] = 1;
        uint32_t seq_net = htonl(seq);
        memcpy(out_buf + 1, &seq_net, 4);
        memcpy(out_buf + 5, payload, PAYLOAD_LEN);
        send(out_fd, out_buf, 5 + PAYLOAD_LEN, 0);

        if (slot == G - 1) {
            /* block complete: emit parity = XOR of the G payloads */
            unsigned char parity[PAYLOAD_LEN];
            memcpy(parity, group_buf[0], PAYLOAD_LEN);
            for (int k = 1; k < G; k++)
                for (int b = 0; b < PAYLOAD_LEN; b++)
                    parity[b] ^= group_buf[k][b];

            uint32_t base_seq = seq - (uint32_t)(G - 1);
            out_buf[0] = 2;
            uint32_t base_net = htonl(base_seq);
            memcpy(out_buf + 1, &base_net, 4);
            out_buf[5] = (unsigned char)G;
            memcpy(out_buf + 6, parity, PAYLOAD_LEN);
            send(out_fd, out_buf, 6 + PAYLOAD_LEN, 0);
        }
    }
    return 0;
}
