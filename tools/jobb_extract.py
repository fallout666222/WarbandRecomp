#!/usr/bin/env python3
"""Extract Android .obb (jobb FAT12/16/32 image) -> directory. Usage: jobb.py <obb> [outdir]"""
import struct,sys,os
img=sys.argv[1]; out=sys.argv[2] if len(sys.argv)>2 else None
f=open(img,'rb'); bs=f.read(512)
bps,spc=struct.unpack_from('<HB',bs,11)
rsv=struct.unpack_from('<H',bs,14)[0]; nfat=bs[16]
rootent=struct.unpack_from('<H',bs,17)[0]
tot16=struct.unpack_from('<H',bs,19)[0]
fatsz16=struct.unpack_from('<H',bs,22)[0]
tot32=struct.unpack_from('<I',bs,32)[0]
fatsz32=struct.unpack_from('<I',bs,36)[0]
fatsz=fatsz16 or fatsz32
tot=tot16 or tot32
rootdir_sec=(rootent*32+bps-1)//bps
data_sec=rsv+nfat*fatsz+rootdir_sec
clusters=(tot-data_sec)//spc
bits=32 if fatsz16==0 else (12 if clusters<4085 else 16)
print(f"FAT{bits} bps={bps} spc={spc} rsv={rsv} nfat={nfat} fatsz={fatsz} rootent={rootent} clusters={clusters}")
f.seek(rsv*bps); fat=f.read(fatsz*bps)
data_off=data_sec*bps
EOC={12:0xFF8,16:0xFFF8,32:0x0FFFFFF8}[bits]
def nxt(c):
    if bits==32: return struct.unpack_from('<I',fat,c*4)[0]&0x0FFFFFFF
    if bits==16: return struct.unpack_from('<H',fat,c*2)[0]
    o=c+(c>>1); v=struct.unpack_from('<H',fat,o)[0]
    return (v>>4) if c&1 else (v&0xFFF)
def chain(c):
    r=[]
    while 2<=c<EOC:
        r.append(c); c=nxt(c)
        if len(r)>4_000_000: break
    return r
def rd(c):
    f.seek(data_off+(c-2)*spc*bps); return f.read(spc*bps)
n=[0,0]
def entries(data,path):
    lfn=[]
    for i in range(0,len(data),32):
        e=data[i:i+32]
        if len(e)<32 or e[0]==0: return
        if e[0]==0xE5: lfn=[]; continue
        if e[11]==0x0F:
            lfn.append((e[0]&0x1f,(e[1:11]+e[14:26]+e[28:32]).decode('utf-16-le','replace'))); continue
        name=''
        if lfn:
            lfn.sort(); name=''.join(x for _,x in lfn).split('\0')[0]
        lfn=[]
        if not name:
            b=e[0:8].decode('ascii','replace').rstrip(); x=e[8:11].decode('ascii','replace').rstrip()
            name=b+('.'+x if x else '')
        if name in ('.','..'): continue
        clus=(struct.unpack_from('<H',e,20)[0]<<16)|struct.unpack_from('<H',e,26)[0]
        size=struct.unpack_from('<I',e,28)[0]
        yield name,e[11],clus,size,path
def walk(clus,path,rootdata=None):
    data=rootdata if rootdata is not None else b''.join(rd(c) for c in chain(clus))
    for name,attr,c,size,_ in list(entries(data,path)):
        p=path+'/'+name
        if attr&0x10:
            n[1]+=1
            if out is None: print(f"[DIR ] {p}")
            walk(c,p)
        else:
            n[0]+=1
            if out is None: print(f"{size:>12} {p}")
            else:
                dst=os.path.join(out,p.lstrip('/')); os.makedirs(os.path.dirname(dst),exist_ok=True)
                d=b''.join(rd(x) for x in chain(c))[:size] if size else b''
                open(dst,'wb').write(d)
if bits==32:
    walk(struct.unpack_from('<I',bs,44)[0],'')
else:
    f.seek((rsv+nfat*fatsz)*bps); walk(0,'',f.read(rootent*32))
print("files",n[0],"dirs",n[1])
