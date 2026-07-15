# NOTES

The sender forwards every frame immediately and additionally sends one XOR-parity
packet per contiguous block of G=3 frames (166B protecting 3×165B of data, ≈1.38x
overhead), so a single lost frame per block is reconstructed from the parity plus
the other two frames instead of being retransmitted — retransmission was rejected
because both the request and the reply would each cross the same hostile relay,
costing more round-trip delay than it saves. The receiver never buffers for
reordering; it forwards each frame the instant it has it (directly or via FEC
recovery) since the harness player already enforces the deadline itself, so holding
frames locally would only add delay with no benefit. **Grade at `--delay_ms 120`**
(no env vars needed; `FEC_G` defaults to 3): this clears both given profiles with
comfortable margin (A: ~0–0.1% misses, B: ~0.5–0.7% misses, both ≤1% cap, 1.38x ≤2x
overhead cap). What breaks it: any block that suffers two or more real drops is
unrecoverable by a single XOR parity regardless of delay, so independent loss much
above ~8% or genuine burst loss (two-plus consecutive drops landing in the same
block) will push the miss rate over 1% — the fix we didn't have time for is
interleaving block membership across non-adjacent frames so a physical burst
spreads its damage across many blocks instead of concentrating it in one.
