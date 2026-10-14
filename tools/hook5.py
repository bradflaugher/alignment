import sys
from pyboy import PyBoy
pb = PyBoy('build/alignment.gb', window='null', cgb=False, sound_emulated=False, symbols='build/alignment.sym')
b,a = pb.symbol_lookup('_gfx_load_title_tiles')
def cb(ctx):
    r=pb.register_file
    print('ENTRY frame',pb.frame_count,'SP',hex(r.SP),'ret',hex(pb.memory[r.SP]|pb.memory[r.SP+1]<<8),'bank',pb.memory[0xff90], 'stack',[hex(pb.memory[r.SP+i]) for i in range(12)])
pb.hook_register(b,a,cb,None)
for i in range(84): pb.tick()
