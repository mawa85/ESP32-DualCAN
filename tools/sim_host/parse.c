/* Syntax-Check: kompiliert eine .be-Datei mit dem Firmware-Berry (Bindings als Dummys). */
#include <stdio.h>
#include <stdlib.h>
#include "berry.h"
static int dummy(bvm*vm){be_return_nil(vm);}
int main(int c,char**v){FILE*f=fopen(v[1],"rb");fseek(f,0,2);long n=ftell(f);rewind(f);char*b=malloc(n+1);fread(b,1,n,f);b[n]=0;
bvm*vm=be_vm_new(); const char*g[]={"millis","can_send_raw","can_recv_raw","timer_every","timer_cancel","action_register",0}; for(int i=0;g[i];i++) be_regfunc(vm,g[i],dummy); if(be_loadbuffer(vm,v[1],b,n)){printf("%s\n",be_tostring(vm,-1));return 1;} printf("ok\n");return 0;}
