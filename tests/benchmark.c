#include "chip-sequencer.h"
#include "chip-audio.h"
#include <assert.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
static double now(void){struct timespec t;timespec_get(&t,TIME_UTC);return t.tv_sec+t.tv_nsec/1e9;}
int main(int argc,char **argv){
    CALimits l={.input_bytes=16<<20,.events=1000000,.visits=100000,.checkpoints=128,.repeat_depth=16,.score_samples=INT64_C(44100)*86400,.performance_samples=INT64_C(44100)*86400,.allocated_bytes=256<<20,.se_instances=17,.commands=256,.retired_objects=256,.command_results=256,.gain_bindings=256};CAResult result;
    ChipAudioNote note={0,441000,60,15,0,0};ChipAudioTrack tracks[4];for(int i=0;i<4;i++)tracks[i]=(ChipAudioTrack){CHIP_PULSE,&note,1};
    ChipAudio *a=chip_audio_create(tracks,441000,NULL);assert(a);ChipDPCMHit hit={0,441000,0,15};assert(chip_audio_set_dpcm_kicks(a,&hit,1));
    CASeDefinition *definition=ca_se_prepare(a,&l,&result);assert(definition);CAMixer *m=ca_mixer_create(&l,&result);assert(m);
    CAHandle handles[17];for(int i=0;i<17;i++){CASeInstance *s=ca_se_instantiate(definition,&result);assert(s&&ca_se_trigger(m,s,(CARoute){CA_SE,0},0,&handles[i])==CA_OK);}
    for(int i=0;i<17;i++)for(int v=0;v<5;v++)assert(ca_gate_ramp(m,handles[i],v,128)==CA_OK);
    CSSequence *sequence=NULL;CSPlayer *replacement=NULL;CAHandle bgm;
    if(argc>1){FILE *f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);long bytes=ftell(f);assert(bytes>0&&(size_t)bytes<=l.input_bytes);rewind(f);char *data=malloc((size_t)bytes);assert(data&&fread(data,1,(size_t)bytes,f)==(size_t)bytes);fclose(f);sequence=cs_sequence_read(data,(size_t)bytes,&l,&result);free(data);assert(sequence);
        CSPlayer *player=cs_player_prepare(sequence,CS_LOOP,&l,&result);replacement=cs_player_prepare(sequence,CS_LOOP,&l,&result);assert(player&&replacement&&cs_play(m,player,0,&bgm)==CA_OK);}
    assert(ca_mixer_advance(m,1)==CA_OK);
    if(replacement)assert(cs_change(m,replacement,44100,137,&bgm)==CA_OK);
    for(int i=0;i<17;i++)for(int j=0;j<15;j++){CAGainBinding b={handles[i],handles[(i+j+1)%17],(uint32_t)(j%5),.5,1};CAHandle binding;assert(ca_bind_gain(m,&b,1,&binding)==CA_OK);}
    double sum=0,max=0;int16_t pcm[128];CAOutput out={.pcm=pcm};
    for(int i=0;i<2000;i++){double start=now();assert(ca_mixer_render(m,128,&out)==CA_OK);double t=now()-start;sum+=t;if(t>max)max=t;}
    printf("%s17 SE x 5 sources, 255 gates, 128-frame blocks: mean %.3f ms, max %.3f ms; deadline %.3f ms\n",sequence?"2 BGM crossfade + ":"",sum/2,max*1000,128000.0/44100);
    ca_mixer_destroy(m);cs_sequence_destroy(sequence);ca_se_definition_destroy(definition);chip_audio_destroy(a);return 0;
}
