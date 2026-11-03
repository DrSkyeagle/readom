/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef PW3_PM_LOGIC_H
#define PW3_PM_LOGIC_H
#include <string.h>
static int token_present(const char *text,const char *token) {
    size_t n=strlen(token);
    for(const char *p=text;*p;) {
        while(*p==' '||*p=='\n'||*p=='\t')p++;
        const char *end=p;while(*end && *end!=' ' && *end!='\n' && *end!='\t')end++;
        if((size_t)(end-p)==n && !memcmp(p,token,n))return 1;
        p=end;
    }
    return 0;
}
static int selected_level(const char *text,const char *wanted) {
    const char *p=strchr(text,'[');size_t n=strlen(wanted);
    return p && (p==text || p[-1]==' ') && !strchr(p+1,'[') && !strncmp(p+1,wanted,n) && p[n+1]==']' && (!p[n+2] || p[n+2]==' ' || p[n+2]=='\n') && !strchr(p+n+2,']');
}
static const char *new_log_suffix(const char *before,const char *after) {
    size_t n=strlen(before);const size_t marker=128;
    if(n<marker)return NULL;
    const char *tail=before+n-marker,*at=strstr(after,tail);
    if(!at || strstr(at+1,tail))return NULL;
    return at+marker;
}
static int pm_log_has_error(const char *text) {
    const char *bad[]={"failed to resume","failed to suspend","Some devices failed","Freezing of tasks failed","Kernel panic","Oops:","BUG:","Flush updates timeout","No powerdown received","failed to disable 3p0 regulator","failed to enable 3p0 regulator"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++)if(strstr(text,bad[i]))return 1;
    return 0;
}
#endif
