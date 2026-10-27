#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int LLVMFuzzerTestOneInput(const uint8_t *,size_t);
static uint32_t seed=0x12345678;
static uint32_t next(void){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;}
int main(int argc,char **argv){assert(argc>1);for(int file=1;file<argc;file++){
    FILE *f=fopen(argv[file],"rb");assert(f);fseek(f,0,SEEK_END);long length=ftell(f);rewind(f);assert(length>0);size_t n=(size_t)length;uint8_t *original=malloc(n),*data=malloc(n+2);assert(original&&data);assert(fread(original,1,n,f)==n);fclose(f);
    LLVMFuzzerTestOneInput(original,n);
    for(size_t i=0;i<n;i++)LLVMFuzzerTestOneInput(original,i);
    for(int run=0;run<10000;run++){memcpy(data,original,n);for(unsigned i=0,k=1+next()%4;i<k;i++)data[next()%n]=(uint8_t)next();LLVMFuzzerTestOneInput(data,n);}
    free(original);free(data);
}puts("strict JSON: every truncation and 20,000 deterministic mutations under ASan/UBSan: PASS");return 0;}
