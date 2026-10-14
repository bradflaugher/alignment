import sys
from pyboy import PyBoy
cgb = sys.argv[1] == 'cgb'
pb = PyBoy('build/alignment.gb', window='null', cgb=cgb, sound_emulated=False)
pb.set_emulation_speed(0)
def run(n):
    for _ in range(n): pb.tick()
def press(b, n=4):
    pb.button_press(b); run(n); pb.button_release(b); run(6)
def shot(name): pb.screen.image.save('build/shots/%s_%s.png' % (name, sys.argv[1]))
run(200); shot('title')
press('start'); run(20); shot('menu')
press('a'); run(400); shot('room1')
for k in ['right']*4 + ['a'] + ['right']*3:
    press(k); run(40)
shot('room1b')
for i in range(30):
    press('right'); run(30)
run(200); shot('room2')
