#!/usr/bin/env python3
"""Audit the compiled ARM boundary, including regressions hidden by flat-cache QEMU."""
import argparse,hashlib,json,re,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--module',type=Path,required=True);p.add_argument('--profile',choices=['stock','qemu'],required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
prefix='/home/builder/toolchains/aosp-arm-gcc49/bin/arm-linux-androideabi-'
syms={}
for line in subprocess.check_output([prefix+'nm','-S','--defined-only',str(a.module)],text=True).splitlines():
 v=line.split()
 if len(v)==4:syms[v[3]]=(int(v[0],16),int(v[1],16))
def body(name):
 start,size=syms[name];return subprocess.check_output([prefix+'objdump','-d','-j','.text','--start-address='+str(start),'--stop-address='+str(start+size),str(a.module)],text=True)
def instructions(text):return [m[1] for line in text.splitlines() if (m:=re.match(r'\s*[0-9a-f]+:\s+[0-9a-f]{8}\s+(.+)',line))]
checks={};artifacts={}
if 'native_finish_cpu' in syms:
 ins=instructions(body('native_finish_cpu'));points=[i for i,x in enumerate(ins) if re.search(r'mcr\s+15, 0, r\d+, cr1, cr0, \{0\}',x)]
 checks['no_stack_access_after_cache_disable']=bool(points) and not any(re.search(r'\bsp\b|\b(push|pop)\b',x) for x in ins[points[-1]+1:])
 artifacts['legacy_finish_disassembly']=body('native_finish_cpu')
 checks['c_cache_disable_function_removed']=False
else:checks['c_cache_disable_function_removed']=True
for name in ['pw3_cache_finish_and_enter','sim_flush_dcache_all','pw3_enter_private_mapping','pw3_cache_backend_check']:
 checks[name+'_present']=name in syms
 if name in syms:artifacts[name]=body(name)
if all(checks.get(n+'_present') for n in ['pw3_cache_finish_and_enter','sim_flush_dcache_all','pw3_enter_private_mapping','pw3_cache_backend_check']):
 finish=instructions(artifacts['pw3_cache_finish_and_enter']);raw=instructions(artifacts['sim_flush_dcache_all']);entry=instructions(artifacts['pw3_enter_private_mapping']);check=instructions(artifacts['pw3_cache_backend_check'])
 checks['finish_and_entry_are_stackless']=not any(re.search(r'\bsp\b|\b(push|pop)\b',x) for x in finish+entry)
 checks['final_clean_body_has_no_stack_or_stores']=not any(re.search(r'\bsp\b|^(push|pop|str|stm)',x) for x in raw)
 checks['raw_clean_preserves_entry_registers']=not any(re.search(r'\br[68]\b',x) for x in raw)
 points=[i for i,x in enumerate(finish) if re.search(r'mcr\s+15, 0, r2, cr1, cr0, \{0\}',x)]
 checks['exactly_one_cache_disable']=len(points)==1
 if len(points)==1:
  cut=points[0];checks['no_memory_or_call_after_disable']=not any(re.search(r'\bsp\b|^(ldr|ldm|str|stm|push|pop|bl\b|blx\b)',x) for x in finish[cut+1:])
  calls=[x for x in finish[:cut] if re.match(r'bl\s',x)];checks['last_clean_precedes_disable']=len(calls)==1 and '<sim_flush_dcache_all>' in calls[0]
  checks['tail_enters_private_mapping']=bool(re.search(r'^b\s.*<pw3_enter_private_mapping>',finish[-1]))
 checks['pure_backend_validator_has_no_calls']=not any(re.match(r'blx?\s',x) for x in check)
 if a.profile=='stock':checks['physical_binary_has_no_private_stack_switch']='pws_call_on_stack' not in syms
r={'schema':1,'passed':bool(checks) and all(checks.values()),'profile':a.profile,'module_sha256':hashlib.sha256(a.module.read_bytes()).hexdigest(),'checks':checks,'scope':'Actual compiled ARM instructions and symbols. Does not simulate physical data caches or prove the cause of the prior reset.'}
a.output.mkdir(exist_ok=False);(a.output/'result.json').write_text(json.dumps(r,indent=2)+'\n')
for n,text in artifacts.items():(a.output/(n+'.txt')).write_text(text)
print(json.dumps(r,indent=2));raise SystemExit(0 if r['passed'] else 1)
