#!/usr/bin/env python3
"""Link the movie runner to a locally built Ymir core; no firmware is bundled."""
from pathlib import Path
import subprocess,shlex,argparse
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--ymir',type=Path,default=Path(__file__).resolve().parents[4]/'Saturn/Ymir')
parser.add_argument('--out',type=Path,default=Path('artifacts/saturn-sv24/ymir_movie'))
options=parser.parse_args();root=options.ymir.resolve();b=root/'build'
# Use the sandbox's link dependencies and build definitions verbatim.
targets=subprocess.check_output(['ninja','-C',str(b),'-t','targets','all'],text=True).splitlines()
target=next(line.split(': ')[0] for line in targets if line.startswith('apps/ymir-sandbox/ymir-sandbox-') and '.dir/' not in line)
commands=subprocess.check_output(['ninja','-C',str(b),'-t','commands',target],text=True).splitlines()
link=commands[-1];args=shlex.split(link[link.index('/usr/bin/clang++'):].split(' &&')[0]);libs=args[args.index('libs/ymir-core/libymir-core.a'):]
source=Path('tools/saturn/tests/ymir_movie.cpp').resolve();out=options.out.resolve();out.parent.mkdir(parents=True,exist_ok=True)
includes=[root/'libs/ymir-core/include',b/'libs/ymir-core/include',root/'vendor/fmt/include',root/'vendor/mio/include',root/'vendor/xxHash/xxHash',root/'vendor/concurrentqueue/concurrentqueue',root/'vendor/libchdr/libchdr/include',b/'vcpkg_installed/x64-linux/include']
compile_line=next(c for c in commands if 'sandbox_input.cpp.o -c' in c)
flags=[a for a in shlex.split(compile_line) if a.startswith('-D')]
cmd=['clang++','-fuse-ld=lld','-flto=thin','-O2','-std=c++20','-DNDEBUG',*flags]
for p in includes:cmd+=['-I',str(p)]
subprocess.run(cmd+[str(source),'-o',str(out),*libs,'-pthread'],cwd=b,check=True)
