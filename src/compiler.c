#include "chip-sequencer.h"
#include "chip-drums.h"
#include "json.h"
#include "chip-tempo.h"
#include <stdio.h>
#include <stdarg.h>
struct CSModel {char *json;size_t bytes;CALimits limits;CSSequence *compiled;};
typedef struct {char *data;size_t used,capacity,max;CAResult failed;} Writer;
static void writef(Writer *w,const char *format,...){
    if(w->failed)return;va_list ap,copy;va_start(ap,format);va_copy(copy,ap);int need=vsnprintf(NULL,0,format,copy);va_end(copy);
    if(need<0||(size_t)need>=w->max||w->used>w->max-(size_t)need-1){w->failed=CA_LIMIT;va_end(ap);return;}
    size_t capacity=w->used+(size_t)need+1;
    if(capacity>w->capacity){size_t grown=w->capacity?w->capacity:1024;while(grown<capacity&&grown<=w->max/2)grown*=2;if(grown<capacity)grown=capacity;char *p=realloc(w->data,grown);if(!p){w->failed=CA_MEMORY;va_end(ap);return;}w->data=p;w->capacity=grown;}
    vsnprintf(w->data+w->used,w->capacity-w->used,format,ap);w->used+=(size_t)need;va_end(ap);
}
static void raw(Writer *w,JP *p,size_t token){if(token>=p->used){w->failed=CA_LIMIT;return;}JT t=p->tokens[token];if(t.end-t.start>INT_MAX){w->failed=CA_LIMIT;return;}writef(w,"%.*s",(int)(t.end-t.start),p->text+t.start);}
static int64_t integer(JP *p,size_t at,const char *key,int64_t lo,int64_t hi,int *ok){int64_t v=0;if(!jint(p,jrequired(p,at,key),lo,hi,&v))*ok=0;return v;}
static double number(JP *p,size_t at,const char *key,double lo,double hi,int *ok){double v=0;if(!jnumber(p,jrequired(p,at,key),lo,hi,&v))*ok=0;return v;}
static int boolean(JP *p,size_t at,const char *key,int *ok){size_t v=jrequired(p,at,key);if(v>=p->used||(p->tokens[v].kind!='t'&&p->tokens[v].kind!='f')){jfail(p,v,CA_INVALID,"Expected a boolean");*ok=0;return 0;}return p->tokens[v].kind=='t';}
static size_t list(JP *p,size_t at,const char *key,size_t max,int *ok){size_t n=jrequired(p,at,key);if(n>=p->used||p->tokens[n].kind!='['||p->tokens[n].count>max){jfail(p,n, n<p->used&&p->tokens[n].kind=='['?CA_LIMIT:CA_INVALID,"Expected an array with at most %zu items",max);*ok=0;return SIZE_MAX;}return n;}
typedef struct {int64_t a,b;double from,to,seconds,duration;} Tempo;
typedef struct {int64_t at,end,release,release_samples,offset,delay;int kind,voice,pitch,volume,dest;double gain;size_t sound,origin;uint64_t id;} Event;
typedef struct {int64_t a,b;size_t stack[16];int passes[16],depth;} CVisit;
typedef struct {int voice,kind; int64_t at,end; double from,to; size_t sound,order;} Control;
typedef struct {int source,destination; int64_t from,to,delay; double gain;size_t token;} Echo;
typedef struct {
    Control *expression,*sound_control; size_t expression_count,sound_count;
    Echo *echo_control; size_t echo_count;
    size_t origin;JP p;CALimits limits;int ok;int64_t ppq,end,length;
    Tempo *tempo;size_t tempos;Event *events;size_t count,capacity;
    CVisit *visits;size_t visits_count,visits_capacity;
    char ids[5][129];size_t initial[5],sound_array,changes,dynamics,hairpins,drums,echoes;
    double gain[5];
} Compile;
static int id(JP *p,size_t token,char out[129]){if(!jstring(p,token,out,129)||!out[0])return jfail(p,token,CA_INVALID,"Expected a nonempty ASCII identifier (maximum 128 bytes)");for(char *s=out;*s;s++)if(!((*s>='a'&&*s<='z')||(*s>='A'&&*s<='Z')||(*s>='0'&&*s<='9')||strchr("_.-",*s)))return jfail(p,token,CA_INVALID,"Identifier permits only letters, digits, underscore, dot and hyphen");return 1;}
static int voice(Compile *c,size_t token){for(int i=0;i<5;i++)if(jeq(&c->p,token,c->ids[i]))return i;jfail(&c->p,token,CA_INVALID,"Unknown source_id");c->ok=0;return 0;}
static size_t sound(Compile *c,size_t name){size_t at=c->sound_array+1;for(size_t i=0;i<c->p.tokens[c->sound_array].count;i++){char a[129],b[129];if(jstring(&c->p,name,a,129)&&jstring(&c->p,jget(&c->p,at,"id"),b,129)&&!strcmp(a,b))return at;at=c->p.tokens[at].next;}jfail(&c->p,name,CA_INVALID,"Unknown sound reference");c->ok=0;return 0;}
static int cmp64(const void *a,const void *b){int64_t x=*(const int64_t *)a,y=*(const int64_t *)b;return x<y?-1:x>y;}
static double bpm(Compile *c,size_t tempos,int64_t tick){double value=0;int64_t last=-1;size_t at=tempos+1;for(size_t i=0;i<c->p.tokens[tempos].count;i++){int64_t t=integer(&c->p,at,"tick",0,c->end-1,&c->ok);double b=number(&c->p,at,"bpm",.001,100000,&c->ok);if(t<=tick&&t>last){last=t;value=b;}at=c->p.tokens[at].next;}if(last<0)c->ok=0;return value;}
static int64_t sample(Compile *c,int64_t tick){
    if(tick<=0)return 0;if(tick>c->end)tick=c->end;
    if(c->tempos==1&&c->tempo[0].from==c->tempo[0].to){long double v=(long double)tick*60*44100/(c->tempo[0].from*c->ppq);if(v>c->limits.score_samples){jfail(&c->p,jget(&c->p,0,"end_tick"),CA_LIMIT,"Score exceeds sample limit");c->ok=0;return 0;}return llroundl(v);}
    for(size_t i=0;i<c->tempos;i++){Tempo *t=&c->tempo[i];if(tick<=t->b){double dx=(double)(tick-t->a),slope=(t->to-t->from)/(double)(t->b-t->a);
        double seconds=t->seconds+cs_tempo_seconds(dx,t->from,slope,c->ppq),v=seconds*44100;
        if(!isfinite(v)||v>c->limits.score_samples){jfail(&c->p,jget(&c->p,0,"end_tick"),CA_LIMIT,"Score exceeds sample limit");c->ok=0;return 0;}return llround(v);}}
    c->ok=0;return 0;
}
static double tick_at(Compile *c,int64_t s){
    double seconds=(double)s/44100.;
    for(size_t i=0;i<c->tempos;i++){Tempo *t=&c->tempo[i];if(seconds<=t->seconds+t->duration){double slope=(t->to-t->from)/(double)(t->b-t->a),elapsed=seconds-t->seconds;return t->a+cs_tempo_ticks(elapsed,t->from,slope,c->ppq);}}return c->end;
}
// Resolve controls once. Generation queries typed, voice-local time indexes;
// it never rescans expression/sound JSON for each 441-sample fragment.
static int control_compare(const void *a, const void *b) {
    const Control *x=a, *y=b;
    if (x->voice!=y->voice) return x->voice-y->voice;
    if (x->at!=y->at) return x->at<y->at?-1:1;
    if (x->kind!=y->kind) return x->kind-y->kind;
    // Equal-time hairpins: last input wins. Dynamics/sounds: first input wins.
    if (x->order==y->order) return 0;
    int order=x->order<y->order?-1:1;
    return x->kind==1?order:-order;
}
static const Control *control_at(const Control *items, size_t count, int voice, int64_t tick) {
    size_t lo=0, hi=count;
    while (lo<hi) {
        size_t mid=lo+(hi-lo)/2;
        const Control *v=&items[mid];
        if (v->voice<voice || (v->voice==voice && v->at<=tick)) lo=mid+1;
        else hi=mid;
    }
    return lo && items[lo-1].voice==voice ? &items[lo-1] : NULL;
}
static int prepare_controls(Compile *c) {
    JP *p=&c->p;
    size_t nd=p->tokens[c->dynamics].count, nh=p->tokens[c->hairpins].count;
    size_t ns=p->tokens[c->changes].count;
    size_t cap=c->limits.allocated_bytes/(16*sizeof(Control));
    if (nd>cap || nh>cap-nd || ns>cap-nd-nh) return jfail(&c->p,SIZE_MAX,CA_LIMIT,"Controls exceed memory budget");
    c->expression_count=nd+nh;
    c->sound_count=ns;
    c->expression=calloc(nd+nh?nd+nh:1,sizeof(Control));
    c->sound_control=calloc(ns?ns:1,sizeof(Control));
    if (!c->expression || !c->sound_control) return jfail(&c->p,SIZE_MAX,CA_MEMORY,"Cannot allocate controls");
    size_t arrays[]={c->dynamics,c->hairpins,c->changes};
    for (int kind=0;kind<3;kind++) {
        size_t at=arrays[kind]+1;
        for (size_t i=0;i<p->tokens[arrays[kind]].count;i++) {
            Control v={.voice=voice(c,jget(p,at,"source_id")),.kind=kind,.order=i};
            v.at=integer(p,at,kind==1?"from_tick":"tick",0,c->end-1,&c->ok);
            if (kind==0) v.from=v.to=integer(p,at,"volume",1,15,&c->ok);
            else if (kind==1) {
                v.end=integer(p,at,"to_tick",v.at+1,c->end,&c->ok);
                v.from=number(p,at,"from_volume",1,15,&c->ok);
                v.to=number(p,at,"to_volume",1,15,&c->ok);
            } else v.sound=sound(c,jget(p,at,"sound_id"));
            if (kind==2) c->sound_control[i]=v;
            else c->expression[kind==0?i:nd+i]=v;
            at=p->tokens[at].next;
        }
    }
    qsort(c->expression,c->expression_count,sizeof(Control),control_compare);
    qsort(c->sound_control,c->sound_count,sizeof(Control),control_compare);
    return c->ok;
}
static size_t sound_at(Compile *c, int voice, int64_t tick) {
    const Control *v=control_at(c->sound_control,c->sound_count,voice,tick);
    return v?v->sound:c->initial[voice];
}
static int volume_at(Compile *c, int voice, int64_t tick, int base) {
    const Control *v=control_at(c->expression,c->expression_count,voice,tick);
    double value=base;
    if (v) value=v->kind==1?
        v->from+fmin(1.,(double)(tick-v->at)/(v->end-v->at))*(v->to-v->from):v->from;
    int result=(int)llround(value);
    return result<1?1:result>15?15:result;
}

