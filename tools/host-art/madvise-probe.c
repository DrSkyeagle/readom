#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
int main(void) {
 const size_t size=65536; size_t changed=0;
 volatile unsigned char *p=mmap(0,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 if(p==MAP_FAILED){perror("mmap");return 2;}
 memset((void*)p,0xa5,size);
 int rc=madvise((void*)p,size,MADV_DONTNEED);
 for(size_t i=0;i<size;i++)if(p[i])changed++;
 printf("madvise_return=%d zeroed=%d nonzero_bytes=%zu\n",rc,changed==0,changed);
 munmap((void*)p,size);return rc||changed?1:0;
}
