# Provenance

Copyright (c) 2026 tappoy. MIT license, as specified by the owner for the shared
audio implementation.

- The performance compiler extracts note/gate, grace, expression, TOM, triangle,
  drum and echo timing rules from Gakufu's own `src/playback.m` implementation.
- Tempo integration/inversion is extracted from Gakufu's `src/tempo.m`.
- The strict JSON reader, prepared sequence player, public interface, ownership
  adapters and tests are newly written for this library.
- The library depends on chip-audio; it does not contain copied external audio,
  game samples, third-party JSON code or production scores.
- Examples are short synthetic event sequences. User production music and the
  resulting PCM verification files are excluded from distribution.
