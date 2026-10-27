#include "chip-sequencer.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void string(const char *s){
    putchar('"');for(;*s;s++){unsigned char c=(unsigned char)*s;
        if(c=='"'||c=='\\'){putchar('\\');putchar(c);}
        else if(c<32)printf("\\u%04x",c);else putchar(c);
    }putchar('"');
}
int main(int argc,char **argv){
    assert(argc>=3);FILE *f=fopen(argv[2],"rb");assert(f);
    fseek(f,0,SEEK_END);long size=ftell(f);assert(size>=0);rewind(f);
    char *text=malloc((size_t)size+1);assert(text);assert(fread(text,1,(size_t)size,f)==(size_t)size);fclose(f);
    CALimits l={.input_bytes=16<<20,.allocated_bytes=256<<20,.events=1000000,.visits=100000,
        .checkpoints=128,.repeat_depth=16,.score_samples=INT64_C(44100)*86400,.performance_samples=INT64_C(44100)*86400};
    if(argc>3){if(!strcmp(argv[3],"bytes"))l.input_bytes=1;else if(!strcmp(argv[3],"memory"))l.allocated_bytes=1;else if(!strcmp(argv[3],"events"))l.events=0;}
    int model=!strcmp(argv[1],"model");CAResult e;CSDiagnostic d;memset(&d,0xa5,sizeof d);
    CSModel *m=NULL;CSSequence *s=NULL;
    if(model)m=cs_model_read_ex(text,(size_t)size,&l,&e,&d);else s=cs_sequence_read_ex(text,(size_t)size,&l,&e,&d);
    int accepted=m||s;assert(d.code==e);assert(accepted==(e==CA_OK));
    if(accepted){CSDiagnostic zero={0};assert(!memcmp(&d,&zero,sizeof d));}
    else assert(d.message[0]);
    CSDiagnostic saved=d,second;CAResult ordinary;
    if(model){CSModel *v=cs_model_read_ex(text,(size_t)size,&l,NULL,&second);cs_model_destroy(v);v=cs_model_read(text,(size_t)size,&l,&ordinary);cs_model_destroy(v);}
    else{CSSequence *v=cs_sequence_read_ex(text,(size_t)size,&l,NULL,&second);cs_sequence_destroy(v);v=cs_sequence_read(text,(size_t)size,&l,&ordinary);cs_sequence_destroy(v);}
    assert(!memcmp(&saved,&d,sizeof d)&&!memcmp(&saved,&second,sizeof d)&&ordinary==e);
    printf("{\"accepted\":%d,\"code\":%d,\"offset\":%zu,\"line\":%zu,\"column\":%zu,\"truncated\":%d,\"path\":",accepted,e,d.offset,d.line,d.column,d.truncated);
    string(d.path);printf(",\"message\":");string(d.message);puts("}");
    cs_model_destroy(m);cs_sequence_destroy(s);free(text);return 0;
}
