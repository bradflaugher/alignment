import sys
from pyboy import PyBoy
pb = PyBoy('build/alignment.gb', window='null', cgb=sys.argv[1]=='cgb', sound_emulated=False, symbols='build/alignment.sym')
syms={}
for l in open('build/alignment.sym'):
    p=l.split()
    if len(p)==2 and ':' in p[0]: syms.setdefault(int(p[0].split(':')[1],16),[]).append(p[1])
import bisect
keys=sorted(syms)
def name(pc):
    i=bisect.bisect_right(keys,pc)-1
    return syms[keys[i]][0]+'+%x'%(pc-keys[i])
for f in [1,5,20,60,120]:
    while pb.frame_count < f: pb.tick()
    r=pb.register_file
    print(f, 'PC', hex(r.PC), name(r.PC), 'SP', hex(r.SP), 'LCDC', hex(pb.memory[0xFF40]), 'bank', pb.memory[0xFF90])
