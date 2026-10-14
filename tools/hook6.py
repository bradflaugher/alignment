import sys
from pyboy import PyBoy
pb = PyBoy('build/alignment.gb', window='null', cgb=False, sound_emulated=False, symbols='build/alignment.sym')
bank=int(sys.argv[1]); lo=int(sys.argv[2],16); hi=int(sys.argv[3],16); frames=int(sys.argv[4])
log=[]
def cb(ctx):
    r=pb.register_file
    if len(log)<400: log.append((hex(ctx), hex(r.SP), pb.frame_count, pb.memory[0xff90], hex(r.A)))
for a in range(lo,hi): pb.hook_register(bank,a,cb,a)
for i in range(frames): pb.tick()
for l in log[-60:]: print(*l)
