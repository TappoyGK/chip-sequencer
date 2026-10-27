#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
int LLVMFuzzerTestOneInput(const uint8_t *,size_t);
int main(int argc,char **argv){
    unsigned seed=123456789;size_t count=0;
    for(int a=1;a<argc;a++){
        FILE *f=fopen(argv[a],"rb");if(!f)return 1;
        fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);
        uint8_t *base=malloc(n+1),*buf=malloc(n+1);
        if(!base||!buf||fread(base,1,n,f)!=(size_t)n)return 1;fclose(f);
        for(size_t i=0;i<(size_t)n;i++){LLVMFuzzerTestOneInput(base,i);count++;}
        for(size_t i=0;i<10000;i++){
            memcpy(buf,base,n);
            for(int j=0;j<1+(int)(i%8);j++){seed=seed*1664525+1013904223;size_t pos=seed%(size_t)n;seed=seed*1664525+1013904223;buf[pos]=(uint8_t)(seed>>24);}
            LLVMFuzzerTestOneInput(buf,n);count++;
        }
        free(base);free(buf);
    }
    printf("%zu truncated/mutated JSON inputs: PASS\n",count);return 0;
}
