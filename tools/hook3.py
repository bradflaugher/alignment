import sys
from pyboy import PyBoy
pb = PyBoy('build/alignment.gb', window='null', cgb=False, sound_emulated=False, symbols='build/alignment.sym')
n=[0]
def cb(ctx):
    n[0]+=1
    r=pb.register_file
    if n[0]<40: print('HIT',ctx,'frame',pb.frame_count,'SP',hex(r.SP),'A',hex(r.A),'BC',hex(r.B<<8|r.C),'DE',hex(r.D<<8|r.E),'HL',hex(r.HL), 'bank', pb.memory[0xFF90], 'stack', [hex(pb.memory[r.SP+i]) for i in range(8)])
for s in sys.argv[1:]:
    b,a=pb.symbol_lookup(s); pb.hook_register(b,a,cb,s)
for i in range(82): pb.tick()
