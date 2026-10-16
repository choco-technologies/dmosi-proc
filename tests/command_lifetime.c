/* Exercise module GC between command creation and process reaping using an
 * allocator that tracks the production SAL ownership tags. */
#include "dmod.h"
#include "dmosi.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
struct allocation {void* p;char owner[128];};
static struct allocation allocations[128];
void* __real_Dmod_MallocEx(size_t size,const char* owner);
void __real_Dmod_FreeEx(void* p,bool concatenate);
void* __wrap_Dmod_MallocEx(size_t size,const char* owner)
{
    void* p=__real_Dmod_MallocEx(size,owner);
    if(!p)return NULL;
    for(unsigned i=0;i<128;++i)if(!allocations[i].p){
        allocations[i].p=p;
        snprintf(allocations[i].owner,sizeof(allocations[i].owner),"%s",owner?owner:"");return p;
    }
    abort();
}
static int live(const void* p)
{for(unsigned i=0;i<128;++i)if(allocations[i].p==p)return 1;return 0;}
void __wrap_Dmod_FreeEx(void* p,bool concatenate)
{
    if(!p)return;
    assert(live(p));
    for(unsigned i=0;i<128;++i)if(allocations[i].p==p)allocations[i].p=NULL;
    __real_Dmod_FreeEx(p,concatenate);
}
bool __wrap_Dmod_RetagEx(void* p,const char* owner)
{
    for(unsigned i=0;i<128;++i)if(allocations[i].p==p){
        snprintf(allocations[i].owner,sizeof(allocations[i].owner),"%s",owner?owner:"");return true;
    }
    return false;
}
void __wrap_Dmod_FreeModule(const char* owner)
{
    for(unsigned i=0;i<128;++i)
        if(allocations[i].p && !strcmp(allocations[i].owner,owner))
            __wrap_Dmod_FreeEx(allocations[i].p,false);
}
int main(void)
{
    for(int args=0;args<2;++args){
        dmosi_process_t process=dmosi_process_create("lifetime","child",NULL);
        assert(process);
        Dmod_Context_t context={0};
        strcpy(context.AllocatorName,"child#lifetime");
        assert(dmosi_process_set_context(process,&context)==0);
        char* argv[]={"child","argument"};
        assert((args?dmosi_process_set_command_args(process,2,argv):
                      dmosi_process_set_command(process,"child argument"))==0);
        const char* command=dmosi_process_get_command(process);
        assert(live(command));
        Dmod_FreeModule(context.AllocatorName);
        assert(live(command));
        assert(!strcmp(command,"child argument"));
        assert(dmosi_process_set_context(process,NULL)==0);
        dmosi_process_destroy(process);
        assert(!live(command));
    }
    for(unsigned i=0;i<128;++i)assert(!allocations[i].p);
    return 0;
}
