#include "chip-sequencer.h"
#include <stddef.h>
#include <stdint.h>
int LLVMFuzzerTestOneInput(const uint8_t *data,size_t size){
    CALimits l={.input_bytes=65536,.allocated_bytes=8<<20,.events=2048,.visits=1024,.checkpoints=8,.repeat_depth=8,.score_samples=44100*10,.performance_samples=44100*60};CAResult error;CSDiagnostic diagnostic;
    CSSequence *s=cs_sequence_read_ex((const char *)data,size,&l,&error,&diagnostic);cs_sequence_destroy(s);
    CSModel *m=cs_model_read_ex((const char *)data,size,&l,&error,&diagnostic);cs_model_destroy(m);return 0;
}
