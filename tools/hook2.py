import sys
from pyboy import PyBoy
pb = PyBoy('build/alignment.gb', window='null', cgb=False, sound_emulated=False, symbols='build/alignment.sym')
n=[0]
def cb(ctx):
    n[0]+=1
    if n[0]<12: print('HIT',ctx,'frame',pb.frame_count,'SP',hex(pb.register_file.SP))
for s in sys.argv[1:]:
    b,a=pb.symbol_lookup(s); pb.hook_register(b,a,cb,s)
for i in range(100): pb.tick()
