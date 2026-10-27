#include "chip-sequencer.h"
#include "chip-audio.h"
#include "json.h"
#include "chip-tempo.h"
#include <stdatomic.h>
#include <stdio.h>
typedef struct {int64_t from,to,from_tick,to_tick;char id[129];} Visit;
typedef struct {int64_t from,to,from_tick,to_tick;double bpm_from,bpm_to;int64_t ppq;} Map;
typedef struct {char id[129];double duty,rate,depth,delay,attack,decay,sustain,release,tom_time,tom_depth;int tom;} Timbre;
struct CSSequence {
    atomic_uint refs;uint64_t revision;ChipAudio *audio;char *json;size_t bytes;
    int muted[5];Visit *plans[3];size_t count[3];int64_t lengths[3];Map *map;size_t maps;
};
struct CSPlayer {CSSequence *sequence;ChipAudio *audio;CSMode mode;size_t visit;int plan;uint64_t loops;int64_t performance;CASource *source;};
struct CASeek {CAOperation *operation;};
struct CSEdit {CAOperation *operation;};
static atomic_uint_fast64_t revisions=1;
static const char sequence_provider;
static int compare_ids(const void *a,const void *b){return strcmp(a,b);}
static int idstring(const JP *p,size_t at,char out[129]){
    if(!jstring(p,at,out,129)||!out[0])return 0;
    for(char *c=out;*c;c++)if(!((*c>='A'&&*c<='Z')||(*c>='a'&&*c<='z')||(*c>='0'&&*c<='9')||*c=='_'||*c=='.'||*c=='-'))return 0;return 1;
}
static int array(const JP *p,size_t at,size_t max){return at<p->used&&p->tokens[at].kind=='['&&p->tokens[at].count<=max;}
static int fieldint(const JP *p,size_t at,const char *key,int64_t lo,int64_t hi,int64_t *value){return jint(p,jget(p,at,key),lo,hi,value);}
static int fieldnum(const JP *p,size_t at,const char *key,double lo,double hi,double *value){return jnumber(p,jget(p,at,key),lo,hi,value);}
static int lookup(char ids[][129],int count,const JP *p,size_t at){for(int i=0;i<count;i++)if(jeq(p,at,ids[i]))return i;return -1;}
static int sound_index(Timbre *sounds,size_t count,const JP *p,size_t at){for(size_t i=0;i<count;i++)if(jeq(p,at,sounds[i].id))return (int)i;return -1;}
static int parse_sound(const JP *p,size_t at,Timbre *t){
    if(!jobject(p,at,"id duty vibrato_hz vibrato_cents vibrato_delay_seconds attack_seconds decay_seconds sustain_level release_seconds tom_enabled tom_seconds tom_semitones")||!idstring(p,jget(p,at,"id"),t->id))return 0;
    size_t flag=jget(p,at,"tom_enabled");if(flag>=p->used||(p->tokens[flag].kind!='t'&&p->tokens[flag].kind!='f'))return 0;t->tom=p->tokens[flag].kind=='t';
    return fieldnum(p,at,"duty",.125,.75,&t->duty)&&(t->duty==.125||t->duty==.25||t->duty==.5||t->duty==.75)&&
        fieldnum(p,at,"vibrato_hz",1,12,&t->rate)&&fieldnum(p,at,"vibrato_cents",0,100,&t->depth)&&
        fieldnum(p,at,"vibrato_delay_seconds",0,1,&t->delay)&&fieldnum(p,at,"attack_seconds",0,.5,&t->attack)&&
        fieldnum(p,at,"decay_seconds",0,2,&t->decay)&&fieldnum(p,at,"sustain_level",0,1,&t->sustain)&&
        fieldnum(p,at,"release_seconds",0,10,&t->release)&&fieldnum(p,at,"tom_seconds",.001,10,&t->tom_time)&&fieldnum(p,at,"tom_semitones",0,127,&t->tom_depth);
}
static ChipPulseChange change(int64_t at,Timbre *t){return (ChipPulseChange){at,t->duty,t->rate,t->depth,t->delay,t->decay,t->attack,1-t->sustain};}
static void sequence_free(CSSequence *s){if(!s)return;chip_audio_destroy(s->audio);for(int i=0;i<3;i++)free(s->plans[i]);free(s->map);free(s->json);free(s);}
CSSequence *cs_sequence_retain(const CSSequence *s){if(!s)return NULL;CSSequence *v=(CSSequence *)s;atomic_fetch_add(&v->refs,1);return v;}
void cs_sequence_destroy(CSSequence *s){if(s&&atomic_fetch_sub(&s->refs,1)==1)sequence_free(s);}
uint64_t cs_sequence_revision(const CSSequence *s){return s?s->revision:0;}
static int64_t mapped_sample(const CSSequence *s,int64_t tick){
    if(s->maps==1&&s->map[0].bpm_from==s->map[0].bpm_to){long double value=(long double)tick*60*44100/(s->map[0].bpm_from*s->map[0].ppq);return value<0||value>=0x1p63?-1:llroundl(value);}
    double seconds=0;
    for(size_t i=0;i<s->maps;i++){Map *m=&s->map[i];double ticks=(double)(m->to_tick-m->from_tick);
        if(tick<=m->to_tick){double value=(seconds+cs_tempo_seconds((double)(tick-m->from_tick),m->bpm_from,(m->bpm_to-m->bpm_from)/ticks,m->ppq))*44100;return !isfinite(value)||value<0||value>=0x1p63?-1:llround(value);}
        seconds+=cs_tempo_duration(ticks,m->bpm_from,m->bpm_to,m->ppq);
    }return -1;
}
CSSequence *cs_sequence_read(const char *text,size_t bytes,const CALimits *l,CAResult *error){
    if(error)*error=CA_INVALID;if(!text||!l)return NULL;
    if(bytes>l->input_bytes||bytes>=l->allocated_bytes){if(error)*error=CA_LIMIT;return NULL;}
    JP p;if(!jparse(&p,text,bytes,l->allocated_bytes-bytes)){free(p.tokens);return NULL;}
    char (*event_ids)[129]=NULL;char (*visit_ids)[129]=NULL;
    CSSequence *s=NULL;Timbre *sounds=NULL;ChipAudioNote *notes[4]={0};ChipPulseChange *changes[2]={0};ChipPulseOnset *onsets[2]={0};ChipDPCMHit *kicks=NULL;ChipEchoTap *echo=NULL;
    size_t counts[4]={0},nc[2]={0},no[2]={0},nk=0,ne=0;
    int64_t length,version,rate;char song[129];
    if(!jobject(&p,0,"format version song_id sample_rate score_samples once_samples first_pass_samples cycle_samples sources sounds events plans positions")||
        !jeq(&p,jget(&p,0,"format"),"chip-sequence")||!fieldint(&p,0,"version",1,1,&version)||!idstring(&p,jget(&p,0,"song_id"),song)||
        !fieldint(&p,0,"sample_rate",44100,44100,&rate)||!fieldint(&p,0,"score_samples",1,l->score_samples,&length))goto failure;
    size_t sources=jget(&p,0,"sources"),sound_array=jget(&p,0,"sounds"),events=jget(&p,0,"events"),positions=jget(&p,0,"positions"),plans=jget(&p,0,"plans");
    if(!array(&p,sources,5)||p.tokens[sources].count!=5||!array(&p,sound_array,l->events)||!p.tokens[sound_array].count||
       !array(&p,events,l->events)||!array(&p,positions,l->events)||!p.tokens[positions].count||!jobject(&p,plans,"once first_pass cycle"))goto failure;
    // Account peak input/token storage, temporary arrays, compiled DSP tables,
    // plans and map before allocating. 2048/event conservatively covers the
    // private DSP segment/echo span representation as well as decoder arrays.
    size_t n=p.tokens[events].count,ns=p.tokens[sound_array].count,remaining=l->allocated_bytes;
#define RESERVE(count,size) do {size_t cnt=(count),sz=(size);if(cnt>remaining/sz){if(error)*error=CA_LIMIT;goto failure;}remaining-=cnt*sz;}while(0)
    RESERVE(1,16384);RESERVE(bytes+1,2);RESERVE(p.used,sizeof(JT));
    RESERVE(n+1,2048);RESERVE(ns,sizeof(Timbre));RESERVE(p.tokens[positions].count,sizeof(Map));
    const char *budget_plans[]={"once","first_pass","cycle"};
    for(int i=0;i<3;i++){size_t list=jget(&p,plans,budget_plans[i]);if(!array(&p,list,l->visits))goto failure;RESERVE(p.tokens[list].count+1,sizeof(Visit)+129);}
    size_t echo_capacity[2]={0},budget_at=events+1;
    for(size_t i=0;i<n;i++){if(jeq(&p,jget(&p,budget_at,"kind"),"echo")){int64_t delay;if(!fieldint(&p,budget_at,"delay",1,1323000,&delay))goto failure;for(int v=0;v<2;v++)if((size_t)delay+1>echo_capacity[v])echo_capacity[v]=(size_t)delay+1;}budget_at=p.tokens[budget_at].next;}
    // EchoSample currently contains an int64 timestamp and double sample. Use
    // 32 bytes/slot as an ABI-independent upper bound including padding.
    RESERVE(echo_capacity[0],32);RESERVE(echo_capacity[1],32);
#undef RESERVE
    sounds=calloc(ns,sizeof(*sounds));event_ids=calloc(n?n:1,sizeof(*event_ids));if(!sounds||!event_ids)goto memory;
    size_t at=sound_array+1;for(size_t i=0;i<ns;i++){if(!parse_sound(&p,at,&sounds[i]))goto failure;for(size_t j=0;j<i;j++)if(!strcmp(sounds[i].id,sounds[j].id))goto failure;at=p.tokens[at].next;}
    char ids[5][129],track_ids[5][129];int used[5]={0},initial[5];double gains[5];int attacks[5],decays[5];ChipAudioTrack tracks[4]={0};
    at=sources+1;for(int i=0;i<5;i++){
        int64_t voice,attack,decay;char id[129],track[129];double gain;
        if(!jobject(&p,at,"id track_id voice wave gain initial_sound dpcm_attack_percent dpcm_decay_ms")||
           !idstring(&p,jget(&p,at,"id"),id)||!idstring(&p,jget(&p,at,"track_id"),track)||
           !fieldint(&p,at,"voice",0,4,&voice)||used[voice]||!fieldnum(&p,at,"gain",0,2,&gain)||
           !fieldint(&p,at,"dpcm_attack_percent",25,150,&attack)||!fieldint(&p,at,"dpcm_decay_ms",5,800,&decay))goto failure;
        for(int j=0;j<5;j++)if(used[j]&&(!strcmp(id,ids[j])||!strcmp(track,track_ids[j])))goto failure;
        used[voice]=1;strcpy(ids[voice],id);strcpy(track_ids[voice],track);gains[voice]=gain;attacks[voice]=(int)attack;decays[voice]=(int)decay;
        initial[voice]=sound_index(sounds,ns,&p,jget(&p,at,"initial_sound"));if(initial[voice]<0)goto failure;
        size_t wave=jget(&p,at,"wave");if(voice==4){if(!jeq(&p,wave,"dpcm"))goto failure;}else{
            int w=jeq(&p,wave,"pulse")?CHIP_PULSE:jeq(&p,wave,"triangle")?CHIP_TRIANGLE:jeq(&p,wave,"noise")?CHIP_NOISE:-1;if(w<0)goto failure;tracks[voice].wave=(ChipWave)w;
        }at=p.tokens[at].next;
    }
    for(int i=0;i<4;i++){notes[i]=calloc(n+1,sizeof(ChipAudioNote));if(!notes[i])goto memory;tracks[i].notes=notes[i];}
    for(int i=0;i<2;i++){changes[i]=calloc(n+1,sizeof(ChipPulseChange));onsets[i]=calloc(n+1,sizeof(ChipPulseOnset));if(!changes[i]||!onsets[i])goto memory;changes[i][nc[i]++]=change(0,&sounds[initial[i]]);}
    kicks=calloc(n+1,sizeof(*kicks));echo=calloc(n+1,sizeof(*echo));if(!kicks||!echo)goto memory;
    int64_t sound_at[2]={-1,-1};int64_t last=-1;int last_kind=-1;char last_id[129]="";
    at=events+1;for(size_t i=0;i<n;i++){
        int64_t when,end,pitch,volume,release_at,release_samples,delay,offset;char id[129];
        if(!idstring(&p,jget(&p,at,"id"),id)||!fieldint(&p,at,"sample",0,length-1,&when))goto failure;
        int voice=lookup(ids,5,&p,jget(&p,at,"source_id"));if(voice<0)goto failure;
        size_t kind=jget(&p,at,"kind");int order=jeq(&p,kind,"sound")?0:jeq(&p,kind,"onset")?1:jeq(&p,kind,"segment")?2:jeq(&p,kind,"dpcm")?3:jeq(&p,kind,"echo")?4:-1;
        if(order<0||when<last||(when==last&&(order<last_kind||(order==last_kind&&strcmp(id,last_id)<=0))))goto failure;
        strcpy(event_ids[i],id);
        last=when;last_kind=order;strcpy(last_id,id);
        if(order==0){
            if(!jobject(&p,at,"id kind sample source_id sound_id")||voice>1||tracks[voice].wave!=CHIP_PULSE)goto failure;
            int index=sound_index(sounds,ns,&p,jget(&p,at,"sound_id"));if(index<0||when<=sound_at[voice])goto failure;sound_at[voice]=when;
            if(when==0&&nc[voice]==1)changes[voice][0]=change(0,&sounds[index]);else changes[voice][nc[voice]++]=change(when,&sounds[index]);
        }else if(order==1){
            if(!jobject(&p,at,"id kind sample source_id end")||voice>1||!fieldint(&p,at,"end",when+1,length,&end))goto failure;
            onsets[voice][no[voice]++]=(ChipPulseOnset){when,end};
        }else if(order==2){
            if(!jobject(&p,at,"id kind sample source_id end pitch volume release_at release_samples")||voice>3||
                !fieldint(&p,at,"end",when+1,length,&end)||!fieldint(&p,at,"pitch",0,127,&pitch)||!fieldint(&p,at,"volume",1,15,&volume)||
                !fieldint(&p,at,"release_at",0,length,&release_at)||!fieldint(&p,at,"release_samples",0,441000,&release_samples)||(!release_samples&&release_at))goto failure;
            notes[voice][counts[voice]++]=(ChipAudioNote){when,end-when,(int)pitch,(int)volume,release_at,release_samples};
        }else if(order==3){
            if(!jobject(&p,at,"id kind sample source_id end offset volume")||voice!=4||!fieldint(&p,at,"end",when+1,length,&end)||
                !fieldint(&p,at,"offset",0,length,&offset)||!fieldint(&p,at,"volume",1,15,&volume))goto failure;
            kicks[nk++]=(ChipDPCMHit){when,end-when,offset,(int)volume};
        }else{
            double gain;int destination=lookup(ids,5,&p,jget(&p,at,"destination_id"));
            if(!jobject(&p,at,"id kind sample source_id end destination_id delay gain")||voice>1||destination!=1-voice||
               !fieldint(&p,at,"end",when+1,length,&end)||!fieldint(&p,at,"delay",1,1323000,&delay)||!fieldnum(&p,at,"gain",0,1,&gain)||gain==0)goto failure;
            echo[ne++]=(ChipEchoTap){when,end,delay,voice,gain};
        }at=p.tokens[at].next;
    }
    qsort(event_ids,n,sizeof(*event_ids),compare_ids);for(size_t i=1;i<n;i++)if(!strcmp(event_ids[i-1],event_ids[i]))goto failure;
    s=calloc(1,sizeof(*s));if(!s)goto memory;atomic_init(&s->refs,1);
    for(int i=0;i<4;i++)tracks[i].count=counts[i];s->audio=chip_audio_create(tracks,length,NULL);if(!s->audio)goto failure;
    for(int i=0;i<5;i++){s->muted[i]=gains[i]==0;chip_audio_set_voice_gain(s->audio,i,gains[i]>0?gains[i]:1);}
    chip_audio_set_dpcm_shape(s->audio,attacks[4],decays[4]);if(!chip_audio_set_dpcm_kicks(s->audio,kicks,nk))goto failure;
    for(int i=0;i<4;i++)if(tracks[i].wave==CHIP_PULSE)chip_audio_set_pulse_duty(s->audio,i,sounds[initial[i]].duty);
    for(int i=0;i<2;i++)if(tracks[i].wave==CHIP_PULSE&&!chip_audio_set_pulse_changes(s->audio,i,changes[i],nc[i],onsets[i],no[i]))goto failure;
    if(!chip_audio_set_echo_taps(s->audio,echo,ne))goto failure;
    const char *names[]={"once","first_pass","cycle"},*lengths[]={"once_samples","first_pass_samples","cycle_samples"};
    size_t total_visits=0;
    for(int plan=0;plan<3;plan++){
        size_t list=jget(&p,plans,names[plan]);if(!array(&p,list,l->visits)||p.tokens[list].count>l->visits-total_visits)goto failure;
        s->count[plan]=p.tokens[list].count;total_visits+=s->count[plan];if(plan<2&&!s->count[plan])goto failure;
        s->plans[plan]=calloc(s->count[plan]?s->count[plan]:1,sizeof(Visit));if(!s->plans[plan])goto memory;
        free(visit_ids);visit_ids=calloc(s->count[plan]?s->count[plan]:1,129);if(!visit_ids)goto memory;at=list+1;int64_t sum=0;
        for(size_t i=0;i<s->count[plan];i++){
            Visit *v=&s->plans[plan][i];if(!jobject(&p,at,"id from_sample to_sample from_tick to_tick passes")||!idstring(&p,jget(&p,at,"id"),v->id)||
                !fieldint(&p,at,"from_sample",0,length-1,&v->from)||!fieldint(&p,at,"to_sample",v->from+1,length,&v->to)||
                !fieldint(&p,at,"from_tick",0,INT64_MAX-1,&v->from_tick)||!fieldint(&p,at,"to_tick",v->from_tick+1,INT64_MAX,&v->to_tick))goto failure;
            strcpy(visit_ids[i],v->id);
            size_t passes=jget(&p,at,"passes");if(!array(&p,passes,l->repeat_depth))goto failure;
            size_t pass=passes+1;for(size_t j=0;j<p.tokens[passes].count;j++){char rid[129];int64_t number;if(!jobject(&p,pass,"repeat_id pass_number")||!idstring(&p,jget(&p,pass,"repeat_id"),rid)||!fieldint(&p,pass,"pass_number",1,65535,&number))goto failure;pass=p.tokens[pass].next;}
            if(sum>l->performance_samples-(v->to-v->from))goto failure;sum+=v->to-v->from;at=p.tokens[at].next;
        }
        qsort(visit_ids,s->count[plan],129,compare_ids);for(size_t i=1;i<s->count[plan];i++)if(!strcmp(visit_ids[i-1],visit_ids[i]))goto failure;
        if(!fieldint(&p,0,lengths[plan],0,l->performance_samples,&s->lengths[plan])||s->lengths[plan]!=sum)goto failure;
    }
    s->maps=p.tokens[positions].count;s->map=calloc(s->maps,sizeof(Map));if(!s->map)goto memory;at=positions+1;
    for(size_t i=0;i<s->maps;i++){
        Map *v=&s->map[i];if(!jobject(&p,at,"from_tick to_tick from_sample to_sample bpm_from bpm_to ppq")||
            !fieldint(&p,at,"from_tick",0,INT64_MAX-1,&v->from_tick)||!fieldint(&p,at,"to_tick",v->from_tick+1,INT64_MAX,&v->to_tick)||
            !fieldint(&p,at,"from_sample",0,length-1,&v->from)||!fieldint(&p,at,"to_sample",v->from,length,&v->to)||
            !fieldnum(&p,at,"bpm_from",.001,100000,&v->bpm_from)||!fieldnum(&p,at,"bpm_to",.001,100000,&v->bpm_to)||!fieldint(&p,at,"ppq",1,INT32_MAX,&v->ppq))goto failure;
        if(i?(v->ppq!=s->map[i-1].ppq||v->from!=s->map[i-1].to||v->from_tick!=s->map[i-1].to_tick):(v->from||v->from_tick))goto failure;at=p.tokens[at].next;
    }
    if(s->map[s->maps-1].to!=length)goto failure;
    for(size_t i=0;i<s->maps;i++)if(mapped_sample(s,s->map[i].from_tick)!=s->map[i].from||mapped_sample(s,s->map[i].to_tick)!=s->map[i].to)goto failure;
    for(int plan=0;plan<3;plan++)for(size_t i=0;i<s->count[plan];i++){Visit *v=&s->plans[plan][i];if(mapped_sample(s,v->from_tick)!=v->from||mapped_sample(s,v->to_tick)!=v->to)goto failure;}
    s->json=malloc(bytes+1);if(!s->json)goto memory;memcpy(s->json,text,bytes);s->json[bytes]=0;s->bytes=bytes;
    uint64_t current=atomic_load(&revisions);
    do {if(current==UINT64_MAX){if(error)*error=CA_OVERFLOW;goto failure;}}while(!atomic_compare_exchange_weak(&revisions,&current,current+1));s->revision=current;
    if(error)*error=CA_OK;goto done;
memory:if(error)*error=CA_MEMORY;
failure:sequence_free(s);s=NULL;
done:free(p.tokens);free(sounds);free(event_ids);free(visit_ids);for(int i=0;i<4;i++)free(notes[i]);for(int i=0;i<2;i++){free(changes[i]);free(onsets[i]);}free(kicks);free(echo);return s;
}
CAResult cs_sequence_write(const CSSequence *s,char *buffer,size_t capacity,size_t *required){if(!s||!required)return CA_INVALID;*required=s->bytes;if(!buffer)return CA_OK;if(capacity<s->bytes)return CA_LIMIT;memcpy(buffer,s->json,s->bytes);return CA_OK;}

