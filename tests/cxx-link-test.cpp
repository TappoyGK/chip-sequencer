#include "chip-sequencer.h"
#include "chip-tempo.h"
#include <cassert>
int main(){CALimits limits{};limits.allocated_bytes=1<<20;CAResult error{};CAMixer *m=ca_mixer_create(&limits,&error);assert(m&&error==CA_OK);assert(ca_mixer_advance(m,128)==CA_OK&&ca_mixer_position(m)==128);ca_mixer_destroy(m);assert(cs_tempo_seconds(96,120,0,96)==.5);assert(cs_sequence_plan_length(nullptr,0)==0);}
