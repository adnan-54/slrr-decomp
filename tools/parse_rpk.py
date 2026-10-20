import struct,sys
d=open(sys.argv[1],'rb').read()
magic,ver,ca,cb,blob,n,x1,x2=struct.unpack_from('<4sIIIIIII',d,0)
print(magic,hex(ver),ca,cb,hex(blob),n,x1,hex(x2))
p=0x20  # observed: header 0x20 bytes incl. count_a/b records? ca+cb==0 here
p=0x20+ (ca+cb)*0x40
end=p+blob
print('blob',hex(p),hex(end),'filesize',hex(len(d)))
i=0
while p<end and i<n:
    unk,rid,typ,fl=struct.unpack_from('<IIBB',d,p)
    off,sz=struct.unpack_from('<II',d,p+0xe)
    nl=d[p+0x16]; name=d[p+0x17:p+0x17+nl].rstrip(b'\0').decode()
    q=p+0x17+nl; extra=b''
    if fl&1: extra=d[q:q+48]; q+=48
    print(f'{i:2d} @{p:#x} unk={unk:#x} id={rid:#x} type={typ} fl={fl:#x} off={off:#x} size={sz:#x} name={name!r} extra={extra.hex()}')
    p=q;i+=1
print('stopped',hex(p))
