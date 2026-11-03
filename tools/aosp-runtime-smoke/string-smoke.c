#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/utsname.h>
static unsigned cases;
static char *(* volatile functions[3])(char *, const char *) = {strcpy, stpcpy, strcat};
static const char *names[3] = {"strcpy", "stpcpy", "strcat"};
static void test(unsigned which, unsigned char *dest, unsigned char *src,
                 unsigned length, unsigned prefix, unsigned char *lo, unsigned char *hi) {
    for (unsigned char *p=lo;p<hi;++p) *p=0xa5;
    for (unsigned i=0;i<length;++i) src[i]=(unsigned char)('a'+i%23);
    src[length]=0;
    if(which==2){ for(unsigned i=0;i<prefix;++i)dest[i]='P'; dest[prefix]=0; }
    else prefix=0;
    char *ret=functions[which]((char*)dest,(char*)src);
    char *expected=(char*)dest+(which==1?length:0);
    int ok=ret==expected;
    for(unsigned char *p=lo;p<hi;++p){
        unsigned char expected_byte=0xa5;
        if(p>=dest && p<dest+prefix) expected_byte='P';
        else if(p>=dest+prefix && p<=dest+prefix+length)
            expected_byte=(p==dest+prefix+length)?0:(unsigned char)('a'+(p-dest-prefix)%23);
        if(*p!=expected_byte){ok=0;break;}
    }
    if(!ok){fprintf(stderr,"STRING_FAIL function=%s length=%u prefix=%u case=%u\n",names[which],length,prefix,cases);exit(1);}
    ++cases;
}
int main(void){
    struct utsname uts;
    if(uname(&uts)||strcmp(uts.machine,"armv7l")||!strstr(uts.release,"-pw3-core-smoke"))return 77;
    setbuf(stdout,NULL);
    unsigned char s[1152] __attribute__((aligned(16))), d[1184] __attribute__((aligned(16)));
    size_t page=(size_t)sysconf(_SC_PAGESIZE);
    unsigned char *source=mmap(NULL,page*3,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    unsigned char *dest=mmap(NULL,page*3,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(source==MAP_FAILED||dest==MAP_FAILED||mprotect(source+page,page,PROT_READ|PROT_WRITE)||mprotect(dest+page,page,PROT_READ|PROT_WRITE))return 2;
    for(unsigned f=0;f<3;++f){
        printf("STRING_START %s\n",names[f]);
        for(unsigned n=0;n<=128;++n)
            for(unsigned sa=0;sa<8;++sa)
                for(unsigned da=0;da<8;++da)
                    test(f,d+16+da,s+sa,n,f==2?(sa+da)%8:0,d,d+sizeof(d));
        const unsigned sizes[]={255,256,257,511,512,513,1023,1024};
        for(unsigned i=0;i<sizeof(sizes)/sizeof(sizes[0]);++i)
            for(unsigned sa=0;sa<8;++sa)
                for(unsigned da=0;da<8;++da)
                    test(f,d+16+da,s+sa,sizes[i],f==2?(sa+da)%8:0,d,d+sizeof(d));
        for(unsigned n=0;n<512;++n){
            unsigned prefix=f==2?n%8:0;
            test(f,dest+page*2-n-prefix-1,source+page*2-n-1,n,prefix,dest+page,dest+page*2);
            test(f,dest+page,source+page,n,prefix,dest+page,dest+page*2);
        }
        printf("STRING_PASS %s\n",names[f]);
    }
    munmap(source,page*3);munmap(dest,page*3);
    printf("PW3_STRING_SMOKE_PASS cases=%u\n",cases);
    return 0;
}
