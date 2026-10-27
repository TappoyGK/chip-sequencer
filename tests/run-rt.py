#!/usr/bin/env python3
"""Compile every production layer with allocator guards, then execute contracts."""
import pathlib, subprocess, tempfile, sys
root=pathlib.Path(__file__).resolve().parents[1]
audio=root.parent/'chip-audio'
with tempfile.TemporaryDirectory(prefix='chip-rt-') as tmp:
    inc=['-I'+str(root/'include'),'-I'+str(audio/'include')]
    common=['clang','-std=c11','-O1','-g','-fsanitize=address,undefined',*inc]
    objs=[]
    for i,src in enumerate([root/'src/chip-sequencer.c',root/'src/compiler.c',audio/'src/chip-audio.c',audio/'src/chip-drums.c',audio/'src/chip-mixer.c']):
        obj=f'{tmp}/{i}.o';subprocess.run([*common,'-include',str(root/'tests/rt/hooks.h'),'-c',str(src),'-o',obj],check=True);objs.append(obj)
    for test in [root/'tests/sequence-test.c',audio/'tests/mixer-test.c',root/'tests/benchmark.c']:
        exe=f'{tmp}/{test.stem}';subprocess.run([*common,'-include',str(root/'tests/rt/guard.h'),str(test),str(root/'tests/rt/hooks.c'),*objs,'-lm','-o',exe],check=True)
        args=[str(root/'examples/sequence.json')] if test.stem=='sequence-test' else ([sys.argv[1] if len(sys.argv)>1 else str(root/'examples/sequence.json')] if test.stem=='benchmark' else [])
        subprocess.run([exe,*args],check=True)
