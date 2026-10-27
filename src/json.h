// Strict JSON reader and per-call diagnostics. Integer tokens never use double.
#ifndef CS_JSON_H
#define CS_JSON_H
#include "chip-sequencer.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdarg.h>
typedef struct {size_t start,end,next,count;char kind;} JT;
typedef struct {
    const char *text; size_t length,at,used,capacity,context;
    JT *tokens; int failed; CAResult status; CSDiagnostic *diagnostic;
} JP;
static void dreset(CSDiagnostic *d) { if(d) memset(d,0,sizeof(*d)); }
static void dset(CSDiagnostic *d, CAResult code, const char *text, size_t bytes,
                 size_t offset, const char *message) {
    if(!d || d->code!=CA_OK) return;
    d->code=code; d->offset=offset;
    int n=snprintf(d->message,sizeof(d->message),"%s",message);
    if(n<0 || (size_t)n>=sizeof(d->message)) d->truncated=1;
    if(offset!=SIZE_MAX && text) {
        if(offset>bytes) offset=bytes;
        d->offset=offset; d->line=d->column=1;
        for(size_t i=0;i<offset;i++) {
            if(text[i]=='\r') {d->line++;d->column=1;if(i+1<offset&&text[i+1]=='\n')i++;}
            else if(text[i]=='\n') {d->line++;d->column=1;}
            else d->column++;
        }
    }
}
static void dappend(CSDiagnostic *d,const char *s) {
    if(!d)return;
    size_t used=strlen(d->path);
    int n=snprintf(d->path+used,sizeof(d->path)-used,"%s",s);
    if(n<0 || (size_t)n>=sizeof(d->path)-used)d->truncated=1;
}
static void dkey(CSDiagnostic *d,const char *s) {
    dappend(d,"/");
    for(;*s;s++){char c[2]={*s,0};dappend(d,*s=='~'?"~0":*s=='/'?"~1":c);}
}
static void jspace(JP *p){while(p->at<p->length&&strchr(" \r\n\t",p->text[p->at])&&p->text[p->at])p->at++;}
static int jhex(char c){return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;}
static int jsyntax(JP *p,CAResult code,size_t offset,const char *message) {
    if(!p->failed){p->status=code;dset(p->diagnostic,code,p->text,p->length,offset,message);}
    p->failed=1;return 0;
}
static size_t jvalue(JP *p,int depth){
    jspace(p);
    if(depth>64){jsyntax(p,CA_LIMIT,p->at,"JSON nesting exceeds 64 levels");return 0;}
    if(p->at>=p->length){jsyntax(p,CA_INVALID,p->at,"Expected a JSON value");return 0;}
    size_t id=p->used++;
    if(p->tokens&&id>=p->capacity){jsyntax(p,CA_LIMIT,p->at,"JSON token capacity exceeded");return 0;}
    JT t={.start=p->at};char c=p->text[p->at++];t.kind=c;
    if(c=='{'||c=='['){
        jspace(p);char end=c=='{'?'}':']';
        if(p->at<p->length&&p->text[p->at]==end)p->at++;
        else for(;;){
            jspace(p);
            if(c=='{'&&(p->at>=p->length||p->text[p->at]!='"')){jsyntax(p,CA_INVALID,p->at,"Expected an object key in double quotes");break;}
            jvalue(p,depth+1);t.count++;if(p->failed)break;
            if(c=='{'){
                jspace(p);
                if(p->at>=p->length||p->text[p->at]!=':'){jsyntax(p,CA_INVALID,p->at,"Expected ':' after object key");break;}
                p->at++;jvalue(p,depth+1);if(p->failed)break;
            }
            jspace(p);
            if(p->at>=p->length){jsyntax(p,CA_INVALID,p->at,"Unterminated object or array");break;}
            if(p->text[p->at]==end){p->at++;break;}
            if(p->text[p->at]!=','){jsyntax(p,CA_INVALID,p->at,"Expected ',' or closing delimiter");break;}
            p->at++;
        }
    }else if(c=='"'){
        int closed=0;
        while(p->at<p->length){
            size_t at=p->at;unsigned char ch=(unsigned char)p->text[p->at++];
            if(ch=='"'){closed=1;break;}
            if(ch<32||ch>=128){jsyntax(p,CA_INVALID,at,"Expected an ASCII string without control characters");break;}
            if(ch=='\\'){
                if(p->at>=p->length){jsyntax(p,CA_INVALID,p->at,"Incomplete string escape");break;}
                at=p->at;char e=p->text[p->at++];
                if(e=='u'){
                    for(int i=0;i<4;i++){
                        if(p->at>=p->length||jhex(p->text[p->at])<0){jsyntax(p,CA_INVALID,p->at,"Expected four hexadecimal digits after \\u");break;}
                        p->at++;
                    }
                    if(p->failed)break;
                }else if(!e||!strchr("\"\\/bfnrt",e)){jsyntax(p,CA_INVALID,at,"Invalid string escape");break;}
            }
        }
        if(!closed&&!p->failed)jsyntax(p,CA_INVALID,p->at,"Unterminated string");
    }else if(c=='t'||c=='f'||c=='n'){
        const char *word=c=='t'?"true":c=='f'?"false":"null";size_t n=strlen(word);
        if(n>p->length-t.start||memcmp(p->text+t.start,word,n))jsyntax(p,CA_INVALID,t.start,"Invalid JSON literal");
        else p->at=t.start+n;
    }else{
        t.kind='0';p->at=t.start;
        if(p->text[p->at]=='-')p->at++;
        if(p->at>=p->length)jsyntax(p,CA_INVALID,p->at,"Expected a digit");
        else if(p->text[p->at]=='0')p->at++;
        else if(p->text[p->at]>='1'&&p->text[p->at]<='9'){
            do{p->at++;}while(p->at<p->length&&p->text[p->at]>='0'&&p->text[p->at]<='9');
        }else jsyntax(p,CA_INVALID,p->at,"Expected a JSON value");
        if(!p->failed&&p->at<p->length&&p->text[p->at]=='.'){
            size_t begin=++p->at;while(p->at<p->length&&p->text[p->at]>='0'&&p->text[p->at]<='9')p->at++;
            if(p->at==begin)jsyntax(p,CA_INVALID,p->at,"Expected digits after decimal point");
        }
        if(!p->failed&&p->at<p->length&&(p->text[p->at]=='e'||p->text[p->at]=='E')){
            p->at++;if(p->at<p->length&&(p->text[p->at]=='+'||p->text[p->at]=='-'))p->at++;
            size_t begin=p->at;while(p->at<p->length&&p->text[p->at]>='0'&&p->text[p->at]<='9')p->at++;
            if(p->at==begin)jsyntax(p,CA_INVALID,p->at,"Expected exponent digits");
        }
    }
    t.end=p->at;t.next=p->used;if(p->tokens)p->tokens[id]=t;return id;
}
static int jstring(const JP *p,size_t id,char *out,size_t cap){
    if(id>=p->used||!p->tokens||p->tokens[id].kind!='"')return 0;JT t=p->tokens[id];size_t n=0;
    for(size_t i=t.start+1;i+1<t.end;i++){
        unsigned char c=p->text[i];if(c=='\\'){c=p->text[++i];if(c=='u'){unsigned value=0;for(int k=0;k<4;k++)value=value*16+(unsigned)jhex(p->text[++i]);if(value<32||value>=128)return 0;c=(unsigned char)value;}else if(c=='b')c='\b';else if(c=='f')c='\f';else if(c=='n')c='\n';else if(c=='r')c='\r';else if(c=='t')c='\t';}
        if(n+1>=cap)return 0;out[n++]=(char)c;
    }out[n]=0;return 1;
}
static int jeq(const JP *p,size_t id,const char *str){char buf[129];return jstring(p,id,buf,sizeof buf)&&!strcmp(buf,str);}
// Construct paths only on failure, without an extra allocation per token.
static void jpath(const JP *p,size_t target,CSDiagnostic *d){
    if(!d||!p->tokens||target>=p->used)return;
    size_t current=0;
    while(current!=target){
        JT t=p->tokens[current];size_t child=current+1;int found=0;
        for(size_t i=0;i<t.count&&(t.kind=='{'||t.kind=='[');i++){
            size_t value=t.kind=='{'?child+1:child;
            if(target>=child&&target<p->tokens[value].next){
                if(t.kind=='{'){char key[129];if(!jstring(p,child,key,sizeof key)){d->truncated=1;return;}dkey(d,key);}
                else {char index[32];snprintf(index,sizeof index,"/%zu",i);dappend(d,index);}
                if(target==child&&t.kind=='{')return;
                current=value;found=1;break;
            }
            child=p->tokens[value].next;
        }
        if(!found)return;
    }
}
static int jfail(JP *p,size_t token,CAResult code,const char *format,...){
    if(p->status!=CA_OK)return 0;
    p->status=code;
    if(p->diagnostic){
        char message[256];va_list ap;va_start(ap,format);int n=vsnprintf(message,sizeof message,format,ap);va_end(ap);
        size_t at=p->tokens&&token<p->used?p->tokens[token].start:SIZE_MAX;
        dset(p->diagnostic,code,p->text,p->length,at,message);
        if(n<0||(size_t)n>=sizeof message)p->diagnostic->truncated=1;
        jpath(p,token,p->diagnostic);
    }
    return 0;
}
static size_t jget(JP *p,size_t object,const char *key){
    p->context=object;
    if(object>=p->used||p->tokens[object].kind!='{')return SIZE_MAX;
    size_t at=object+1;for(size_t i=0;i<p->tokens[object].count;i++){size_t value=at+1;if(jeq(p,at,key)){p->context=value;return value;}at=p->tokens[value].next;}return SIZE_MAX;
}
static size_t jrequired(JP *p,size_t object,const char *key){
    size_t value=jget(p,object,key);
    if(value==SIZE_MAX){
        int first=p->status==CA_OK;
        jfail(p,object,CA_INVALID,"Missing required field '%s'",key);
        if(first)dkey(p->diagnostic,key);
    }
    return value;
}
static int jobject(JP *p,size_t object,const char *keys){
    p->context=object;
    if(object>=p->used||p->tokens[object].kind!='{')return jfail(p,object,CA_INVALID,"Expected an object");
    size_t at=object+1;
    for(size_t i=0;i<p->tokens[object].count;i++){
        char key[129];if(!jstring(p,at,key,sizeof key))return jfail(p,at,CA_INVALID,"Invalid or overlong object key");
        int found=0;for(const char *s=keys;*s;){const char *e=strchr(s,' ');size_t n=e?(size_t)(e-s):strlen(s);if(strlen(key)==n&&!memcmp(key,s,n))found=1;s=e?e+1:s+n;}
        if(!found)return jfail(p,at,CA_INVALID,"Unknown field '%s'",key);
        size_t prev=object+1;for(size_t j=0;j<i;j++){if(jeq(p,prev,key))return jfail(p,at,CA_INVALID,"Duplicate field '%s'",key);prev=p->tokens[prev+1].next;}
        at=p->tokens[at+1].next;
    }
    for(const char *s=keys;*s;){
        const char *e=strchr(s,' ');size_t n=e?(size_t)(e-s):strlen(s);char key[129];
        memcpy(key,s,n);key[n]=0;
        if(jget(p,object,key)==SIZE_MAX){
            int first=p->status==CA_OK;jfail(p,object,CA_INVALID,"Missing required field '%s'",key);
            if(first)dkey(p->diagnostic,key);return 0;
        }
        s=e?e+1:s+n;
    }
    p->context=object;return 1;
}
static int jint(JP *p,size_t id,int64_t lo,int64_t hi,int64_t *out){
    p->context=id;
    if(id>=p->used||p->tokens[id].kind!='0')return jfail(p,id,CA_INVALID,"Expected an integer");
    JT t=p->tokens[id];size_t i=t.start;int negative=p->text[i]=='-';if(negative)i++;
    uint64_t v=0,limit=negative?(uint64_t)INT64_MAX+1:INT64_MAX;
    for(;i<t.end;i++){unsigned d=(unsigned)(p->text[i]-'0');if(d>9)return jfail(p,id,CA_INVALID,"Expected an integer without fraction or exponent");if(v>(limit-d)/10)return jfail(p,id,CA_INVALID,"Integer exceeds signed 64-bit range");v=v*10+d;}
    int64_t value=negative?(v==(uint64_t)INT64_MAX+1?INT64_MIN:-(int64_t)v):(int64_t)v;
    if(value<lo||value>hi)return jfail(p,id,CA_INVALID,"Expected integer in [%lld, %lld]",(long long)lo,(long long)hi);
    *out=value;return 1;
}
static int jnumber(JP *p,size_t id,double lo,double hi,double *out){
    p->context=id;
    if(id>=p->used||p->tokens[id].kind!='0')return jfail(p,id,CA_INVALID,"Expected a number");
    JT t=p->tokens[id];char buf[128];size_t n=t.end-t.start;
    if(n>=sizeof buf)return jfail(p,id,CA_INVALID,"Number token is too long");
    memcpy(buf,p->text+t.start,n);buf[n]=0;char *end;errno=0;double v=strtod(buf,&end);
    if(errno||*end||!isfinite(v)||v<lo||v>hi)return jfail(p,id,CA_INVALID,"Expected finite number in [%.17g, %.17g]",lo,hi);
    *out=v;return 1;
}
static int jparse(JP *p,const char *text,size_t bytes,size_t budget,CSDiagnostic *diagnostic){
    *p=(JP){.text=text,.length=bytes,.diagnostic=diagnostic};
    if(!bytes)return jsyntax(p,CA_INVALID,0,"Expected a JSON document");
    jvalue(p,0);jspace(p);if(p->failed)return 0;
    if(p->at!=bytes)return jsyntax(p,CA_INVALID,p->at,"Unexpected data after JSON value");
    if(p->used>budget/sizeof(JT))return jsyntax(p,CA_LIMIT,SIZE_MAX,"JSON tokens exceed memory budget");
    size_t count=p->used;p->tokens=calloc(count,sizeof(JT));
    if(!p->tokens)return jsyntax(p,CA_MEMORY,SIZE_MAX,"Cannot allocate JSON tokens");
    p->capacity=count;p->at=p->used=0;jvalue(p,0);jspace(p);return !p->failed&&p->at==bytes;
}
static int jduplicate(JP *p,size_t array,const char *name){
    size_t at=array+1,seen=0;
    for(size_t i=0;i<p->tokens[array].count;i++){
        size_t key=jget(p,at,"id");
        if(jeq(p,key,name)&&seen++)return jfail(p,key,CA_INVALID,"Duplicate id '%s'",name);
        at=p->tokens[at].next;
    }
    return jfail(p,array,CA_INVALID,"Duplicate id '%s'",name);
}
// Resolve diagnostic pointers for fields copied into compiler output. Errors in
// generated events are mapped separately to their originating input object.
static size_t jfindpath(JP *p,const char *path){
    size_t token=0;
    while(*path){
        if(*path++!='/')return SIZE_MAX;
        char key[512];size_t used=0;
        while(*path&&*path!='/'){
            char c=*path++;
            if(c=='~'&&(*path=='0'||*path=='1'))c=*path++=='0'?'~':'/';
            if(used+1>=sizeof key)return SIZE_MAX;key[used++]=c;
        }
        key[used]=0;
        if(token>=p->used)return SIZE_MAX;
        if(p->tokens[token].kind=='{')token=jget(p,token,key);
        else if(p->tokens[token].kind=='['){
            char *end;errno=0;unsigned long long index=strtoull(key,&end,10);
            if(errno||!*key||*end||index>=p->tokens[token].count)return SIZE_MAX;
            token++;while(index--)token=p->tokens[token].next;
        }else return SIZE_MAX;
    }
    return token;
}
static void jfinish(JP *p,CAResult *error,const char *fallback){
    if(p->status==CA_OK)jfail(p,error&&(*error==CA_MEMORY||*error==CA_LIMIT)?SIZE_MAX:p->context,error&&*error!=CA_OK?*error:CA_INVALID,"%s",
        error&&*error==CA_MEMORY?"Memory allocation failed":
        error&&*error==CA_LIMIT?"Configured resource limit exceeded":fallback);
    if(error)*error=p->status;
}
#endif
