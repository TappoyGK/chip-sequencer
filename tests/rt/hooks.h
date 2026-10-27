#include <stdlib.h>
void *cs_checked_malloc(size_t);
void *cs_checked_calloc(size_t,size_t);
void *cs_checked_realloc(void *,size_t);
void cs_checked_free(void *);
#define malloc cs_checked_malloc
#define calloc cs_checked_calloc
#define realloc cs_checked_realloc
#define free cs_checked_free
