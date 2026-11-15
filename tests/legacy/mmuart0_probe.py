import mmap,os,struct,time
f=os.open('/dev/mem',os.O_RDWR|os.O_SYNC)
def m(b): return mmap.mmap(f,0x1000,offset=b)
s=m(0x20002000); u=m(0x20000000)
r=lambda mm,o: struct.unpack('<I',mm[o:o+4])[0]
print('SUBBLK_CLOCK_CR=%08x SOFT_RESET_CR=%08x'%(r(s,0x84),r(s,0x88)))
print('LCR=%02x LSR=%02x IER=%02x MCR=%02x'%(r(u,0x0c)&255,r(u,0x14)&255,r(u,0x04)&255,r(u,0x10)&255))
for c in b'PY_MMUART0_TEST\r\n'*3:
    while not r(u,0x14)&0x20: pass
    u[0:4]=struct.pack('<I',c)
print('sent')
