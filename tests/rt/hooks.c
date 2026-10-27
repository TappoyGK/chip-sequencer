#include <stdlib.h>
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
int cs_realtime;
static size_t live,peak,allocations;
typedef union {max_align_t align;size_t size;} Header;
void *cs_checked_malloc(size_t n){assert(!cs_realtime);if(n>SIZE_MAX-sizeof(Header))return NULL;Header *h=malloc(sizeof(*h)+n);if(!h)return NULL;h->size=n;live+=n;if(live>peak)peak=live;allocations++;return h+1;}
void *cs_checked_calloc(size_t n,size_t s){assert(!cs_realtime);if(s&&n>SIZE_MAX/s)return NULL;void *p=cs_checked_malloc(n*s);if(p)memset(p,0,n*s);return p;}
void cs_checked_free(void *p){assert(!cs_realtime);if(!p)return;Header *h=(Header *)p-1;live-=h->size;free(h);}
void *cs_checked_realloc(void *p,size_t n){assert(!cs_realtime);if(!p)return cs_checked_malloc(n);if(!n){cs_checked_free(p);return NULL;}void *q=cs_checked_malloc(n);if(q){Header *h=(Header *)p-1;memcpy(q,p,h->size<n?h->size:n);cs_checked_free(p);}return q;}
__attribute__((destructor)) static void report(void){printf("instrumented library allocations: %zu, peak payload bytes: %zu, remaining: %zu\n",allocations,peak,live);assert(!live);}
