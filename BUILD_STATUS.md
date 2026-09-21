# Fox build/flash package status

This package fixes the 8MB flash map and the generated-data mismatch that made the previous package invalid.

## Fixed

- `fox.bin` has a 0x360000 factory partition.
- PicoTTS EN-US resources are stored in dedicated `picotts_ta` (0xA0000) and `picotts_sg` (0xC0000) partitions instead of consuming the factory app partition.
- `foxbrain` is 0x40000 and Brain B was resized to a 253,753-byte compact transformer that still exceeds Brain A's 216,377-byte pack.
- `foxdata` is 0x32000, large enough for the current 196,857-byte IR library.
- `foxfs` is 0x4000 for the journal.
- The final partition end is 0x7F6000, leaving 0xA000 bytes inside the 8MB flash boundary.
- CI validates partition overlap, 8MB bounds, generated brain sizes, IR size, and Pico resource sizes before publishing flash manifests.
- When PicoTTS is available, CI includes its TA/SG binaries in both web-flash manifests. If the pinned PicoTTS download is unavailable, the build remains SAM-capable.

## Verification performed in this source package

- `tests/test_build_artifacts.py` — PASS
- `tests/test_app_wiring.py` — PASS
- Workflow YAML parse — PASS
- Partition overlap/8MB validation — PASS
- Brain A/B serialized-size calculation — A 216,377 bytes; B 253,753 bytes
- IR serialized-size calculation — 196,857 bytes

A physical AtomS3R flash/audio/display test cannot be performed in this build environment; the GitHub Actions workflow is the authoritative ESP-IDF 5.5.2 build and flash-manifest path.