static int echo_compare(const void *a, const void *b) {
    const Echo *x=a, *y=b;
    if (x->source!=y->source) return x->source-y->source;
    return x->from<y->from?-1:x->from>y->from;
}
static const Echo *echo_at(const Compile *c, int voice, int64_t tick) {
    size_t lo=0,hi=c->echo_count;
    while (lo<hi) {
        size_t mid=lo+(hi-lo)/2;
        const Echo *v=&c->echo_control[mid];
        if (v->source<voice || (v->source==voice && v->from<=tick)) lo=mid+1;
        else hi=mid;
    }
    if (!lo) return NULL;
    const Echo *v=&c->echo_control[lo-1];
    return v->source==voice && tick<v->to?v:NULL;
}
// Echo delay is validated even if its arrival is beyond the song. Beyond the
// last tempo segment the endpoint BPM is held; the song boundary never clamps
// the delay or silently makes an invalid setting disappear.
static double delay_samples(const Compile *c, double start, int64_t ticks) {
    double end=start+(double)ticks, seconds=0;
    for (size_t i=0;i<c->tempos && start<end;i++) {
        const Tempo *t=&c->tempo[i];
        if (start>=t->b) continue;
        double stop=fmin(end,(double)t->b);
        double slope=(t->to-t->from)/(double)(t->b-t->a);
        double from=t->from+slope*(start-t->a);
        seconds+=cs_tempo_seconds(stop-start,from,slope,c->ppq);
        start=stop;
    }
    if (start<end) seconds+=cs_tempo_seconds(end-start,c->tempo[c->tempos-1].to,0,c->ppq);
    return seconds*44100;
}
static void tempo_line(const Compile *c, double at, double *bpm, double *slope) {
    for (size_t i=0;i<c->tempos;i++) {
        const Tempo *t=&c->tempo[i];
        if (at<t->b) {
            *slope=(t->to-t->from)/(double)(t->b-t->a);
            *bpm=t->from+*slope*(at-t->a);
            return;
        }
    }
    *bpm=c->tempo[c->tempos-1].to; *slope=0;
}
static int valid_delay_at(const Compile *c, const Echo *e, double at) {
    double samples=delay_samples(c,at,e->delay);
    return isfinite(samples) && samples>=1-1e-7 && samples<=1323000+1e-7;
}
static int valid_echo_delay(const Compile *c, const Echo *e, int64_t *cuts) {
    double max_bpm=0;
    for (size_t i=0;i<c->tempos;i++) max_bpm=fmax(max_bpm,fmax(c->tempo[i].from,c->tempo[i].to));
    if (e->delay>INT64_MAX-c->end ||
        (double)e->delay*60*44100/(max_bpm*c->ppq)>1323000+1e-7) return 0;
    size_t count=0;
    cuts[count++]=e->from; cuts[count++]=e->to-1;
    // The delay integral changes derivative at tempo boundaries and at those
    // boundaries shifted by -delay. Between them an extremum satisfies
    // BPM(t)==BPM(t+delay); both sides are affine, so there is at most one root.
    for (size_t i=0;i<c->tempos;i++) {
        int64_t points[]={c->tempo[i].b,c->tempo[i].b-e->delay};
        for (int j=0;j<2;j++) if (points[j]>e->from && points[j]<e->to-1) cuts[count++]=points[j];
    }
    qsort(cuts,count,sizeof(*cuts),cmp64);
    for (size_t i=0;i<count;i++) {
        if (!valid_delay_at(c,e,(double)cuts[i])) return 0;
        if (!i || cuts[i]==cuts[i-1]) continue;
        double a=cuts[i-1],b=cuts[i],mid=a+(b-a)/2;
        double from,to,from_slope,to_slope;
        tempo_line(c,mid,&from,&from_slope);
        tempo_line(c,mid+(double)e->delay,&to,&to_slope);
        double slope=to_slope-from_slope;
        if (slope!=0) {
            double root=mid-(to-from)/slope;
            if (root>a && root<b) {
                // Onsets are integer ticks. Check the neighbors of an extremum.
                if (!valid_delay_at(c,e,floor(root)) || !valid_delay_at(c,e,ceil(root))) return 0;
            }
        }
    }
    return 1;
}
static int prepare_echoes(Compile *c) {
    JP *p=&c->p;
    c->echo_count=p->tokens[c->echoes].count;
    if (c->echo_count>c->limits.allocated_bytes/(32*sizeof(Echo)) ||
        c->tempos>c->limits.allocated_bytes/(64*sizeof(int64_t))) return jfail(&c->p,SIZE_MAX,CA_LIMIT,"Echo controls exceed memory budget");
    c->echo_control=calloc(c->echo_count?c->echo_count:1,sizeof(Echo));
    int64_t *cuts=calloc(2+2*c->tempos,sizeof(int64_t));
    if (!c->echo_control || !cuts) { free(cuts); return jfail(&c->p,SIZE_MAX,CA_MEMORY,"Cannot allocate echo controls"); }
    size_t at=c->echoes+1;
    for (size_t i=0;i<c->echo_count;i++) {
        Echo *e=&c->echo_control[i];e->token=at;
        e->source=voice(c,jget(p,at,"source_id"));
        e->destination=voice(c,jget(p,at,"destination_id"));
        e->from=integer(p,at,"from_tick",0,c->end-1,&c->ok);
        e->to=integer(p,at,"to_tick",e->from+1,c->end,&c->ok);
        e->delay=integer(p,at,"delay_ticks",1,INT64_MAX,&c->ok);
        e->gain=number(p,at,"gain",0,1,&c->ok);
        if(!c->ok){free(cuts);return 0;}
        if(e->source>1||e->destination!=1-e->source){jfail(p,at,CA_INVALID,"Echo must route voice 0 to 1 or voice 1 to 0");free(cuts);return 0;}
        if(e->gain==0){jfail(p,jget(p,at,"gain"),CA_INVALID,"Echo gain must be greater than zero");free(cuts);return 0;}
        if(!valid_echo_delay(c,e,cuts)){jfail(p,jget(p,at,"delay_ticks"),CA_INVALID,"Echo delay must remain within 1..1323000 samples throughout its interval");free(cuts);return 0;}
        at=p->tokens[at].next;
    }
    free(cuts);
    qsort(c->echo_control,c->echo_count,sizeof(Echo),echo_compare);
    for (size_t i=1;i<c->echo_count;i++) {
        const Echo *a=&c->echo_control[i-1],*b=&c->echo_control[i];
        if(a->source==b->source&&a->to>b->from)return jfail(p,jget(p,b->token,"from_tick"),CA_INVALID,"Echo intervals overlap on the same source");
    }
    return 1;
}
static void event(Compile *c,Event e){
    if(!c->ok)return;if(c->count>=c->limits.events){jfail(&c->p,c->p.context,CA_LIMIT,"Compiled events exceed configured limit");c->ok=0;return;}
    if(c->count==c->capacity){size_t n=c->capacity?c->capacity*2:256;if(n>c->limits.events)n=c->limits.events;if(n>c->limits.allocated_bytes/(16*sizeof(Event))){jfail(&c->p,SIZE_MAX,CA_LIMIT,"Compiled tables exceed memory budget");c->ok=0;return;}Event *p=realloc(c->events,n*sizeof(*p));if(!p){jfail(&c->p,SIZE_MAX,CA_MEMORY,"Cannot allocate compiled tables");c->ok=0;return;}c->events=p;c->capacity=n;}
    e.id=c->count;e.origin=c->origin;c->events[c->count++]=e;
}
typedef struct {size_t token;int voice;int64_t tick;} NoteRef;
static int note_compare(const void *a,const void *b){const NoteRef *x=a,*y=b;return x->voice!=y->voice?x->voice-y->voice:x->tick!=y->tick?(x->tick<y->tick?-1:1):x->token<y->token?-1:x->token>y->token;}
static int compare_names(const void *a,const void *b){return strcmp(a,b);}
static int validate_items(Compile *c,size_t list_token,const char *fields,int has_id){
    size_t n=c->p.tokens[list_token].count;char (*names)[129]=NULL;
    if(has_id){if(n>c->limits.allocated_bytes/(16*129))return jfail(&c->p,list_token,CA_LIMIT,"Identifier table exceeds memory budget");names=calloc(n?n:1,129);if(!names)return jfail(&c->p,SIZE_MAX,CA_MEMORY,"Cannot allocate identifier table");}
    int ok=1;size_t at=list_token+1;for(size_t i=0;i<n;i++){
        if(!jobject(&c->p,at,fields)||(has_id&&!id(&c->p,jget(&c->p,at,"id"),names[i]))){ok=0;break;}at=c->p.tokens[at].next;
    }
    if(ok&&has_id){qsort(names,n,129,compare_names);for(size_t i=1;i<n;i++)if(!strcmp(names[i-1],names[i])){jduplicate(&c->p,list_token,names[i]);ok=0;break;}}
    free(names);return ok;
}
static int event_compare(const void *a,const void *b){const Event *x=a,*y=b;return x->at!=y->at?(x->at<y->at?-1:1):x->kind!=y->kind?x->kind-y->kind:x->id<y->id?-1:x->id>y->id;}
static void visit(Compile *c,CVisit v){
    if(!c->ok)return;if(c->visits_count>=c->limits.visits){jfail(&c->p,c->p.context,CA_LIMIT,"Compiled visits exceed configured limit");c->ok=0;return;}
    if(c->visits_count==c->visits_capacity){size_t n=c->visits_capacity?c->visits_capacity*2:64;if(n>c->limits.visits)n=c->limits.visits;if(n>c->limits.allocated_bytes/(32*sizeof(CVisit))){jfail(&c->p,SIZE_MAX,CA_LIMIT,"Compiled tables exceed memory budget");c->ok=0;return;}CVisit *p=realloc(c->visits,n*sizeof(*p));if(!p){jfail(&c->p,SIZE_MAX,CA_MEMORY,"Cannot allocate compiled tables");c->ok=0;return;}c->visits=p;c->visits_capacity=n;}c->visits[c->visits_count++]=v;
}
static int valid_order_ids(Compile *c,size_t root){
    if(root>=c->p.used)return 0;size_t count=c->p.tokens[root].next-root;
    if(count>c->limits.allocated_bytes/(16*129))return jfail(&c->p,root,CA_LIMIT,"Order identifier table exceeds memory budget");char (*names)[129]=calloc(count?count:1,129);if(!names)return jfail(&c->p,SIZE_MAX,CA_MEMORY,"Cannot allocate identifier table");size_t n=0;int ok=1;
    for(size_t i=root;i<c->p.tokens[root].next;i++)if(c->p.tokens[i].kind=='{'){
        size_t key=jget(&c->p,i,"id");if(key!=SIZE_MAX&&!id(&c->p,key,names[n++])){ok=0;break;}}
    qsort(names,n,129,compare_names);for(size_t i=1;i<n;i++)if(!strcmp(names[i-1],names[i])){
        for(size_t t=root,seen=0;t<c->p.tokens[root].next;t++)if(c->p.tokens[t].kind=='{'){
            size_t key=jget(&c->p,t,"id");if(jeq(&c->p,key,names[i])&&seen++){jfail(&c->p,key,CA_INVALID,"Duplicate playback order id '%s'",names[i]);break;}}
        ok=0;break;
    }
    free(names);return ok;
}
static int valid_order_tree(Compile *c,size_t token,unsigned depth){
    JP *p=&c->p;
    if(depth>16||depth>c->limits.repeat_depth)return jfail(p,token,CA_LIMIT,"Repeat nesting exceeds configured depth");
    if(token>=p->used||p->tokens[token].kind!='['||!p->tokens[token].count)return jfail(p,token,CA_INVALID,"Playback order must be a nonempty array");
    size_t at=token+1;for(size_t i=0;i<p->tokens[token].count;i++){
        if(jget(p,at,"repeat_count")==SIZE_MAX){if(!jobject(p,at,"id from_tick to_tick"))return 0;int64_t a=integer(p,at,"from_tick",0,c->end-1,&c->ok);integer(p,at,"to_tick",a+1,c->end,&c->ok);}
        else {if(!jobject(p,at,"id repeat_count body endings"))return 0;int64_t count=integer(p,at,"repeat_count",2,65535,&c->ok);
            if(!valid_order_tree(c,jget(p,at,"body"),depth+1))return 0;size_t endings=list(p,at,"endings",c->limits.visits,&c->ok);if(!c->ok)return 0;
            size_t e=endings+1;for(size_t j=0;j<p->tokens[endings].count;j++){
                if(!jobject(p,e,"passes body")||!valid_order_tree(c,jget(p,e,"body"),depth+1))return 0;size_t passes=list(p,e,"passes",65535,&c->ok);if(!c->ok)return 0;if(!p->tokens[passes].count)return jfail(p,passes,CA_INVALID,"Ending passes must not be empty");
                size_t pass=passes+1;for(size_t k=0;k<p->tokens[passes].count;k++){int64_t value;if(!jint(p,pass,1,count,&value))return 0;pass=p->tokens[pass].next;}e=p->tokens[e].next;
            }
        }if(!c->ok)return 0;at=p->tokens[at].next;
    }return 1;
}
static void order(Compile *c,size_t list_token,CVisit path,int depth){
    if(!c->ok)return;if(depth>16||(uint32_t)depth>c->limits.repeat_depth||list_token>=c->p.used||c->p.tokens[list_token].kind!='['||!c->p.tokens[list_token].count){c->ok=0;return;}
    size_t at=list_token+1;for(size_t i=0;i<c->p.tokens[list_token].count&&c->ok;i++){
        char name[129];if(!id(&c->p,jget(&c->p,at,"id"),name)){c->ok=0;return;}
        if(jget(&c->p,at,"repeat_count")==SIZE_MAX){
            if(!jobject(&c->p,at,"id from_tick to_tick")){c->ok=0;return;}
            path.a=integer(&c->p,at,"from_tick",0,c->end-1,&c->ok);path.b=integer(&c->p,at,"to_tick",path.a+1,c->end,&c->ok);visit(c,path);
        }else{
            if(depth==16||!jobject(&c->p,at,"id repeat_count body endings")){c->ok=0;return;}
            int count=(int)integer(&c->p,at,"repeat_count",2,65535,&c->ok);size_t body=list(&c->p,at,"body",c->limits.visits,&c->ok),endings=list(&c->p,at,"endings",c->limits.visits,&c->ok);if(!c->ok)return;
            for(int pass=1;pass<=count&&c->ok;pass++){
                CVisit child=path;child.depth=depth+1;child.stack[depth]=jget(&c->p,at,"id");child.passes[depth]=pass;order(c,body,child,depth+1);
                size_t e=endings+1;int matched=0;
                for(size_t j=0;j<c->p.tokens[endings].count;j++){
                    if(!jobject(&c->p,e,"passes body")){c->ok=0;return;}size_t passes=list(&c->p,e,"passes",65535,&c->ok);if(!c->ok)return;size_t pt=passes+1;
                    for(size_t k=0;k<c->p.tokens[passes].count;k++){int64_t pv;if(!jint(&c->p,pt,1,count,&pv)){c->ok=0;return;}if(pv==pass){if(matched++){jfail(&c->p,pt,CA_INVALID,"Repeated pass selects more than one ending");c->ok=0;return;}order(c,jget(&c->p,e,"body"),child,depth+1);}pt=c->p.tokens[pt].next;}e=c->p.tokens[e].next;
                }
            }
        }at=c->p.tokens[at].next;
    }
}
static void write_visit(Writer *w,Compile *c,CVisit *v,size_t i){
    writef(w,"{\"id\":\"visit%zu\",\"from_sample\":%lld,\"to_sample\":%lld,\"from_tick\":%lld,\"to_tick\":%lld,\"passes\":[",i,(long long)sample(c,v->a),(long long)sample(c,v->b),(long long)v->a,(long long)v->b);
    for(int j=0;j<v->depth;j++){if(j)writef(w,",");writef(w,"{\"repeat_id\":");raw(w,&c->p,v->stack[j]);writef(w,",\"pass_number\":%d}",v->passes[j]);}writef(w,"]}");
}
CSModel *cs_model_read(const char *text,size_t bytes,const CALimits *limits,CAResult *error){
    return cs_model_read_ex(text,bytes,limits,error,NULL);
}
CSModel *cs_model_read_ex(const char *text,size_t bytes,const CALimits *limits,CAResult *error,CSDiagnostic *diagnostic){
    CAResult local; if(!error)error=&local; *error=CA_INVALID;dreset(diagnostic);
    if(!text||!limits){dset(diagnostic,*error,NULL,0,SIZE_MAX,"Input and limits are required");return NULL;}
    if(bytes>limits->input_bytes||bytes>=limits->allocated_bytes){*error=CA_LIMIT;dset(diagnostic,*error,NULL,0,SIZE_MAX,"JSON input exceeds byte or memory budget");return NULL;}
    JP p;if(!jparse(&p,text,bytes,limits->allocated_bytes-bytes,diagnostic)){*error=p.status;free(p.tokens);return NULL;}
    int valid=jobject(&p,0,"format version song_id ppq end_tick sources sounds tempos tempo_ramps meters notes sound_changes dynamics hairpins drums echoes order loop");
    if(valid&&!jeq(&p,jget(&p,0,"format"),"chip-performance"))valid=jfail(&p,jget(&p,0,"format"),CA_INVALID,"Expected format 'chip-performance'");
    if(!valid)jfinish(&p,error,"Invalid performance document");
    free(p.tokens);if(!valid)return NULL;
    CSModel *m=calloc(1,sizeof(*m));
    if(m){m->json=malloc(bytes+1);if(!m->json){free(m);m=NULL;}}
    if(!m){*error=CA_MEMORY;dset(diagnostic,*error,NULL,0,SIZE_MAX,"Cannot allocate performance model");return NULL;}
    memcpy(m->json,text,bytes);m->json[bytes]=0;m->bytes=bytes;m->limits=*limits;
    CSSequence *validated=cs_compile_ex(m,limits,error,diagnostic);
    if(!validated){cs_model_destroy(m);return NULL;}
    m->compiled=validated;return m;
}
void cs_model_destroy(CSModel *m){if(m){cs_sequence_destroy(m->compiled);free(m->json);free(m);}}
CSSequence *cs_compile(const CSModel *m,const CALimits *limits,CAResult *error){return cs_compile_ex(m,limits,error,NULL);}
CSSequence *cs_compile_ex(const CSModel *m,const CALimits *limits,CAResult *error,CSDiagnostic *diagnostic){
    CAResult local;if(!error)error=&local;*error=CA_INVALID;dreset(diagnostic);
    if(!m||!limits){dset(diagnostic,*error,NULL,0,SIZE_MAX,"Model and limits are required");return NULL;}
    if(m->compiled&&!memcmp(&m->limits,limits,sizeof(*limits))){if(error)*error=CA_OK;return cs_sequence_retain(m->compiled);}
    Compile c={.limits=*limits,.ok=1};Writer w={.max=limits->allocated_bytes/4};CSSequence *result=NULL;int64_t *cuts=NULL;size_t *note_tokens=NULL;NoteRef *refs=NULL;
    if(!jparse(&c.p,m->json,m->bytes,limits->allocated_bytes/4,diagnostic))goto done;
    JP *p=&c.p;integer(p,0,"version",1,1,&c.ok);char song[129];if(!id(p,jget(p,0,"song_id"),song))goto done;
    c.ppq=integer(p,0,"ppq",1,INT32_MAX,&c.ok);c.end=integer(p,0,"end_tick",1,INT64_MAX/65536,&c.ok);
    size_t sources=list(p,0,"sources",5,&c.ok);c.sound_array=list(p,0,"sounds",limits->events,&c.ok);
    size_t tempos=list(p,0,"tempos",limits->events,&c.ok),ramps=list(p,0,"tempo_ramps",limits->events,&c.ok),meters=list(p,0,"meters",limits->events,&c.ok),notes=list(p,0,"notes",limits->events,&c.ok);
    c.changes=list(p,0,"sound_changes",limits->events,&c.ok);c.dynamics=list(p,0,"dynamics",limits->events,&c.ok);c.hairpins=list(p,0,"hairpins",limits->events,&c.ok);c.drums=list(p,0,"drums",4,&c.ok);c.echoes=list(p,0,"echoes",limits->events,&c.ok);
    if(!c.ok)goto done;
    if(p->tokens[sources].count!=5){jfail(p,sources,CA_INVALID,"Exactly five sources are required");goto done;}
    if(!p->tokens[tempos].count){jfail(p,tempos,CA_INVALID,"A tempo at tick 0 is required");goto done;}
    if(!p->tokens[meters].count){jfail(p,meters,CA_INVALID,"A meter at tick 0 is required");goto done;}
    if(p->tokens[c.drums].count!=4){jfail(p,c.drums,CA_INVALID,"Exactly four drum definitions are required");goto done;}
    size_t cut_count=2+p->tokens[tempos].count+2*p->tokens[ramps].count;if(cut_count>limits->allocated_bytes/(16*(sizeof(int64_t)+sizeof(Tempo)))){*error=CA_LIMIT;goto done;}
    cuts=calloc(cut_count,sizeof(*cuts));c.tempo=calloc(cut_count,sizeof(Tempo));if(!cuts||!c.tempo){*error=CA_MEMORY;goto done;}size_t used=0;cuts[used++]=0;cuts[used++]=c.end;
    size_t at=tempos+1;int zero=0;for(size_t i=0;i<p->tokens[tempos].count;i++){
        if(!jobject(p,at,"tick bpm"))goto done;int64_t t=integer(p,at,"tick",0,c.end-1,&c.ok);number(p,at,"bpm",.001,100000,&c.ok);if(t==0)zero++;
        for(size_t j=2;j<used;j++)if(cuts[j]==t){jfail(p,jget(p,at,"tick"),CA_INVALID,"Duplicate tempo tick");goto done;}cuts[used++]=t;at=p->tokens[at].next;
    }if(zero!=1){jfail(p,tempos,CA_INVALID,"A tempo at tick 0 is required");goto done;}
    at=ramps+1;for(size_t i=0;i<p->tokens[ramps].count;i++){
        if(!jobject(p,at,"from_tick to_tick"))goto done;int64_t a=integer(p,at,"from_tick",0,c.end-1,&c.ok),b=integer(p,at,"to_tick",a+1,c.end-1,&c.ok);int ends=0;
        for(size_t j=2;j<2+p->tokens[tempos].count;j++){if(cuts[j]==a||cuts[j]==b)ends++;if(cuts[j]>a&&cuts[j]<b){jfail(p,at,CA_INVALID,"Tempo ramp contains an interior tempo point");goto done;}}if(ends!=2){jfail(p,at,CA_INVALID,"Tempo ramp requires tempo points at both endpoints");goto done;}
        size_t earlier=ramps+1;for(size_t j=0;j<i;j++){int64_t x=integer(p,earlier,"from_tick",0,c.end-1,&c.ok),y=integer(p,earlier,"to_tick",x+1,c.end-1,&c.ok);if(x<b&&y>a){jfail(p,at,CA_INVALID,"Tempo ramps overlap");goto done;}earlier=p->tokens[earlier].next;}
        cuts[used++]=a;cuts[used++]=b;at=p->tokens[at].next;
    }
    qsort(cuts,used,sizeof(*cuts),cmp64);double seconds=0;
    for(size_t i=1;i<used;i++)if(cuts[i]>cuts[i-1]){
        int64_t a=cuts[i-1],b=cuts[i];double from=bpm(&c,tempos,a),to=from;at=ramps+1;
        for(size_t j=0;j<p->tokens[ramps].count;j++){int64_t x=integer(p,at,"from_tick",0,c.end-1,&c.ok),y=integer(p,at,"to_tick",x+1,c.end-1,&c.ok);if(a>=x&&a<y)to=bpm(&c,tempos,b);at=p->tokens[at].next;}
        double duration=cs_tempo_duration(b-a,from,to,c.ppq);c.tempo[c.tempos++]=(Tempo){a,b,from,to,seconds,duration};seconds+=duration;
    }
    if(!c.ok)goto done;c.length=sample(&c,c.end);if(!c.ok||!c.length)goto done;
    int taken[5]={0};at=sources+1;for(int i=0;i<5;i++){
        if(!jobject(p,at,"id track_id voice wave gain initial_sound dpcm_attack_percent dpcm_decay_ms"))goto done;
        int v=(int)integer(p,at,"voice",0,4,&c.ok);if(!c.ok)goto done;if(taken[v]){jfail(p,jget(p,at,"voice"),CA_INVALID,"Duplicate source voice");goto done;}if(!id(p,jget(p,at,"id"),c.ids[v]))goto done;taken[v]=1;c.initial[v]=sound(&c,jget(p,at,"initial_sound"));c.gain[v]=number(p,at,"gain",0,2,&c.ok);at=p->tokens[at].next;
    }
    if(!c.ok||!validate_items(&c,c.changes,"id source_id tick sound_id",1)||!validate_items(&c,notes,"id source_id at end pitch volume accent staccato tom drum grace_pitches",1)||
       !validate_items(&c,c.dynamics,"id source_id tick volume",1)||
       !validate_items(&c,c.hairpins,"id source_id from_tick to_tick from_volume to_volume",1)||
       !validate_items(&c,c.echoes,"id source_id destination_id from_tick to_tick delay_ticks gain",1)||
       !validate_items(&c,c.drums,"kind tone decay_ms gain",0)||
       !validate_items(&c,meters,"tick numerator denominator",0))goto done;
    at=meters+1;int64_t previous_meter=-1;for(size_t i=0;i<p->tokens[meters].count;i++){
        int64_t t=integer(p,at,"tick",0,c.end-1,&c.ok),den=integer(p,at,"denominator",1,64,&c.ok);integer(p,at,"numerator",1,64,&c.ok);
        if((!i&&t)||t<=previous_meter||(den&(den-1))){jfail(p,at,CA_INVALID,"Meters must start at tick 0, increase in time, and use power-of-two denominators");goto done;}previous_meter=t;at=p->tokens[at].next;
    }
    if(!prepare_controls(&c)||!prepare_echoes(&c))goto done;
    at=c.drums+1;unsigned drum_mask=0;for(size_t i=0;i<4;i++){size_t kind=jget(p,at,"kind");int d=jeq(p,kind,"kick")?0:jeq(p,kind,"snare")?1:jeq(p,kind,"open")?2:jeq(p,kind,"close")?3:-1;if(d<0||(drum_mask&(1u<<d))){jfail(p,kind,CA_INVALID,"Drum kinds kick, snare, open and close must each occur once");goto done;}drum_mask|=1u<<d;integer(p,at,"tone",-127,127,&c.ok);integer(p,at,"decay_ms",5,800,&c.ok);number(p,at,"gain",0,2,&c.ok);at=p->tokens[at].next;}
    if(!c.ok)goto done;
    // Explicit settings changes are copied; source defaults remain fully declared.
    at=c.changes+1;for(size_t i=0;i<p->tokens[c.changes].count;i++){
        c.origin=at;if(!jobject(p,at,"id source_id tick sound_id"))goto done;
        int v=voice(&c,jget(p,at,"source_id"));int64_t t=integer(p,at,"tick",0,c.end-1,&c.ok);size_t snd=sound(&c,jget(p,at,"sound_id"));event(&c,(Event){.at=sample(&c,t),.kind=0,.voice=v,.sound=snd});at=p->tokens[at].next;
    }
    size_t nn=p->tokens[notes].count;if(nn>limits->allocated_bytes/sizeof(size_t)){*error=CA_LIMIT;goto done;}note_tokens=calloc(nn?nn:1,sizeof(size_t));if(!note_tokens){*error=CA_MEMORY;goto done;}at=notes+1;for(size_t i=0;i<nn;i++){note_tokens[i]=at;at=p->tokens[at].next;}
    // Preparation is O(n log n) even for reverse-ordered notes.
    if(nn>limits->allocated_bytes/(8*sizeof(NoteRef)))goto done;
    refs=calloc(nn?nn:1,sizeof(*refs));if(!refs){*error=CA_MEMORY;goto done;}
    for(size_t i=0;i<nn;i++)refs[i]=(NoteRef){note_tokens[i],voice(&c,jget(p,note_tokens[i],"source_id")),integer(p,note_tokens[i],"at",0,c.end-1,&c.ok)};
    if(!c.ok)goto done;qsort(refs,nn,sizeof(*refs),note_compare);for(size_t i=0;i<nn;i++)note_tokens[i]=refs[i].token;
    for(size_t i=0;i<nn&&c.ok;i++){
        at=note_tokens[i];c.origin=at;if(!jobject(p,at,"id source_id at end pitch volume accent staccato tom drum grace_pitches"))goto done;
        int v=voice(&c,jget(p,at,"source_id")),pitch=(int)integer(p,at,"pitch",0,127,&c.ok),base=(int)integer(p,at,"volume",1,15,&c.ok);
        int64_t nt=integer(p,at,"at",0,c.end-1,&c.ok),et=integer(p,at,"end",nt+1,c.end,&c.ok),onset=sample(&c,nt),end=sample(&c,et);
        int accent=boolean(p,at,"accent",&c.ok),staccato=boolean(p,at,"staccato",&c.ok),tom=boolean(p,at,"tom",&c.ok),level=volume_at(&c,v,nt,base);if(accent)level=level+3>15?15:level+3;
        int64_t next=c.length;if(i+1<nn&&voice(&c,jget(p,note_tokens[i+1],"source_id"))==v){int64_t next_tick=integer(p,note_tokens[i+1],"at",0,c.end-1,&c.ok);if(next_tick<et){jfail(p,jget(p,note_tokens[i+1],"at"),CA_INVALID,"Note overlaps the preceding note on the same source");goto done;}next=sample(&c,next_tick);}
        size_t grace=list(p,at,"grace_pitches",limits->events,&c.ok);if(!c.ok)goto done;if(end<=onset){jfail(p,at,CA_INVALID,"Note duration rounds to zero samples");goto done;}
        size_t first=c.count;int initial_level=level;
        if(v<3&&p->tokens[grace].count){size_t ng=p->tokens[grace].count;int64_t total=(end-onset)/4;if(total<1)total=1;if(ng<=(uint64_t)total/1544)total=1544*(int64_t)ng;int64_t begin=onset;size_t gt=grace+1;
            for(size_t j=0;j<ng;j++){int64_t gp;if(!jint(p,gt,0,127,&gp))goto done;int64_t to=begin+total*(int64_t)(j+1)/(int64_t)ng;if(to>onset)event(&c,(Event){.kind=2,.voice=v,.at=onset,.end=to,.pitch=(int)gp,.volume=level});onset=to;gt=p->tokens[gt].next;}}
        int64_t gap=v==2?(end-onset)*15/100:(end-onset)/10;if(v!=2&&gap>(v>=3?441:2205))gap=v>=3?441:2205;
        int64_t z=staccato?onset+((end-onset)/2>0?(end-onset)/2:1):end-gap;if(z<=onset)continue;
        size_t drum=jget(p,at,"drum");int preset=jeq(p,drum,"none")?0:jeq(p,drum,"kick")?1:jeq(p,drum,"snare")?2:jeq(p,drum,"open")?3:jeq(p,drum,"close")?4:-1;if(preset<0){jfail(p,drum,CA_INVALID,"Unknown drum kind");goto done;}
        size_t config=0;if(preset){size_t d=c.drums+1;for(size_t j=0;j<p->tokens[c.drums].count;j++){char a[129],b[129];if(jstring(p,drum,a,129)&&jstring(p,jget(p,d,"kind"),b,129)&&!strcmp(a,b))config=d;d=p->tokens[d].next;}if(!config)goto done;level=(int)llround(level*number(p,config,"gain",0,2,&c.ok));if(!level)continue;if(level>15){jfail(p,at,CA_INVALID,"Drum gain produces a volume greater than 15");c.ok=0;break;}}
        if(v==4){if(preset!=1){jfail(p,drum,CA_INVALID,"Voice 4 requires drum kind kick");goto done;}int64_t tail=integer(p,config,"decay_ms",5,800,&c.ok)*44100/1000;event(&c,(Event){.kind=3,.voice=4,.at=onset,.end=z<onset+tail?z:onset+tail,.volume=level});}
        else if(preset){if(v!=3||preset==1){jfail(p,drum,CA_INVALID,"Noise drums require voice 3; kick requires voice 4");goto done;}ChipAudioNote hits[CHIP_DRUM_MAX_SEGMENTS];size_t count=chip_drum_expand((ChipDrumPreset)preset,0,44100,level,hits,CHIP_DRUM_MAX_SEGMENTS);double denominator=preset==2?80:preset==3?440:50,scale=number(p,config,"decay_ms",5,800,&c.ok)/denominator;int tone=(int)integer(p,config,"tone",-127,127,&c.ok);
            for(size_t j=0;j<count;j++){int64_t a=onset+llround(hits[j].at*scale),b=onset+llround((hits[j].at+hits[j].duration)*scale);if(b>z)b=z;if(b>a){int pch=hits[j].pitch+tone;if(pch<0)pch=0;if(pch>127)pch=127;event(&c,(Event){.kind=2,.voice=v,.at=a,.end=b,.pitch=pch,.volume=hits[j].volume});}}}
        else if(v<2&&tom&&boolean(p,sound_at(&c,v,nt),"tom_enabled",&c.ok)){
            int64_t tail=z<onset+7938?z:onset+7938;for(int64_t t=onset;t<tail;t+=132){size_t snd=sound_at(&c,v,(int64_t)floor(tick_at(&c,t)+1e-7));double age=(double)(t-onset)/44100,depth=number(p,snd,"tom_semitones",0,127,&c.ok),seconds=number(p,snd,"tom_seconds",.001,10,&c.ok);int pitch2=pitch-(int)llround(depth*(1-exp(-age/seconds)));if(pitch2<0)pitch2=0;int vol=(int)llround(level*exp(-age/.045));if(vol<1)vol=1;event(&c,(Event){.kind=2,.voice=v,.at=t,.end=t+132<tail?t+132:tail,.pitch=pitch2,.volume=vol});}}
        else if(v==2){int64_t gate=z-onset,bounds[4]={onset,onset+(gate/4<882?gate/4:882),onset+(gate/2<3528?gate/2:3528),z};int volumes[3]={level,(int)llround(level*.8),(int)llround(level*.6)};for(int j=0;j<3;j++)if(bounds[j+1]>bounds[j])event(&c,(Event){.kind=2,.voice=v,.at=bounds[j],.end=bounds[j+1],.pitch=pitch,.volume=volumes[j]>0?volumes[j]:1});}
        else event(&c,(Event){.kind=2,.voice=v,.at=onset,.end=z,.pitch=pitch,.volume=level});
        // Attach release to the final pulse segment, before expression subdivision.
        if(v<2&&c.count>first){Event *last=&c.events[c.count-1];if(last->kind==2&&last->end<next){size_t snd=sound_at(&c,v,(int64_t)floor(tick_at(&c,last->end)+1e-7));int64_t release=llround(number(p,snd,"release_seconds",0,10,&c.ok)*44100);if(release){last->release=last->end;last->release_samples=release;last->end=release>next-last->end?next:last->end+release;}}}
        if(v<4&&(p->tokens[c.hairpins].count||p->tokens[c.dynamics].count)){
            size_t finish=c.count;for(size_t j=first;j<finish;j++){Event original=c.events[j];if(original.kind!=2)continue;int64_t end2=original.end;c.events[j].end=original.at+441<end2?original.at+441:end2;
                for(int64_t t=original.at;t<end2;t+=441){Event e=original;e.at=t;e.end=t+441<end2?t+441:end2;int vol=volume_at(&c,v,(int64_t)floor(tick_at(&c,t)+1e-7),base);if(accent)vol=vol+3>15?15:vol+3;e.volume=(int)llround((double)original.volume*vol/initial_level);if(e.volume<1)e.volume=1;if(e.volume>15)e.volume=15;if(t==original.at){e.id=c.events[j].id;c.events[j]=e;}else event(&c,e);}}
        }
        if(v<2)event(&c,(Event){.kind=1,.voice=v,.at=onset,.end=end});
        const Echo *echo=v<2?echo_at(&c,v,nt):NULL;
        if(echo && echo->delay<c.end-nt){
            int64_t begin=sample(&c,nt),delay=sample(&c,nt+echo->delay)-begin;
            size_t snd=sound_at(&c,v,et);
            int64_t release=llround(number(p,snd,"release_seconds",0,10,&c.ok)*44100),to=sample(&c,et);
            to=release>next-to?next:to+release;
            event(&c,(Event){.kind=4,.voice=v,.dest=echo->destination,.at=begin,.end=to,.delay=delay,.gain=echo->gain});
        }
    }
    if(!valid_order_ids(&c,jget(p,0,"order"))||!valid_order_tree(&c,jget(p,0,"order"),0))goto done;
    order(&c,jget(p,0,"order"),(CVisit){0},0);if(!c.ok||!c.visits_count)goto done;
    int64_t loop_a=0,loop_b=0;size_t loop=jget(p,0,"loop");if(loop>=p->used)goto done;int has_loop=p->tokens[loop].kind!='n';
    if(has_loop){if(!jobject(p,loop,"from_tick to_tick"))goto done;loop_a=integer(p,loop,"from_tick",0,c.end-1,&c.ok);loop_b=integer(p,loop,"to_tick",loop_a+1,c.end,&c.ok);}
    // Split visits at loop boundaries before selecting the first and last occurrence.
    if(has_loop){CVisit *original=c.visits;size_t count=c.visits_count;c.visits=NULL;c.visits_count=c.visits_capacity=0;for(size_t i=0;i<count;i++){CVisit v=original[i];int64_t marks[4]={v.a,loop_a,loop_b,v.b};qsort(marks,4,sizeof(int64_t),cmp64);int64_t from=v.a;for(int j=0;j<4;j++)if(marks[j]>from&&marks[j]<v.b){CVisit part=v;part.a=from;part.b=marks[j];visit(&c,part);from=marks[j];}v.a=from;visit(&c,v);}free(original);}
    size_t cycle_start=SIZE_MAX,cycle_end=0;int64_t lengths[3]={0};
    for(size_t i=0;i<c.visits_count;i++){CVisit *v=&c.visits[i];int64_t n=sample(&c,v->b)-sample(&c,v->a);if(n<=0||lengths[0]>limits->performance_samples-n)goto done;lengths[0]+=n;if(has_loop&&v->a==loop_a&&cycle_start==SIZE_MAX)cycle_start=i;if(has_loop&&v->b==loop_b)cycle_end=i+1;}
    if(has_loop&&(cycle_start==SIZE_MAX||cycle_end<=cycle_start)){jfail(p,loop,CA_INVALID,"Loop boundaries do not form a nonempty cycle in playback order");goto done;}
    if(has_loop)for(size_t i=cycle_start;i<cycle_end;i++)if(c.visits[i].a<loop_a||c.visits[i].b>loop_b){jfail(p,loop,CA_INVALID,"Loop cycle contains a visit outside its boundaries");goto done;}
    if(!has_loop)cycle_end=c.visits_count;
    for(size_t i=0;i<cycle_end;i++)lengths[1]+=sample(&c,c.visits[i].b)-sample(&c,c.visits[i].a);
    if(has_loop)for(size_t i=cycle_start;i<cycle_end;i++)lengths[2]+=sample(&c,c.visits[i].b)-sample(&c,c.visits[i].a);
    qsort(c.events,c.count,sizeof(Event),event_compare);
    writef(&w,"{\"format\":\"chip-sequence\",\"version\":1,\"song_id\":\"%s\",\"sample_rate\":44100,\"score_samples\":%lld,\"once_samples\":%lld,\"first_pass_samples\":%lld,\"cycle_samples\":%lld,\"sources\":",song,(long long)c.length,(long long)lengths[0],(long long)lengths[1],(long long)lengths[2]);raw(&w,p,sources);writef(&w,",\"sounds\":");raw(&w,p,c.sound_array);writef(&w,",\"events\":[");
    for(size_t i=0;i<c.count;i++){
        Event *e=&c.events[i];const char *kind[]={"sound","onset","segment","dpcm","echo"};if(i)writef(&w,",");writef(&w,"{\"id\":\"e%016llu\",\"kind\":\"%s\",\"sample\":%lld,\"source_id\":\"%s\"",(unsigned long long)e->id,kind[e->kind],(long long)e->at,c.ids[e->voice]);
        if(e->kind==0){writef(&w,",\"sound_id\":");raw(&w,p,jget(p,e->sound,"id"));}
        else {writef(&w,",\"end\":%lld",(long long)e->end);if(e->kind==2)writef(&w,",\"pitch\":%d,\"volume\":%d,\"release_at\":%lld,\"release_samples\":%lld",e->pitch,e->volume,(long long)e->release,(long long)e->release_samples);if(e->kind==3)writef(&w,",\"offset\":%lld,\"volume\":%d",(long long)e->offset,e->volume);if(e->kind==4)writef(&w,",\"destination_id\":\"%s\",\"delay\":%lld,\"gain\":%.17g",c.ids[e->dest],(long long)e->delay,e->gain);}writef(&w,"}");
    }
    writef(&w,"],\"plans\":{");const char *plan_names[]={"once","first_pass","cycle"};
    for(int plan=0;plan<3;plan++){if(plan)writef(&w,",");writef(&w,"\"%s\":[",plan_names[plan]);size_t begin=plan==2?cycle_start:0,finish=plan==0?c.visits_count:cycle_end;if(plan==2&&!has_loop)begin=finish;
        for(size_t i=begin;i<finish;i++){if(i>begin)writef(&w,",");write_visit(&w,&c,&c.visits[i],i);}writef(&w,"]");}
    writef(&w,"},\"positions\":[");for(size_t i=0;i<c.tempos;i++){Tempo *t=&c.tempo[i];if(i)writef(&w,",");writef(&w,"{\"from_tick\":%lld,\"to_tick\":%lld,\"from_sample\":%lld,\"to_sample\":%lld,\"bpm_from\":%.17g,\"bpm_to\":%.17g,\"ppq\":%lld}",(long long)t->a,(long long)t->b,(long long)sample(&c,t->a),(long long)sample(&c,t->b),t->from,t->to,(long long)c.ppq);}writef(&w,"]}");
    if(w.failed)jfail(p,SIZE_MAX,w.failed,"Cannot allocate compiled JSON within memory budget");
    if(c.ok&&!w.failed){CALimits decode=*limits;decode.allocated_bytes/=2;CSDiagnostic generated;
        result=cs_sequence_read_ex(w.data,w.used,&decode,error,&generated);
        if(!result){
            size_t token=SIZE_MAX,index=0;int consumed=0;
            if(sscanf(generated.path,"/events/%zu%n",&index,&consumed)==1&&consumed>0&&index<c.count)
                token=c.events[index].origin;
            else if(!strncmp(generated.path,"/sources",8)||!strncmp(generated.path,"/sounds",7))
                token=jfindpath(p,generated.path);
            else if(!strncmp(generated.path,"/plans",6))token=jget(p,0,"order");
            else if(!strncmp(generated.path,"/positions",10))token=jget(p,0,"tempos");
            else token=jget(p,0,"end_tick");
            if(token==SIZE_MAX)token=0;
            jfail(p,token,*error,"%s",generated.message);
        }}
done:if(!result)jfinish(&c.p,error,"Cannot compile performance: inconsistent notes, controls, or playback order");else dreset(diagnostic);free(c.expression);free(c.sound_control);free(c.echo_control);free(c.p.tokens);free(cuts);free(c.tempo);free(c.events);free(c.visits);free(note_tokens);free(refs);free(w.data);return result;
}
