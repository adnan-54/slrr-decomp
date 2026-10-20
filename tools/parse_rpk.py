import struct,sys
d=open(sys.argv[1],'rb').read()
magic,ver,ca,cb=struct.unpack_from('<4sIII',d,0)
print(magic,hex(ver),'count_a',ca,'count_b',cb)
p=0x10
for i in range(ca+cb):
    z,rid=struct.unpack_from('<HH',d,p)
    name=d[p+4:p+0x3c].split(b'\0')[0].decode()
    tail=struct.unpack_from('<I',d,p+0x3c)[0]
    print(f'  pkg[{i+1}] @{p:#x} id={rid} name={name!r} tail={tail:#x} z={z}')
    p+=0x40
blob,n,x1,x2=struct.unpack_from('<IIII',d,p)
print('blob_size',hex(blob),'records',n,'x1',x1,'x2',hex(x2))
p+=16; end=p+blob
print('table',hex(p),hex(end),'filesize',hex(len(d)))
i=0
while p<end and i<n:
    unk,rid,typ,fl=struct.unpack_from('<IIBB',d,p)
    off,sz=struct.unpack_from('<II',d,p+0xe)
    nl=d[p+0x16]; name=d[p+0x17:p+0x17+nl].rstrip(b'\0').decode(errors='replace')
    q=p+0x17+nl; extra=b''
    if fl&1: extra=d[q:q+48]; q+=48
    print(f'{i:3d} @{p:#x} unk={unk:#x} id={rid:#x} type={typ} fl={fl:#x} b0a={d[p+10:p+14].hex()} off={off:#x} size={sz:#x} name={name!r}'+(f' extra={extra.hex()}' if extra else ''))
    p=q;i+=1
print('stopped',hex(p),'expected',hex(end),'records',i)
