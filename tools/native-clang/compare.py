#!/usr/bin/env python3
"""Compare real AOSP compiler commands without writing into the build output."""
from pathlib import Path
import argparse,hashlib,json,os,re,shlex,struct,subprocess,time
ap=argparse.ArgumentParser();ap.add_argument('--commands',type=Path,required=True);ap.add_argument('--output',type=Path,required=True);ap.add_argument('--native-prefix',type=Path,required=True);opts=ap.parse_args()
root=opts.output;root.mkdir(mode=0o700,exist_ok=False)
commands=json.loads(opts.commands.read_text())
results=[]
compiler=opts.native_prefix/'bin/clang.native'
compiler_sha=hashlib.sha256(compiler.read_bytes()).hexdigest()
def dependencies(path):
 data=path.read_text().replace('\\\n',' ')
 names=shlex.split(data.split(':',1)[1])
 files={str((Path('/src')/n).resolve()):hashlib.sha256((Path('/src')/n).read_bytes()).hexdigest() for n in names}
 return files

for index,(target,line) in enumerate(commands.items()):
 outer=shlex.split(line)
 body=outer[2] if outer[:2]==['/bin/bash','-c'] and len(outer)==3 else line
 def expand_includes(match):
  relative=Path(match.group(1));assert relative.parts[0]=='out' and '..' not in relative.parts
  return (Path('/src')/relative).read_text()
 body=re.sub(r'\\?\$\(cat ([A-Za-z0-9_./+-]+)\)',expand_includes,body)
 assert '$(' not in body
 args=shlex.split(body);assert args.pop(0)=='PWD=/proc/self/cwd'
 assert args[0].startswith('prebuilts/clang/host/linux-x86/clang-4053586/bin/clang')
 assert not any(p in ('&&','||',';','|') for p in args)
 case=root/str(index);case.mkdir()
 variants=[]
 for variant in ('original','native'):
  argv=list(args);output=case/(variant+'.o');dep=case/(variant+'.d')
  argv[argv.index('-o')+1]=str(output);argv[argv.index('-MF')+1]=str(dep)
  if variant=='native':argv[0]=str(opts.native_prefix/'bin'/Path(argv[0]).name)
  # Driver traces make sysroots, ABI switches and link tools reviewable.
  trace=subprocess.run(argv+['-###'],cwd='/src',text=True,capture_output=True)
  (case/(variant+'-driver.txt')).write_text(trace.stdout+trace.stderr)
  started=time.monotonic()
  run=subprocess.run(argv,cwd='/src',env={**os.environ,'PWD':'/proc/self/cwd','LC_ALL':'C'},text=True,capture_output=True)
  elapsed=time.monotonic()-started
  (case/(variant+'-compile.log')).write_text(run.stdout+run.stderr)
  if run.returncode:raise SystemExit('Candidate comparison compile failed: '+variant+' case '+str(index))
  data=output.read_bytes();assert data[:4]==b'\x7fELF'
  machine=struct.unpack_from('<H',data,18)[0]
  assert machine in (3,40,62)
  objcopy='/src/prebuilts/gcc/linux-x86/arm/arm-linux-androideabi-4.9/bin/arm-linux-androideabi-objcopy' if machine==40 else '/usr/bin/objcopy'
  normalized=case/(variant+'.code.o')
  subprocess.run([objcopy,'--strip-debug','--remove-section=.comment',str(output),str(normalized)],check=True)
  variants.append({'variant':variant,'seconds':elapsed,'sha256':hashlib.sha256(data).hexdigest(),'bytes':len(data),'argv':argv,
                   'machine':machine,'dependencies':dependencies(dep),'code_sha256':hashlib.sha256(normalized.read_bytes()).hexdigest()})
 results.append({'target':target,'variants':variants,'code_equal':variants[0]['code_sha256']==variants[1]['code_sha256'],
                 'dependencies_equal':variants[0]['dependencies']==variants[1]['dependencies']})
stable=compiler_sha==hashlib.sha256(compiler.read_bytes()).hexdigest()
passed=stable and len(results)==4 and all(c['code_equal'] and c['dependencies_equal'] for c in results)
record={'schema':1,'scope':'Four real ARM C/C++ and x86/x86_64 C++ object comparisons; not proof of all possible compiler inputs',
        'passed':passed,'compiler_stable':stable,'compiler_sha256':compiler_sha,
        'harness_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'native_prefix':str(opts.native_prefix),'cases':results}
(root/'result.json').write_text(json.dumps(record,indent=2)+'\n')
for case in results: print(case['target'], 'code_equal=',case['code_equal'],'dependencies_equal=',case['dependencies_equal'], 'seconds=',[round(v['seconds'],3) for v in case['variants']])
if not passed:raise SystemExit('Compiler comparison did not satisfy all gates')
print('PASS: all four actual-command object and dependency comparisons')
