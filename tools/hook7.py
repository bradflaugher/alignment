from pyboy import PyBoy
pb = PyBoy('build/alignment.gb', window='null', cgb=False, sound_emulated=False, symbols='build/alignment.sym')
log=[]
def cb(ctx):
    r=pb.register_file
    if len(log)<30: log.append((hex(ctx), hex(r.SP), pb.frame_count, [hex(pb.memory[r.SP+i]) for i in range(8)]))
for a in [0x00,0x08,0x10,0x18,0x20,0x28,0x30,0x38,0x40,0x48]:
    pb.hook_register(0,a,cb,a)
while pb.frame_count<81: pb.tick()
log.clear()
for i in range(3): pb.tick()
for l in log: print(*l)
