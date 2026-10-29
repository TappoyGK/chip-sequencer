#include "chip-sequencer.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static CALimits limits(void){CALimits l={0};l.input_bytes=1<<20;l.allocated_bytes=256<<20;l.events=100000;l.visits=10000;l.checkpoints=128;l.repeat_depth=16;l.score_samples=44100*3600;l.performance_samples=44100*3600;l.se_instances=17;l.commands=128;l.retired_objects=128;l.command_results=32;l.gain_bindings=128;return l;}
int main(int argc,char **argv){
    assert(argc==2);FILE *f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);long bytes=ftell(f);rewind(f);char *json=malloc((size_t)bytes+1);assert(json);assert(fread(json,1,(size_t)bytes,f)==(size_t)bytes);json[bytes]=0;fclose(f);
    CALimits l=limits();CAResult error;CSSequence *s=cs_sequence_read(json,(size_t)bytes,&l,&error);if(!s)fprintf(stderr,"read error=%d\n",error);assert(s);
    size_t n;assert(cs_sequence_write(s,NULL,0,&n)==CA_OK&&n==(size_t)bytes);
    char *copy=malloc(n);assert(cs_sequence_write(s,copy,n,&n)==CA_OK&&!memcmp(copy,json,n));free(copy);
    // Entire first pass and the first loop boundary are invariant to host blocks.
    size_t sizes[]={1,127,128,129,735,4096};size_t frames=352801;
    int16_t *reference=malloc(frames*sizeof(*reference)),*actual=malloc(frames*sizeof(*actual));assert(reference&&actual);
    for(size_t k=0;k<sizeof(sizes)/sizeof(*sizes);k++){
        CSPlayer *player=cs_player_prepare(s,CS_LOOP,&l,&error);CAMixer *mix=ca_mixer_create(&l,&error);CAHandle handle;assert(player&&mix&&cs_play(mix,player,0,&handle)==CA_OK);
        for(size_t at=0;at<frames;){size_t take=sizes[k];if(take>frames-at)take=frames-at;CAOutput out={.pcm=actual+at};assert(ca_mixer_render(mix,take,&out)==CA_OK);at+=take;}
        if(!k)memcpy(reference,actual,frames*sizeof(*actual));else assert(!memcmp(reference,actual,frames*sizeof(*actual)));ca_mixer_destroy(mix);
    }free(reference);free(actual);
    // A no-op revision swap preserves oscillator phase and output across the
    // scheduled boundary; it must not restart from a newly prepared snapshot.
    CSSequence *revision2=cs_sequence_read(json,(size_t)bytes,&l,&error);assert(revision2);
    CAMixer *pair[2];CAHandle pair_handle[2];int16_t comparison[2][2000];
    for(int k=0;k<2;k++){pair[k]=ca_mixer_create(&l,&error);CSPlayer *player=cs_player_prepare(s,CS_LOOP,&l,&error);assert(player&&pair[k]&&cs_play(pair[k],player,0,&pair_handle[k])==CA_OK);assert(ca_mixer_advance(pair[k],137)==CA_OK);}
    CSEdit *noop=cs_prepare_edit(s,revision2,&l,&error);CACommandID noop_id;assert(noop&&cs_apply_edit(pair[1],pair_handle[1],noop,211,&noop_id)==CA_OK);
    for(int k=0;k<2;k++){CAOutput out={.pcm=comparison[k]};assert(ca_mixer_render(pair[k],2000,&out)==CA_OK);}
    assert(!memcmp(comparison[0],comparison[1],sizeof comparison[0]));
    ca_mixer_destroy(pair[0]);ca_mixer_destroy(pair[1]);cs_sequence_destroy(revision2);
    CSPlayer *p=cs_player_prepare(s,CS_ONCE,&l,&error);assert(p);CAMixer *m=ca_mixer_create(&l,&error);assert(m);CAHandle h;assert(cs_play(m,p,0,&h)==CA_OK);
    double pcm[735];CAOutput output={.mix=pcm};assert(ca_mixer_render(m,735,&output)==CA_OK);int audible=0;for(int i=0;i<735;i++)if(pcm[i]!=0)audible++;assert(audible);
    CSPosition pos;assert(cs_position(m,h,&pos)==CA_OK&&pos.source_sample==735&&pos.performance_sample==735&&pos.revision==cs_sequence_revision(s));
    CASeek *seek=cs_prepare_seek(s,CS_ONCE,2,137,0,&l,&error);assert(seek);CACommandID command;assert(cs_seek(m,h,seek,800,&command)==CA_OK);assert(ca_mixer_advance(m,66)==CA_OK);assert(cs_position(m,h,&pos)==CA_OK&&pos.source_sample==138&&pos.visit_index==2);
    CACommandResult result;assert(ca_poll_result(m,&result)&&result.result==CA_OK);
    CSSequence *next=cs_sequence_read(json,(size_t)bytes,&l,&error);assert(next);
    CSEdit *edit=cs_prepare_edit(s,next,&l,&error);assert(edit);assert(cs_apply_edit(m,h,edit,900,&command)==CA_OK);assert(ca_mixer_advance(m,100)==CA_OK);assert(ca_poll_result(m,&result)&&result.result==CA_OK&&result.revision==cs_sequence_revision(next));
    assert(cs_position(m,h,&pos)==CA_OK&&pos.visit_index==2&&pos.source_sample==238);
    ca_mixer_destroy(m);cs_sequence_destroy(next);
    p=cs_player_prepare(s,CS_LOOP,&l,&error);assert(p);m=ca_mixer_create(&l,&error);assert(cs_play(m,p,0,&h)==CA_OK);
    assert(ca_mixer_advance(m,352801)==CA_OK);assert(cs_position(m,h,&pos)==CA_OK&&pos.loop_count==1&&pos.source_sample==1);
    ca_mixer_destroy(m);
    // A performance point's visit (cs_sequence_visit_at): a fresh player seeked there is where the
    // song played on from its top is at that point, visit, source sample, loop count and
    // performance sample alike, a sample on (points in the first pass and loops on, a repeated
    // section's later visits among them); a point on a visit's start sounds the same samples.
    {
        int64_t first=cs_sequence_plan_length(s,1),cycle=cs_sequence_plan_length(s,2);assert(first>0&&cycle>0);
        int64_t points[]={1,4410,first/2,first-1,first,first+cycle/3,first+2*cycle+7};size_t count=sizeof(points)/sizeof(*points);
        for(size_t k=0;k<count;k++){
            uint32_t visit;int64_t offset;uint64_t loops;assert(cs_sequence_visit_at(s,CS_LOOP,points[k],&visit,&offset,&loops)==CA_OK);
            assert(loops==(points[k]<first?0:(uint64_t)((points[k]-first)/cycle)+1));
            CSPosition along,seeked;
            p=cs_player_prepare(s,CS_LOOP,&l,&error);assert(p);m=ca_mixer_create(&l,&error);assert(cs_play(m,p,0,&h)==CA_OK);
            assert(ca_mixer_advance(m,(size_t)points[k]+1)==CA_OK);assert(cs_position(m,h,&along)==CA_OK);ca_mixer_destroy(m);
            CASeek *ticket=cs_prepare_seek(s,CS_LOOP,visit,offset,loops,&l,&error);assert(ticket);
            p=cs_player_prepare(s,CS_LOOP,&l,&error);assert(p);m=ca_mixer_create(&l,&error);assert(cs_play(m,p,0,&h)==CA_OK);
            CACommandID id;assert(cs_seek(m,h,ticket,0,&id)==CA_OK);assert(ca_mixer_advance(m,1)==CA_OK);assert(cs_position(m,h,&seeked)==CA_OK);ca_mixer_destroy(m);
            assert(seeked.visit_index==along.visit_index&&seeked.source_sample==along.source_sample&&seeked.loop_count==along.loop_count&&seeked.performance_sample==along.performance_sample);
        }
        // On a visit's start (the second visit of the first pass, a repeat), the very samples.
        int64_t start=first/2;uint32_t visit;int64_t offset;uint64_t loops;assert(cs_sequence_visit_at(s,CS_LOOP,start,&visit,&offset,&loops)==CA_OK&&offset==0&&visit>0);
        size_t span=4410;int16_t *top=malloc((size_t)(start+(int64_t)span)*sizeof(*top)),*from=malloc(span*sizeof(*from));assert(top&&from);
        p=cs_player_prepare(s,CS_LOOP,&l,&error);assert(p);m=ca_mixer_create(&l,&error);assert(cs_play(m,p,0,&h)==CA_OK);
        for(int64_t at=0;at<start+(int64_t)span;){int64_t take=start+(int64_t)span-at<4096?start+(int64_t)span-at:4096;CAOutput out={.pcm=top+at};assert(ca_mixer_render(m,(size_t)take,&out)==CA_OK);at+=take;}
        ca_mixer_destroy(m);
        CASeek *ticket=cs_prepare_seek(s,CS_LOOP,visit,offset,loops,&l,&error);assert(ticket);
        p=cs_player_prepare(s,CS_LOOP,&l,&error);assert(p);m=ca_mixer_create(&l,&error);assert(cs_play(m,p,0,&h)==CA_OK);
        CACommandID id;assert(cs_seek(m,h,ticket,0,&id)==CA_OK);CAOutput out={.pcm=from};assert(ca_mixer_render(m,span,&out)==CA_OK);ca_mixer_destroy(m);
        assert(!memcmp(top+start,from,span*sizeof(*from)));free(top);free(from);
        assert(cs_sequence_visit_at(s,CS_ONCE,cs_sequence_plan_length(s,0),&visit,&offset,&loops)==CA_NOT_PREPARED);
        assert(cs_sequence_visit_at(s,CS_LOOP,-1,&visit,&offset,&loops)==CA_INVALID);
    }
    cs_sequence_destroy(s);
    json[0]='[';assert(!cs_sequence_read(json,(size_t)bytes,&l,&error));free(json);
    puts("sequence: strict read/write, once/loop, positions, prepared seek, a performance point's visit, live edit revision: PASS");return 0;
}
