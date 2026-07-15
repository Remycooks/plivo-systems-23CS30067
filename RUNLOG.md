# RUNLOG — The Flaky Network

All runs: `make clean && make && python3 run.py --profile profiles/<X>.json --delay_ms <D> --duration <T>`
Bandwidth overhead below is `(up_bytes + down_bytes) / raw`, raw = n_frames * 160B, reported by score.py.
We never use the feedback path (47003/47004), so down_bytes = 0 in every run.

## 0. Baseline (unmodified naive sender/receiver, forward-once, no recovery)

```
make && python3 run.py --profile profiles/A.json --delay_ms 40
```
Result: **INVALID**. Every dropped packet is a permanent glitch (the baseline has no
mechanism to survive a drop at all), so miss rate tracks the raw loss rate of the
profile (~2% on A) — already over the 1% cap regardless of delay_ms. Overhead is
1.00x (forwards each frame exactly once). This confirms the two only remedies for a
lost frame at a fixed 20ms cadence: (a) get another copy of it to the receiver before
its deadline (retransmission or redundancy sent ahead of time), or (b) reconstruct it
from other data (parity / FEC). A resend is unattractive here because the request
*and* the reply both cross the hostile relay — at these delay/jitter profiles that
round trip alone can burn 60–160ms before the resent frame even leaves the sender,
which is more added delay than we want to spend, and the resend itself can be
dropped too. So we chose (b), sent proactively, never triggered by feedback.

## 1. Design implemented

Contiguous-block XOR FEC, forward-only, no feedback:
- Sender groups frames into blocks of **G** consecutive frames (env `FEC_G`,
  compiled-in default **2**, see §5–6 for why). Every frame is forwarded
  immediately, unchanged. After the G-th frame of a block is sent, the sender
  also sends one parity packet = XOR of the G payloads in that block.
- Receiver forwards every arriving data frame to the harness player
  immediately (no artificial hold/reorder — the harness player already
  scores "first arrival before deadline", so buffering on our side would only
  add delay for no benefit). Whenever a data or parity packet arrives, the
  receiver checks whether its block now has exactly one missing member; if
  so, it XORs the parity with the G-1 present frames to reconstruct the
  missing one and forwards it immediately.
- Bandwidth cost: 1 parity packet (166B) per G data packets (165B each), i.e.
  overhead ≈ (165G + 166) / 160G. This is far cheaper than full duplication
  (which would need ~2x by itself, no headroom left for anything else) because
  one parity packet protects a whole group, not just one frame.

## 2. Tuning G (group size) — profile B (loss 5%, delay 20–80ms, dup 1%), duration 20s

*(This section and §3–4 document the first tuning pass, which settled on G=3 /
delay_ms=120. §5–6 push further to the final G=2 / delay_ms=110 answer — kept
here so the reasoning trail and the "why not just jump to G=2" comparison is visible.)*

| G | overhead | delay_ms=140 miss% | notes |
|---|----------|---------------------|-------|
| 4 | 1.29x    | 0.9–1.1% (borderline, sometimes over 1%) | recovers block iff exactly 1 of 4 lost: P(miss)=p(1-(1-p)^4)≈0.93% at p=5% — right at the cap, no margin |
| 3 | 1.38x    | 0.5–0.7% (stable)   | P(miss)=p(1-(1-p)^3)≈0.71% at p=5% — comfortable margin |
| 2 | 1.55x    | (not needed, G=3 already safe) | more overhead for little extra safety once G=3 already clears the cap with margin |

We picked **G=3**: predicted and measured miss rate for independent random loss
sits well under 1% on both given profiles, overhead (1.38x) leaves plenty of
room under the 2.0x cap, and a smaller group also means shorter FEC recovery
latency (a lost frame can only be reconstructed once its block's last member
and parity have gone out, i.e. up to (G-1)*20ms = 40ms after the loss).

## 3. Shrinking delay_ms — profile A (loss 2%, delay 10–40ms), G=3, duration 20–30s

| delay_ms | miss% | overhead | result |
|----------|-------|----------|--------|
| 60  | invalid (arrives too late even with recovery) | 1.38x | INVALID |
| 70  | 0.5–0.7% | 1.38x | VALID |
| 80  | 0.0–0.1% | 1.38x | VALID |
| 90  | 0.0–0.1% | 1.38x | VALID |
| 120 | 0.0–0.13% | 1.38x | VALID (stable across repeats) |

## 4. Shrinking delay_ms — profile B (loss 5%, delay 20–80ms), G=3, duration 20–30s

