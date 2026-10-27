#include "chip-sequencer.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc,char **argv){assert(argc>=2);FILE *f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);char *data=malloc(n);assert(data);assert(fread(data,1,n,f)==(size_t)n);fclose(f);
CALimits l={0};l.input_bytes=16<<20;l.allocated_bytes=256<<20;l.events=1000000;l.visits=100000;l.checkpoints=128;l.repeat_depth=16;l.score_samples=l.performance_samples=INT64_C(44100)*86400;
CAResult error;CSModel *model=cs_model_read(data,n,&l,&error);if(!model)fprintf(stderr,"model error %d\n",error);assert(model);CSSequence *sequence=cs_compile(model,&l,&error);if(!sequence)fprintf(stderr,"compile error %d\n",error);assert(sequence);size_t bytes;assert(cs_sequence_write(sequence,NULL,0,&bytes)==CA_OK);char *out=malloc(bytes+1);assert(out);assert(cs_sequence_write(sequence,out,bytes,&bytes)==CA_OK);out[bytes]=0;if(argc>2){FILE *f=fopen(argv[2],"wb");assert(f);assert(fwrite(out,1,bytes,f)==bytes);fclose(f);}else puts(out);
free(out);cs_sequence_destroy(sequence);cs_model_destroy(model);free(data);return 0;}
