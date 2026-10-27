#include "chip-sequencer.h"
extern int cs_realtime;
static CAResult rt_render(CAMixer *m,size_t n,const CAOutput *o){cs_realtime=1;CAResult r=ca_mixer_render(m,n,o);cs_realtime=0;return r;}
static CAResult rt_advance(CAMixer *m,size_t n){cs_realtime=1;CAResult r=ca_mixer_advance(m,n);cs_realtime=0;return r;}
static CAResult rt_edit(CAMixer *m,CAHandle h,CSEdit *e,int64_t at,CACommandID *id){cs_realtime=1;CAResult r=cs_apply_edit(m,h,e,at,id);cs_realtime=0;return r;}
static CAResult rt_seek(CAMixer *m,CAHandle h,CASeek *e,int64_t at,CACommandID *id){cs_realtime=1;CAResult r=cs_seek(m,h,e,at,id);cs_realtime=0;return r;}
static CARetired *rt_collect(CAMixer *m){cs_realtime=1;CARetired *r=ca_collect(m);cs_realtime=0;return r;}
#define ca_mixer_render rt_render
#define ca_mixer_advance rt_advance
#define cs_apply_edit rt_edit
#define cs_seek rt_seek
#define ca_collect rt_collect