static int64_t player_settle(void *context){
    CSPlayer *p=context;CSSequence *s=p->sequence;
    for(;;){
        if(p->visit>=s->count[p->plan]){
            if(p->mode!=CS_LOOP||!s->count[2])return 0;
            p->plan=2;p->visit=0;if(p->loops<UINT64_MAX)p->loops++;
            chip_audio_seek(p->audio,s->plans[2][0].from);
        }
        Visit *v=&s->plans[p->plan][p->visit];int64_t position=chip_audio_position(p->audio);
        if(position<v->to)return v->to-position;
        p->visit++;
        if(p->visit<s->count[p->plan]){int64_t next=s->plans[p->plan][p->visit].from;if(next!=position)chip_audio_seek(p->audio,next);}
    }
}
static void player_render(void *context,double *planes[5],size_t frames){CSPlayer *p=context;chip_audio_render_stems(p->audio,planes,frames);for(int v=0;v<5;v++)if(p->sequence->muted[v]&&planes[v])memset(planes[v],0,frames*sizeof(double));p->performance=p->performance>INT64_MAX-(int64_t)frames?INT64_MAX:p->performance+(int64_t)frames;}
static void player_dispose(void *context){CSPlayer *p=context;chip_audio_destroy(p->audio);cs_sequence_destroy(p->sequence);free(p);}
static int sample_compare(const void *a,const void *b){int64_t x=*(const int64_t *)a,y=*(const int64_t *)b;return x<y?-1:x>y;}
static ChipAudio *prepare_audio(const CSSequence *s,const CALimits *l,CAResult *error){
    size_t count=s->count[0]+s->count[1]+s->count[2];
    if(count>l->visits||count>SIZE_MAX/sizeof(int64_t)){if(error)*error=CA_LIMIT;return NULL;}
    int64_t *targets=calloc(count?count:1,sizeof(*targets));if(!targets){if(error)*error=CA_MEMORY;return NULL;}
    size_t used=0;for(int plan=0;plan<3;plan++)for(size_t i=0;i<s->count[plan];i++)targets[used++]=s->plans[plan][i].from;
    qsort(targets,used,sizeof(*targets),sample_compare);size_t unique=0;for(size_t i=0;i<used;i++)if(!i||targets[i]!=targets[i-1])targets[unique++]=targets[i];used=unique;
    size_t size=chip_audio_clone_size(s->audio),overhead=count*sizeof(int64_t)+1024;
    if(overhead>l->allocated_bytes||used>l->checkpoints||size==SIZE_MAX||size>(l->allocated_bytes-overhead)/(used+2)){free(targets);if(error)*error=CA_LIMIT;return NULL;}
    ChipAudio *audio=chip_audio_clone(s->audio,l->allocated_bytes,NULL);
    if(audio&&!chip_audio_prepare_seeks(audio,targets,used)){chip_audio_destroy(audio);audio=NULL;}
    free(targets);if(!audio&&error)*error=CA_MEMORY;return audio;
}
CSPlayer *cs_player_prepare(const CSSequence *s,CSMode mode,const CALimits *l,CAResult *error){
    if(error)*error=CA_INVALID;if(!s||!l||(mode!=CS_ONCE&&mode!=CS_LOOP))return NULL;
    ChipAudio *audio=prepare_audio(s,l,error);if(!audio)return NULL;
    CSPlayer *p=calloc(1,sizeof(*p));if(!p){chip_audio_destroy(audio);if(error)*error=CA_MEMORY;return NULL;}
    p->sequence=(CSSequence *)s;atomic_fetch_add(&p->sequence->refs,1);p->mode=mode;p->plan=mode==CS_ONCE?0:1;p->audio=audio;
    chip_audio_seek(audio,s->plans[p->plan][0].from);
    CASourceOps ops={player_settle,player_render,player_dispose};p->source=ca_source_prepare(p,&ops,l,error);
    if(!p->source){player_dispose(p);return NULL;}ca_source_bind_provider(p->source,&sequence_provider);return p;
}
void cs_player_destroy(CSPlayer *p){if(p)ca_source_destroy(p->source);}
CAResult cs_play(CAMixer *m,CSPlayer *p,int64_t at,CAHandle *out){if(!p)return CA_INVALID;return ca_source_attach(m,p->source,(CARoute){CA_BGM,0},at,out);}
// The host supplies the current handle for ordinary source replacement. cs_change
// queries the current BGM in the mixer through its public group query.
CAResult cs_change(CAMixer *m,CSPlayer *p,int64_t fade,int64_t at,CAHandle *out){
    CAHandle current;CAResult e=ca_group_current(m,CA_BGM,&current);
    if(e==CA_STALE_HANDLE)return cs_play(m,p,at,out);
    if(e)return e;if(!p)return CA_INVALID;return ca_source_replace(m,current,p->source,(CARoute){CA_BGM,0},fade,at,out);
}
CAResult cs_stop(CAMixer *m,CAHandle h,int64_t f,int64_t at){return ca_source_stop(m,h,f,at);}
CAResult cs_pause(CAMixer *m,CAHandle h,int64_t f,int64_t at){return ca_source_pause(m,h,f,at);}
CAResult cs_resume(CAMixer *m,CAHandle h,int64_t f,int64_t at){return ca_source_resume(m,h,f,at);}
static void position_query(const void *ctx,void *out){
    const CSPlayer *p=ctx;CSPosition *v=out;v->performance_sample=p->performance;v->source_sample=chip_audio_position(p->audio);v->visit_index=(uint32_t)p->visit;v->loop_count=p->loops;v->revision=p->sequence->revision;
    v->source_tick=0;
    for(size_t i=0;i<p->sequence->maps;i++){
        Map *map=&p->sequence->map[i];if(v->source_sample<=map->to){double seconds=(double)(v->source_sample-map->from)/44100.,slope=(map->bpm_to-map->bpm_from)/(double)(map->to_tick-map->from_tick);
            double dx=cs_tempo_ticks(seconds,map->bpm_from,slope,map->ppq);
            v->source_tick=fmin((double)map->to_tick,fmax((double)map->from_tick,map->from_tick+dx));break;}
    }
}
CAResult cs_position(const CAMixer *m,CAHandle h,CSPosition *out){if(!out)return CA_INVALID;CSPosition pos={0};CAResult e=ca_source_query(m,h,&sequence_provider,position_query,&pos);if(e)return e;pos.output_sample=ca_mixer_position(m);e=ca_state(m,h,&pos.state);if(!e)*out=pos;return e;}
typedef struct {CSSequence *sequence;ChipAudio *audio;CSMode mode;size_t visit;uint64_t loops;int64_t performance;} SeekPayload;
static CAResult seek_check(const void *ctx,const void *payload){const CSPlayer *p=ctx;const SeekPayload *v=payload;return p->sequence->revision!=v->sequence->revision?CA_STALE_REVISION:p->mode!=v->mode?CA_NOT_PREPARED:CA_OK;}
static void seek_apply(void *ctx,void *payload){CSPlayer *p=ctx;SeekPayload *v=payload;ChipAudio *old=p->audio;p->audio=v->audio;v->audio=old;p->visit=v->visit;p->loops=v->loops;p->plan=v->loops?2:p->mode==CS_ONCE?0:1;p->performance=v->performance;}
static uint64_t player_revision(const void *ctx){return ((const CSPlayer *)ctx)->sequence->revision;}
static void seek_dispose(void *payload){SeekPayload *v=payload;chip_audio_destroy(v->audio);cs_sequence_destroy(v->sequence);free(v);}
CASeek *cs_prepare_seek(const CSSequence *s,CSMode mode,uint32_t visit,int64_t offset,uint64_t loops,const CALimits *l,CAResult *error){
    if(error)*error=CA_INVALID;if(!s||!l||(mode!=CS_ONCE&&mode!=CS_LOOP)||(mode==CS_ONCE&&loops))return NULL;
    int plan=loops?2:mode==CS_ONCE?0:1;if(visit>=s->count[plan]||offset<0||offset>=s->plans[plan][visit].to-s->plans[plan][visit].from)return NULL;
    int64_t perf=0;if(loops){if(!s->lengths[2]||loops-1>(uint64_t)(INT64_MAX-s->lengths[1])/s->lengths[2])return NULL;perf=s->lengths[1]+(int64_t)(loops-1)*s->lengths[2];}
    for(uint32_t i=0;i<visit;i++){int64_t n=s->plans[plan][i].to-s->plans[plan][i].from;if(perf>INT64_MAX-n)return NULL;perf+=n;}if(perf>INT64_MAX-offset)return NULL;perf+=offset;if(perf>l->performance_samples){if(error)*error=CA_LIMIT;return NULL;}
    SeekPayload *v=calloc(1,sizeof(*v));if(!v){if(error)*error=CA_MEMORY;return NULL;}v->audio=prepare_audio(s,l,error);if(!v->audio){free(v);return NULL;}
    // Arbitrary checkpoint reconstruction is deliberately off the audio thread.
    chip_audio_seek(v->audio,s->plans[plan][visit].from+offset);v->sequence=(CSSequence *)s;atomic_fetch_add(&v->sequence->refs,1);v->mode=mode;v->visit=visit;v->loops=loops;v->performance=perf;
    CAOperationOps ops={&sequence_provider,0,player_revision,seek_check,seek_apply,seek_dispose};CAOperation *op=ca_operation_prepare(&ops,v,error);if(!op)seek_dispose(v);return (CASeek *)op;
}
void ca_seek_destroy(CASeek *s){ca_operation_destroy((CAOperation *)s);}
CAResult cs_seek(CAMixer *m,CAHandle h,CASeek *s,int64_t at,CACommandID *id){return ca_source_operation(m,h,(CAOperation *)s,at,id);}
typedef struct {CSSequence *old,*next;ChipAudio *audio;} EditPayload;
static CAResult edit_check(const void *ctx,const void *payload){return ((const CSPlayer *)ctx)->sequence->revision==((const EditPayload *)payload)->old->revision?CA_OK:CA_STALE_REVISION;}
static void edit_apply(void *ctx,void *payload){CSPlayer *p=ctx;EditPayload *v=payload;chip_audio_exchange_score(p->audio,v->audio);CSSequence *old=p->sequence;p->sequence=v->next;v->next=old;}
static void edit_dispose(void *payload){EditPayload *v=payload;chip_audio_destroy(v->audio);cs_sequence_destroy(v->old);cs_sequence_destroy(v->next);free(v);}
CSEdit *cs_prepare_edit(const CSSequence *old,const CSSequence *next,const CALimits *l,CAResult *error){
    if(error)*error=CA_INVALID;if(!old||!next||!l)return NULL;
    if(error)*error=CA_UNSUPPORTED;
    if(chip_audio_length(old->audio)!=chip_audio_length(next->audio)||old->maps!=next->maps||memcmp(old->map,next->map,old->maps*sizeof(Map)))return NULL;
    for(int i=0;i<3;i++)if(old->count[i]!=next->count[i]||memcmp(old->plans[i],next->plans[i],old->count[i]*sizeof(Visit)))return NULL;
    EditPayload *v=calloc(1,sizeof(*v));if(!v){if(error)*error=CA_MEMORY;return NULL;}v->audio=prepare_audio(next,l,error);if(!v->audio){free(v);return NULL;}
    v->old=(CSSequence *)old;v->next=(CSSequence *)next;atomic_fetch_add(&v->old->refs,1);atomic_fetch_add(&v->next->refs,1);
    CAOperationOps ops={&sequence_provider,1,player_revision,edit_check,edit_apply,edit_dispose};CAOperation *o=ca_operation_prepare(&ops,v,error);if(!o)edit_dispose(v);return (CSEdit *)o;
}
void cs_edit_destroy(CSEdit *e){ca_operation_destroy((CAOperation *)e);}
CAResult cs_apply_edit(CAMixer *m,CAHandle h,CSEdit *e,int64_t at,CACommandID *id){return ca_source_operation(m,h,(CAOperation *)e,at,id);}

