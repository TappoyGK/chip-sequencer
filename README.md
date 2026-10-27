# chip-sequencer

C11 sequencer and performance compiler. Depends only on chip-audio
and the C library. No application, OS audio device or Objective-C dependency.

`chip-performance` JSON describes resolved MIDI notes, source settings, expression,
tempo and finite repeat order. Compilation produces `chip-sequence` JSON with
absolute 44,100 Hz sample events and explicit once / first-pass / loop-cycle
visits. All fields are explicit; unknown fields, malformed values and unsupported
versions are rejected. No legacy format reader or migration fallback is included.

## Build

Install chip-audio first, then:

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/prefix \
  -DCMAKE_INSTALL_PREFIX=/path/to/prefix \
  -DCHIP_SEQUENCER_BUILD_TESTS=ON -DCHIP_SEQUENCER_BUILD_EXAMPLES=ON
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build
build/chip-render examples/sequence.json example.s16le
```

Consumers use `find_package(chip-sequencer CONFIG REQUIRED)` and link
`chip-sequencer::chip-sequencer`. Both headers are C++17-compatible. With sibling
source trees, `make CHIP_AUDIO_LIB=../chip-audio/build/libchip-audio.a test` works.

## Ownership and audio thread

1. Supply explicit `CALimits`; zero means zero capacity, never unlimited.
2. Outside the callback: load/compile a sequence and prepare a player.
3. On the mixer owner thread: `cs_play` transfers the player only on `CA_OK`.
4. Render or advance using the mixer; query `cs_position` on that same thread.
5. Prepare seek/edit tickets off-thread. Submit tickets on the owner thread,
   then poll the application result. Receipt alone does not imply execution.
6. Collect retirement tokens on the owner thread and dispose them off-thread.

A live edit keeps phase and the current repeat pass. Tempo/order changes require
a new player; `cs_prepare_edit` rejects them. A second edit stays busy until the
first result is polled. Queries expose both source and performance samples.
All loops seek to prepared checkpoints; the callback never reconstructs history.
The right endpoint of a render is settled at the next positive-length call.

Four synth sources may select pulse, triangle or noise; voice 4 is independent
DPCM. Pulse envelope/onset automation and paired echo use sources 0 and 1.
Voice gains are explicit and mixer gain automation uses output time even while
paused. Stable song IDs are caller-owned, not inferred from an array position.

`examples/render.c` is a complete standalone consumer. `tests/run-rt.py` checks
the entire DSP/mixer/sequencer with allocation/free guards and sanitizers;
`tests/fuzz.c` is a libFuzzer entrypoint, with `mutation-test.c` as a deterministic
runner when the toolchain lacks libFuzzer. Musical samples are synthetic.

## License

[MIT](LICENSE).
