#!/usr/bin/env python3
import mmap, os, sys, termios, tty

PATH = "/dev/shm/fd_manual_leader"

def write_val(v: int):
    fd = os.open(PATH, os.O_CREAT | os.O_RDWR, 0o666)
    os.ftruncate(fd, 4)
    m = mmap.mmap(fd, 4, mmap.MAP_SHARED, mmap.PROT_WRITE | mmap.PROT_READ)
    m[0:4] = int(v).to_bytes(4, "little")
    m.flush(); m.close(); os.close(fd)

def getch():
    fd = sys.stdin.fileno(); old = termios.tcgetattr(fd)
    try:
        tty.setraw(fd); ch = sys.stdin.read(1)
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old)
    return ch

print("Press l = leader(1), f = follower(0), q = quit")
while True:
    c = getch().lower()
    if c == "l":
        write_val(1); print("\nSet leader=1")
    elif c == "f":
        write_val(0); print("\nSet leader=0")
    elif c == "q":
        print("\nBye"); break