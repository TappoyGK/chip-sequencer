#ifndef CHIP_SEQUENCER_H
#define CHIP_SEQUENCER_H
#include "chip-source.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct CSModel CSModel;
typedef struct CSSequence CSSequence;
typedef struct CSPlayer CSPlayer;
/* Per-call, caller-owned diagnostic. On success all fields are zero.
   offset is a zero-based byte offset; line/column are one-based byte positions.
   path is an RFC 6901 JSON Pointer (empty means the document root).
   For missing fields, path names the field and offset points at its container.
   Failures without an input location use
   offset=SIZE_MAX and line=column=0. Fixed strings are NUL-terminated;
   truncated is nonzero if a path or message did not fit. */
typedef struct {
    CAResult code;
    size_t offset, line, column;
    char path[512], message[256];
    int truncated;
} CSDiagnostic;
typedef enum { CS_ONCE, CS_LOOP } CSMode;
typedef struct {
    int64_t output_sample, performance_sample, source_sample;
    double source_tick;
    uint64_t loop_count, revision;
    uint32_t visit_index;
    CAState state;
} CSPosition;
CSModel *cs_model_read(const char *utf8, size_t bytes, const CALimits *, CAResult *);
void cs_model_destroy(CSModel *);
CSSequence *cs_compile(const CSModel *, const CALimits *, CAResult *);
uint64_t cs_sequence_revision(const CSSequence *);
CSSequence *cs_sequence_read(const char *utf8, size_t bytes, const CALimits *, CAResult *);
/* Same operations with optional detailed diagnostics; no global error state. */
CSModel *cs_model_read_ex(const char *, size_t, const CALimits *, CAResult *, CSDiagnostic *);
CSSequence *cs_sequence_read_ex(const char *, size_t, const CALimits *, CAResult *, CSDiagnostic *);
CSSequence *cs_compile_ex(const CSModel *, const CALimits *, CAResult *, CSDiagnostic *);
CAResult cs_sequence_write(const CSSequence *, char *buffer, size_t capacity, size_t *required);
CSSequence *cs_sequence_retain(const CSSequence *);
void cs_sequence_destroy(CSSequence *);
// Query immutable prepared plans: 0=once, 1=first pass, 2=loop cycle.
int64_t cs_sequence_plan_length(const CSSequence *, unsigned plan);
CAResult cs_sequence_locate(const CSSequence *, CSMode, int64_t source_sample,
                            uint32_t *visit, int64_t *offset);
CAResult cs_sequence_at(const CSSequence *, CSMode, int64_t performance_sample,
                        int64_t *source_sample, int64_t *remaining);
CSPlayer *cs_player_prepare(const CSSequence *, CSMode, const CALimits *, CAResult *);
void cs_player_destroy(CSPlayer *); /* caller-owned states only */
CAResult cs_play(CAMixer *, CSPlayer *, int64_t at, CAHandle *);
CAResult cs_change(CAMixer *, CSPlayer *, int64_t fade, int64_t at, CAHandle *);
CAResult cs_stop(CAMixer *, CAHandle, int64_t fade, int64_t at);
CAResult cs_pause(CAMixer *, CAHandle, int64_t fade, int64_t at);
CAResult cs_resume(CAMixer *, CAHandle, int64_t fade, int64_t at);
CAResult cs_position(const CAMixer *, CAHandle, CSPosition *);
/* Seek ticket binds to immutable sequence revision, mode and visit, not tick alone. */
CASeek *cs_prepare_seek(const CSSequence *, CSMode, uint32_t visit,
                        int64_t offset, uint64_t loop_count, const CALimits *, CAResult *);
void ca_seek_destroy(CASeek *);
CAResult cs_seek(CAMixer *, CAHandle, CASeek *, int64_t at, CACommandID *);
/* Live score replacement retains synth phase/compatible echo and current pass.
   Preparation is position-independent, using immutable old/new sequences.
   Apply inherits the then-current position/pass/phase; no snapshot seek.
   At most ONE accepted edit per player until its result is polled.
   Revision checked both at acceptance and execution; results are always reported.
   Structural tempo/order edits remain stop/restart in the first migration. */
typedef struct CSEdit CSEdit;
CSEdit *cs_prepare_edit(const CSSequence *old_sequence, const CSSequence *new_sequence,
                        const CALimits *, CAResult *);
void cs_edit_destroy(CSEdit *);
CAResult cs_apply_edit(CAMixer *, CAHandle, CSEdit *, int64_t at, CACommandID *);
#ifdef __cplusplus
}
#endif
#endif
