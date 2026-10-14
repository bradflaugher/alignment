import sys, bisect
from pyboy import PyBoy
pb = PyBoy('build/alignment.gb', window='null', cgb=sys.argv[1]=='cgb', sound_emulated=False, symbols='build/alignment.sym')
syms={}
for l in open('build/alignment.sym'):
    p=l.split()
    if len(p)==2 and ':' in p[0]: syms.setdefault(int(p[0].split(':')[1],16),[]).append(p[1])
keys=sorted(syms)
def name(pc):
    i=bisect.bisect_right(keys,pc)-1
    return syms[keys[i]][0]+'+%x'%(pc-keys[i])
hits=[0]
def cb(ctx):
    hits[0]+=1
    if hits[0]>40: return
    r=pb.register_file; sp=r.SP
    print('HIT', ctx, 'SP', hex(sp), 'frame', pb.frame_count)
    for i in range(0,16,2):
        a=pb.memory[sp+i]|pb.memory[sp+i+1]<<8
        print('  ', hex(a), name(a))
for s in sys.argv[2:]:
    b,a = pb.symbol_lookup(s)
    pb.hook_register(b, a, cb, s)
for i in range(130): pb.tick()
