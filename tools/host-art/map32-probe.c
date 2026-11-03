#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
int main(void) {
 size_t n=(size_t)sysconf(_SC_PAGESIZE); int bad=0;
 void *p=mmap(0,n,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
 if(p==MAP_FAILED){perror("MAP_32BIT");return 2;}
 bad=(uintptr_t)p+n>UINT32_MAX;
 printf("MAP_32BIT_below_4GiB=%d\n",!bad);munmap(p,n);
 p=mmap((void*)0x20000000UL,n,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 if(p==MAP_FAILED){perror("low hint");return 3;}
 printf("nonfixed_low_hint_below_4GiB=%d\n",(uintptr_t)p+n<=UINT32_MAX);munmap(p,n);
 return bad?1:0;
}
