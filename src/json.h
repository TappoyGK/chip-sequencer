// Strict JSON token reader. Integer tokens are never converted through double.
#ifndef CS_JSON_H
#define CS_JSON_H
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <limits.h>
typedef struct {size_t start,end,next,count;char kind;} JT;
typedef struct {const char *text;size_t length,at,used,capacity;JT *tokens;int failed;} JP;
static void jspace(JP *p){while(p->at<p->length&&(p->text[p->at]==' '||p->text[p->at]=='\r'||p->text[p->at]=='\n'||p->text[p->at]=='\t'))p->at++;}
static int jhex(char c){return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;}
static size_t jvalue(JP *p,int depth){
    if(depth>64||p->at>=p->length){p->failed=1;return 0;}
    jspace(p);if(p->at>=p->length){p->failed=1;return 0;}size_t id=p->used++;if(id>=p->capacity&&p->tokens){p->failed=1;return 0;}
    JT t={.start=p->at};char c=p->text[p->at++];t.kind=c;
    if(c=='{'||c=='['){
        jspace(p);char end=c=='{'?'}':']';
        if(p->at<p->length&&p->text[p->at]==end)p->at++;
        else for(;;){
            jspace(p);if(c=='{'&&(p->at>=p->length||p->text[p->at]!='"')){p->failed=1;break;}
            jvalue(p,depth+1);t.count++;if(p->failed)break;
            if(c=='{'){jspace(p);if(p->at>=p->length||p->text[p->at++]!=':'){p->failed=1;break;}jspace(p);jvalue(p,depth+1);if(p->failed)break;}
            jspace(p);if(p->at>=p->length){p->failed=1;break;}char next=p->text[p->at++];if(next==end)break;if(next!=','){p->failed=1;break;}
        }
    }else if(c=='"'){
        int closed=0;
        while(p->at<p->length){unsigned char ch=(unsigned char)p->text[p->at++];if(ch=='"'){closed=1;break;}if(ch<32||ch>=128){p->failed=1;break;}
            if(ch=='\\'){if(p->at>=p->length){p->failed=1;break;}char e=p->text[p->at++];if(e=='u'){for(int i=0;i<4;i++)if(p->at>=p->length||jhex(p->text[p->at++])<0){p->failed=1;break;}}else if(!e||!strchr("\"\\/bfnrt",e)){p->failed=1;break;}}
        }if(!closed)p->failed=1;
    }else if(c=='t'||c=='f'||c=='n'){
        const char *word=c=='t'?"true":c=='f'?"false":"null";size_t n=strlen(word);if(t.start+n>p->length||memcmp(p->text+t.start,word,n))p->failed=1;else p->at=t.start+n;
    }else{
        t.kind='0';size_t i=t.start;if(p->text[i]=='-')i++;
        if(i>=p->length)p->failed=1;
        else if(p->text[i]=='0')i++;
        else if(p->text[i]>='1'&&p->text[i]<='9'){do{i++;}while(i<p->length&&p->text[i]>='0'&&p->text[i]<='9');}
        else p->failed=1;
        if(i<p->length&&p->text[i]=='.'){i++;size_t begin=i;while(i<p->length&&p->text[i]>='0'&&p->text[i]<='9')i++;if(i==begin)p->failed=1;}
        if(i<p->length&&(p->text[i]=='e'||p->text[i]=='E')){i++;if(i<p->length&&(p->text[i]=='+'||p->text[i]=='-'))i++;size_t begin=i;while(i<p->length&&p->text[i]>='0'&&p->text[i]<='9')i++;if(i==begin)p->failed=1;}
        p->at=i;
    }
    t.end=p->at;t.next=p->used;if(p->tokens)p->tokens[id]=t;return id;
}
static int jstring(const JP *p,size_t id,char *out,size_t cap){
    if(id>=p->used||p->tokens[id].kind!='"')return 0;JT t=p->tokens[id];size_t n=0;
    for(size_t i=t.start+1;i+1<t.end;i++){
        unsigned char c=p->text[i];if(c=='\\'){c=p->text[++i];if(c=='u'){unsigned value=0;for(int k=0;k<4;k++)value=value*16+(unsigned)jhex(p->text[++i]);if(value<32||value>=128)return 0;c=(unsigned char)value;}else if(c=='b')c='\b';else if(c=='f')c='\f';else if(c=='n')c='\n';else if(c=='r')c='\r';else if(c=='t')c='\t';}
        if(n+1>=cap)return 0;out[n++]=(char)c;
    }out[n]=0;return 1;
}
static int jeq(const JP *p,size_t id,const char *str){char buf[129];return jstring(p,id,buf,sizeof buf)&&!strcmp(buf,str);}
static size_t jget(const JP *p,size_t object,const char *key){
    if(object>=p->used||p->tokens[object].kind!='{')return SIZE_MAX;
    size_t at=object+1;for(size_t i=0;i<p->tokens[object].count;i++){size_t value=at+1;if(jeq(p,at,key))return value;at=p->tokens[value].next;}return SIZE_MAX;
}
static int jobject(const JP *p,size_t object,const char *keys){
    if(object>=p->used||p->tokens[object].kind!='{')return 0;
    size_t expected=1;for(const char *s=keys;*s;s++)if(*s==' ')expected++;
    if(p->tokens[object].count!=expected)return 0;
    size_t at=object+1;
    for(size_t i=0;i<expected;i++){
        char key[129];if(!jstring(p,at,key,sizeof key))return 0;
        int found=0;for(const char *s=keys;*s;){const char *e=strchr(s,' ');size_t n=e?(size_t)(e-s):strlen(s);if(strlen(key)==n&&!memcmp(key,s,n))found=1;s=e?e+1:s+n;}
        if(!found)return 0;
        size_t prev=object+1;for(size_t j=0;j<i;j++){if(jeq(p,prev,key))return 0;prev=p->tokens[prev+1].next;}
        at=p->tokens[at+1].next;
    }return 1;
}
static int jint(const JP *p,size_t id,int64_t lo,int64_t hi,int64_t *out){
    if(id>=p->used||p->tokens[id].kind!='0')return 0;JT t=p->tokens[id];size_t i=t.start;int negative=p->text[i]=='-';if(negative)i++;
    uint64_t v=0,limit=negative?(uint64_t)INT64_MAX+1:INT64_MAX;
    for(;i<t.end;i++){unsigned d=(unsigned)(p->text[i]-'0');if(d>9||v>(limit-d)/10)return 0;v=v*10+d;}
    int64_t value=negative?(v==(uint64_t)INT64_MAX+1?INT64_MIN:-(int64_t)v):(int64_t)v;
    if(value<lo||value>hi)return 0;*out=value;return 1;
}
static int jnumber(const JP *p,size_t id,double lo,double hi,double *out){
    if(id>=p->used||p->tokens[id].kind!='0')return 0;JT t=p->tokens[id];char buf[128];size_t n=t.end-t.start;if(n>=sizeof buf)return 0;memcpy(buf,p->text+t.start,n);buf[n]=0;char *end;errno=0;double v=strtod(buf,&end);if(errno||*end||!isfinite(v)||v<lo||v>hi)return 0;*out=v;return 1;
}
static int jparse(JP *p,const char *text,size_t bytes,size_t budget){
    *p=(JP){.text=text,.length=bytes};if(!bytes)return 0;jspace(p);jvalue(p,0);jspace(p);if(p->failed||p->at!=bytes||p->used>budget/sizeof(JT))return 0;
    size_t count=p->used;p->tokens=calloc(count,sizeof(JT));if(!p->tokens)return 0;p->capacity=count;p->at=p->used=0;jspace(p);jvalue(p,0);jspace(p);return !p->failed&&p->at==bytes;
}
#endif