| delay_ms | miss% | overhead | result |
|----------|-------|----------|--------|
| 90  | invalid | 1.38x | INVALID |
| 100 | invalid | 1.38x | INVALID |
| 105 | invalid | 1.38x | INVALID |
| 110 | 1.00% (right at the cap, ran INVALID once out of several tries) | 1.38x | borderline |
| 120 | 0.5–0.73% (stable over 5+ repeats) | 1.38x | VALID |
| 140 | 0.5–1.1% (occasionally tipped over 1% due to scheduling jitter) | 1.38x | mostly VALID but less margin than 120! |
| 160/180 | plateaus ~0.9–1.0% | 1.38x | miss floor is now FEC-limited (double-loss-in-block), not timing-limited |

Note the non-monotonic result at 140 vs 120: past a certain point, extra
delay stops helping because remaining misses are frames whose *block* had
2+ real drops (unrecoverable by a single XOR parity, regardless of how long
we wait), not frames that were merely late. That floor for G=3 at p=5% is
≈0.7%, matching the (1-(1-p)^G) recovery model above. **120ms sits closest to
that floor with a comfortable margin below the 1% cap and was the most
stable point across repeated runs**, so it is our chosen operating point
rather than the higher delay values that show more run-to-run variance.

## 5. Re-tuning for a lower floor: G=2

G=3 at delay_ms=120 was safely valid but left an obvious question: how low can
delay_ms actually go? Smaller blocks recover faster (recovery latency is
`(G-1)*20ms`, so G=2 only adds 20ms vs. G=3's 40ms) at the cost of higher
overhead per byte (1.55x vs 1.38x — still far under the 2.0x cap, so the extra
overhead is free to spend). Re-swept with G=2:

| profile | delay_ms | miss% (multiple 20s/30s reps) | overhead | result |
|---------|----------|-------------------------------|----------|--------|
| A | 45–55 | 0.7–2.2% (unstable, several INVALID) | 1.55x | unreliable |
| A | 60     | 0.1–0.5% (stable, 6 reps)      | 1.55x | VALID |
| A | 100–110 | 0.1–0.2%                      | 1.55x | VALID (large margin) |
| B | 90     | 0.9–1.1% (borderline, 1 INVALID in 4 reps) | 1.55x | unreliable |
| B | 95–100 | 0.6–0.93% (stable at 20s, thinner margin at 30s) | 1.55x | VALID |
| B | 110    | 0.8% consistently (3 reps, 30s each)          | 1.55x | VALID, most consistent |

At delay_ms=100, profile B crept up to 0.93% on longer (30s) runs — still
under the cap but with less margin than we want for profiles we haven't
seen. delay_ms=110 gave the same ~0.8% consistently across repeats at 30s,
so it's the safer floor rather than 100.

## 6. Final chosen operating point

**delay_ms = 110, FEC_G = 2 (compiled-in default, no env var needed)**

This is 10ms lower than the previous G=3/120ms operating point, confirmed
over multiple repeated runs (20s and 30s durations) on both given profiles:

| profile | miss% | overhead | result |
|---------|-------|----------|--------|
| A (mild)     | 0.10–0.20% | 1.55x | VALID |
| B (moderate) | 0.80–0.93% | 1.55x | VALID |

## 7. Stress tests beyond the given profiles (to understand failure modes for
   profiles we have not seen — not part of the graded profiles, informal only)

- **Higher independent loss (8%)**: miss rate plateaus ~2% regardless of
  delay_ms — confirms the failure mode is FEC block capacity, not timing.
- **Burst loss (Gilbert-Elliott, ~90% loss while "in burst")**: miss rate
  jumped to ~6.7% at delay_ms=120 (tested with the earlier G=3 config; the
  same limitation applies to the final G=2 config, since any block size ≥2
  fails once 2+ consecutive frames in the same block are genuinely lost).
  Contiguous-block XOR only recovers 1 loss per block; a real burst that hits
  2+ consecutive frames in the same block is unrecoverable no matter how much
  delay budget is spent. This is the design's known weak point (see
  NOTES.md) — an interleaved FEC scheme (grouping physically-adjacent frames
  into *different* blocks) would trade some recovery latency for real burst
  tolerance, and was the next thing we'd build with more time.

## 8. What we did NOT do, and why

- **No retransmission / NACK path.** A request and its reply both cross the
  hostile relay, so the achievable round trip is at least 2x the relay's
  per-hop delay (up to ~160ms one-way on profile B), often longer than just
  budgeting extra delay_ms for FEC recovery, and the retransmitted packet can
  itself be dropped. FEC sent ahead of time avoids this entirely.
- **No jitter/reorder buffer on the receiver.** The harness player already
  enforces "first correct arrival before deadline"; holding frames locally
  before forwarding would only add delay without changing which frames make
  their deadline, since we don't need to reorder anything — recovered frames
  are simply forwarded the moment they become available.
