// Standalone consumer: compiled sequence JSON -> signed little-endian PCM.
#include "chip-sequencer.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc,char **argv){
    if(argc!=3){fprintf(stderr,"usage: render sequence.json output.s16le\n");return 2;}
    CALimits l={.input_bytes=16<<20,.allocated_bytes=256<<20,.events=1000000,.visits=100000,.checkpoints=128,.repeat_depth=16,.se_instances=17,.commands=256,.retired_objects=256,.command_results=256,.gain_bindings=256,.score_samples=INT64_C(44100)*86400,.performance_samples=INT64_C(44100)*86400};
    FILE *f=fopen(argv[1],"rb");if(!f)return 1;if(fseek(f,0,SEEK_END)){fclose(f);return 1;}long length=ftell(f);if(length<0||(size_t)length>l.input_bytes){fclose(f);return 1;}rewind(f);
    char *json=malloc((size_t)length);if(!json){fclose(f);return 1;}size_t n=fread(json,1,(size_t)length,f);fclose(f);CAResult error;
    CSSequence *s=n==(size_t)length?cs_sequence_read(json,n,&l,&error):NULL;free(json);if(!s){fprintf(stderr,"invalid sequence\n");return 1;}
    CSPlayer *p=cs_player_prepare(s,CS_ONCE,&l,&error);CAMixer *m=ca_mixer_create(&l,&error);CAHandle h;int ok=p&&m&&cs_play(m,p,0,&h)==CA_OK;
    if(!ok)cs_player_destroy(p);f=ok?fopen(argv[2],"wb"):NULL;if(!f)ok=0;
    for(int64_t at=0,frames=cs_sequence_plan_length(s,0);ok&&at<frames;){int16_t pcm[735];unsigned char bytes[1470];size_t take=(size_t)(frames-at<735?frames-at:735);CAOutput out={.pcm=pcm};ok=ca_mixer_render(m,take,&out)==CA_OK;
        for(size_t i=0;i<take;i++){unsigned v=(uint16_t)pcm[i];bytes[i*2]=(unsigned char)v;bytes[i*2+1]=(unsigned char)(v>>8);}if(ok)ok=fwrite(bytes,2,take,f)==take;at+=(int64_t)take;}
    if(f&&fclose(f))ok=0;ca_mixer_destroy(m);cs_sequence_destroy(s);return ok?0:1;
}
