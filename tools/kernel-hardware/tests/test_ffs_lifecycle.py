from pathlib import Path
import argparse,hashlib,os,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
# The one stock file this reads, by argument or by the variable
# kernel-hardware/tests/run_tests.py sets. tools/run-tests.py gives the
# argument, out of the pinned archive, so the stock side needs no kernel tree.
ap=argparse.ArgumentParser(description=__doc__ if __doc__ else None)
ap.add_argument('--kernel-source',type=Path,default=os.environ.get('PW3_KERNEL_SOURCE_DIR'))
ap.add_argument('--cc',default=os.environ.get('CC','clang'))
args=ap.parse_args()
if args.kernel_source is None:raise SystemExit('Needs --kernel-source or PW3_KERNEL_SOURCE_DIR')
KERNEL=Path(args.kernel_source)
source=KERNEL/'drivers/usb/gadget/f_fs.c'
assert hashlib.sha256(source.read_bytes()).hexdigest()=='5a4aa9e4d611b01d2183ae9657e34370fa2b6aa3a44b47a19d796fb868a04f5c'
text=source.read_text()
def fn(name):
    start=text.index(name+'(')
    while text.find(';',start,text.find('{',start))!=-1:start=text.index(name+'(',start+len(name))
    start=text.rfind('\n',0,start)+1;end=text.index('{',start)+1;depth=1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]+'\n'
body='\n'.join(fn(n) for n in ('ffs_data_opened','ffs_data_clear','ffs_data_reset','ffs_data_put','ffs_data_closed','ffs_ep0_open','ffs_ep0_release'))
prefix=r'''
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#define ENTER() ((void)0)
#define unlikely(x) (x)
#define BUG_ON(x) assert(!(x))
#define pr_info(...) ((void)0)
#define FFS_FL_CALL_CLOSED_CALLBACK 0
#define FFS_READ_DESCRIPTORS 0
#define FFS_CLOSING 4
#define FFS_NO_SETUP 0
#define atomic_inc(x) (++*(x))
#define atomic_dec_and_test(x) (--*(x)==0)
#define mutex_is_locked(x) 0
#define spin_is_locked(x) 0
#define waitqueue_active(x) 0
#define kfree(x) free(x)
struct ffs_data { int ref,opened,state,setup_state;unsigned long flags;void *gadget,*epfiles,*raw_descs,*raw_strings,*stringtabs;unsigned eps_count,raw_descs_length,raw_fs_descs_length,fs_descs_count,hs_descs_count,strings_count,interfaces_count;struct {unsigned count;int waitq;} ev;int mutex,eps_lock,ep0req_completion; };
struct inode {void* i_private;};struct file {void* private_data;};
static int closed_callbacks=0,epfiles_destroyed=0;
static int test_and_clear_bit(unsigned bit,unsigned long* flags){int old=(*flags>>bit)&1;*flags&=~(1ul<<bit);return old;}
static void functionfs_closed_callback(struct ffs_data* f){++closed_callbacks;f->gadget=NULL;}
static void ffs_epfiles_destroy(void* files,unsigned count){(void)count;++epfiles_destroyed;free(files);}
'''
suffix=r'''
int main(void){
 struct ffs_data data={0};data.ref=1;data.state=FFS_READ_DESCRIPTORS;
 struct inode inode={&data};struct file file={0};
 for(unsigned i=0;i<100;i++){
  assert(ffs_ep0_open(&inode,&file)==0);assert(data.ref==2&&data.opened==1);
  assert(ffs_ep0_release(&inode,&file)==0);
  assert(data.ref==1&&data.opened==0&&data.state==FFS_READ_DESCRIPTORS&&data.flags==0);
  assert(closed_callbacks==0&&epfiles_destroyed==0&&data.gadget==NULL);
 }
 assert(ffs_ep0_open(&inode,&file)==0);assert(ffs_ep0_open(&inode,&file)==0);
 assert(ffs_ep0_release(&inode,&file)==0);assert(data.opened==1&&data.ref==2);
 assert(ffs_ep0_release(&inode,&file)==0);assert(data.opened==0&&data.ref==1);
 // Verify the callback is conditional: an active session would close its gadget.
 assert(ffs_ep0_open(&inode,&file)==0);data.flags=1;data.gadget=&data;
 assert(ffs_ep0_release(&inode,&file)==0);assert(closed_callbacks==1&&data.gadget==NULL&&data.state==FFS_READ_DESCRIPTORS);
 puts("Exact old FunctionFS empty-open/close lifecycle passed");
}
'''
with tempfile.TemporaryDirectory(prefix='pw3-ffs-lifecycle-') as d:
    p=Path(d);(p/'test.c').write_text(prefix+body+suffix)
    subprocess.run([args.cc,'-std=c11','-Wall','-Wextra','-Wno-unused-parameter','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
