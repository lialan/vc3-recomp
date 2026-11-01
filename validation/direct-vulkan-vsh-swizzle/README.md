# Rejected AArch64 vertex operand table swizzles

Replaced the four scalar source lane gathers in the shared vertex interpreter with vqtbl1q_u8, using a shared 4 KiB table of all 256 swizzle byte selectors. Non-AArch64 retained scalar code; negation, operand selection, opcodes and writes were unchanged. No game-specific specialization or new program-cache state.

Differential checks passed against a separately compiled snapshot of the original interpreter: 25,600 generated paired-opcode programs (including writable constants), 12,288 partial-word program updates, and exhaustive 256 swizzles with both negate states and MAC/ILU MOV. Special inputs included signed zero, subnormal, finite extrema, infinity and NaN payloads. Normal and ASan/UBSan builds passed. Test artifacts: /tmp/vsh-swizzle-differential.c, /tmp/vsh-swizzle-reference.c, /tmp/vsh-ref.h; binaries /tmp/vsh-swizzle-test and /tmp/vsh-swizzle-sanitized. macOS executable built; no Android or GPU fixture runs for this rejected CPU-only candidate.

## End-to-end result

Apple M4 Pro / KosmicKrisp; mono headless, sound off, 60 FPS cap, identical warm starting pipeline cache copies. Candidate then control, two immutable binaries. Both completed 2100 frames successfully.

| Variant | Activation FPS | Early held FPS | Later held FPS | Held p95 ms |
|---|---:|---:|---:|---|
| Scalar control | 43.33 | 54.11 | 57.01 | 23.03 / 21.27 |
| Table swizzles | 42.63 | 53.89 | 54.09 | 24.21 / 24.27 |

No end-to-end benefit; later held throughput and pacing regress. Restored the original interpreter and rebuilt the retained executable. Patch /tmp/vc3-vsh-swizzle-rejected.patch. This does not prove table lookup is intrinsically slower; the interpreter's combined code and data costs matter, and this implementation does not justify retention. No headset use. Raw /tmp/vc3-vsh-swizzle-{candidate,control}/report.json; full provenance in measurements.json.