int64_t cs_sequence_plan_length(const CSSequence *s,unsigned plan){return s&&plan<3?s->lengths[plan]:0;}
CAResult cs_sequence_locate(const CSSequence *s,CSMode mode,int64_t source_sample,uint32_t *visit,int64_t *offset){
    if(!s||!visit||!offset||source_sample<0||(mode!=CS_ONCE&&mode!=CS_LOOP))return CA_INVALID;
    int plan=mode==CS_ONCE?0:1;
    for(size_t i=0;i<s->count[plan];i++){Visit *v=&s->plans[plan][i];if(source_sample>=v->from&&source_sample<v->to){if(i>UINT32_MAX)return CA_OVERFLOW;*visit=(uint32_t)i;*offset=source_sample-v->from;return CA_OK;}}
    return CA_NOT_PREPARED;
}

CAResult cs_sequence_at(const CSSequence *s,CSMode mode,int64_t elapsed,int64_t *source,int64_t *remaining){
    if(!s||!source||!remaining||elapsed<0||(mode!=CS_ONCE&&mode!=CS_LOOP))return CA_INVALID;
    int plan=mode==CS_ONCE?0:1;
    if(elapsed>=s->lengths[plan]){
        if(mode!=CS_LOOP||!s->lengths[2])return CA_NOT_PREPARED;
        elapsed=(elapsed-s->lengths[plan])%s->lengths[2];plan=2;
    }
    for(size_t i=0;i<s->count[plan];i++){
        Visit *v=&s->plans[plan][i];int64_t n=v->to-v->from;
        if(elapsed<n){*source=v->from+elapsed;*remaining=n-elapsed;
            while(i+1<s->count[plan]&&s->plans[plan][i].to==s->plans[plan][i+1].from){i++;*remaining+=s->plans[plan][i].to-s->plans[plan][i].from;}return CA_OK;}
        elapsed-=n;
    }return CA_NOT_PREPARED;
}
