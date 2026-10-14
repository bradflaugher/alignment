import sys, bisect
from pyboy import PyBoy
pb = PyBoy('build/alignment.gb', window='null', cgb=sys.argv[1]=='cgb', sound_emulated=False)
syms={}
for l in open('build/alignment.sym'):
    p=l.split()
    if len(p)==2 and ':' in p[0] and p[1].startswith('_'): syms.setdefault(int(p[0].split(':')[1],16),[]).append(p[1])
keys=sorted(syms)
def name(pc):
    i=bisect.bisect_right(keys,pc)-1
    return syms[keys[i]][0]+'+%x'%(pc-keys[i]) if i>=0 else hex(pc)
last=None
start=int(sys.argv[2]); end=int(sys.argv[3])
while pb.frame_count < start: pb.tick()
seen=[]
# single-step approximations: tick 1 frame, record PC
for f in range(start,end):
    pb.tick()
    r=pb.register_file
    seen.append((pb.frame_count, name(r.PC), hex(r.SP), hex(pb.memory[0xFF40])))
for s in seen: print(*s)
