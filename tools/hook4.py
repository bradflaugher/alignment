import sys
from pyboy import PyBoy
pb = PyBoy('build/alignment.gb', window='null', cgb=False, sound_emulated=False, symbols='build/alignment.sym')
def cb(ctx):
    r=pb.register_file
    print('HIT',hex(ctx),'frame',pb.frame_count,'SP',hex(r.SP),'stack',[hex(pb.memory[r.SP+i]) for i in range(10)])
for a in [int(x,16) for x in sys.argv[1:]]:
    pb.hook_register(0,a,cb,a)
for i in range(82): pb.tick()
