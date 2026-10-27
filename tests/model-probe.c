#include "chip-sequencer.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
int main(int argc,char **argv) {
    if (argc<2) return 2;
    CALimits l={.input_bytes=16<<20,.allocated_bytes=256<<20,.events=1000000,
        .visits=100000,.checkpoints=128,.repeat_depth=16,
        .score_samples=INT64_C(44100)*86400,.performance_samples=INT64_C(44100)*86400};
    FILE *f=fopen(argv[1],"rb");if (!f) return 2;
    fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);
    if (size<0 || (size_t)size>l.input_bytes) {fclose(f);return 2;}
    char *text=malloc((size_t)size);if (!text) {fclose(f);return 2;}
    size_t read=fread(text,1,(size_t)size,f);fclose(f);
    if (read!=(size_t)size) {free(text);return 2;}
    CAResult result;clock_t start=clock();
    CSModel *m=cs_model_read(text,read,&l,&result);
    double seconds=(double)(clock()-start)/CLOCKS_PER_SEC;
    printf("{\"accepted\":%d,\"result\":%d,\"cpu_seconds\":%.9f}\n",m!=NULL,result,seconds);
    int ok=1;
    if (m && argc>2) {
        CSSequence *s=cs_compile(m,&l,&result);size_t bytes=0;
        ok=s&&cs_sequence_write(s,NULL,0,&bytes)==CA_OK;
        char *out=ok?malloc(bytes):NULL;
        ok=out&&cs_sequence_write(s,out,bytes,&bytes)==CA_OK;
        f=ok?fopen(argv[2],"wb"):NULL;
        ok=f&&fwrite(out,1,bytes,f)==bytes;
        if (f&&fclose(f)) ok=0;
        free(out);cs_sequence_destroy(s);
    }
    cs_model_destroy(m);free(text);return ok?0:2;
}
