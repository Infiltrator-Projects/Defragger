#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build tiny non-journaled HFS+/HFSX images directly from TN1150 structures."""
from __future__ import annotations
import argparse, struct
from pathlib import Path

BS=4096
BLOCKS=128
SIZE=BS*BLOCKS

def be16(x): return struct.pack('>H',x)
def be32(x): return struct.pack('>I',x)
def be64(x): return struct.pack('>Q',x)

def fork(logical,total,extents):
    out=bytearray(80)
    out[0:8]=be64(logical)
    out[8:12]=be32(0)
    out[12:16]=be32(total)
    for i,(s,c) in enumerate(extents[:8]):
        out[16+i*8:20+i*8]=be32(s)
        out[20+i*8:24+i*8]=be32(c)
    return bytes(out)

def header_node(node_size,first_leaf,total_nodes):
    n=bytearray(node_size)
    # BTNodeDescriptor at 0; header node kind=1.
    n[8]=1
    n[10:12]=be16(3)
    # BTHeaderRec begins at 14. These absolute offsets match TN1150.
    n[14:16]=be16(1 if first_leaf else 0)
    n[16:20]=be32(first_leaf)
    n[18:22]=be32(first_leaf)  # harmless overlap for unused root in this fixture
    n[24:28]=be32(first_leaf)
    n[28:32]=be32(first_leaf)
    n[32:34]=be16(node_size)
    n[34:36]=be16(516)
    n[36:40]=be32(total_nodes)
    return n

def catalog_key(parent=2):
    # keyLength=6: parentID + zero-length Unicode name.
    return be16(6)+be32(parent)+be16(0)

def folder_record():
    data=bytearray(88)
    data[0:2]=be16(1)
    data[8:12]=be32(2)
    return catalog_key(1)+data

def file_record(file_id,logical,total,extents):
    data=bytearray(248)
    data[0:2]=be16(2)
    data[8:12]=be32(file_id)
    data[88:168]=fork(logical,total,extents)
    return catalog_key(2)+data

def leaf_node(records,node_size=BS):
    n=bytearray(node_size)
    n[8]=0xff # kBTLeafNode = -1
    n[9]=1
    n[10:12]=be16(len(records))
    pos=14
    starts=[]
    for r in records:
        starts.append(pos)
        n[pos:pos+len(r)]=r
        pos+=len(r)
    # Offset table is reverse-ordered from end of node.
    for i,start in enumerate(starts):
        n[-2*(i+1): -2*i if i else None]=be16(start)
    n[-2*(len(records)+1):-2*len(records)]=be16(pos)
    return n

def extents_record(file_id,start_block,extents):
    key=be16(10)+bytes([0,0])+be32(file_id)+be32(start_block)
    data=bytearray(64)
    for i,(start,count) in enumerate(extents[:8]):
        data[i*8:i*8+4]=be32(start)
        data[i*8+4:i*8+8]=be32(count)
    return key+data

def set_used(bitmap,block,used=True):
    mask=1 << (7-(block%8))
    if used: bitmap[block//8]|=mask
    else: bitmap[block//8]&=~mask

def journal_checksum(data:bytes):
    c=0
    for byte in data:
        c=((c << 8) & 0xffffffff) ^ ((c + byte) & 0xffffffff)
    return (~c) & 0xffffffff

def build(path:Path,hfsx=False,journaled=False,dirty_journal=False,overflow=False,inconsistent=False,reserved14=False):
    image=bytearray(SIZE)
    if overflow:
        file16_extents=[(10,1),(12,1),(14,1),(16,1),(18,1),(26,1),(28,1),(30,1),(32,1)]
        file17_extents=[(35,1),(37,2)]
        used={0,2,3,4,5,BLOCKS-1}
        used.update(start+b for start,count in file16_extents+file17_extents for b in range(count))
    else:
        file16_extents=[(10,1),(12,1)]
        file17_extents=[(15,1),(17,2)]
        used={0,2,3,4,5,BLOCKS-1,10,12,15,17,18}
    if journaled:
        used.update({20,21,22,23,24})
    bitmap=bytearray((BLOCKS+7)//8)
    for b in used: set_used(bitmap,b)
    image[2*BS:2*BS+len(bitmap)]=bitmap
    ext=header_node(512,1 if overflow else 0,2 if overflow else 1)
    image[3*BS:3*BS+512]=ext
    if overflow:
        image[3*BS+512:3*BS+1024]=leaf_node([extents_record(16,8,[file16_extents[8]])],512)
    cat0=header_node(BS,1,2)
    image[4*BS:5*BS]=cat0
    file16_logical=9*BS-777 if overflow else 7000
    records=[folder_record(),
             file_record(16,file16_logical,9 if overflow else 2,file16_extents[:8]),
             file_record(17,10000,3,file17_extents)]
    if journaled:
        # The C engine must discover these from the volume header/JIB ranges,
        # not from their names.  Their file IDs are ordinary catalog IDs.
        records += [file_record(18,180,1,[(20,1)]),
                    file_record(19,4*BS,4,[(21,4)])]
    image[5*BS:6*BS]=leaf_node(records)
    for index,(start,count) in enumerate(file16_extents):
        for b in range(count): image[(start+b)*BS:(start+b+1)*BS]=bytes([0x41+index])*BS
    file17_patterns=[0x51,0x52,0x53]
    index=0
    for start,count in file17_extents:
        for b in range(count):
            image[(start+b)*BS:(start+b+1)*BS]=bytes([file17_patterns[index]])*BS
            index+=1
    vh=bytearray(512)
    vh[0:2]=b'HX' if hfsx else b'H+'
    vh[2:4]=be16(5 if hfsx else 4)
    attrs=0x100 | (0x2000 if journaled else 0)
    if inconsistent: attrs |= 0x00000800
    if reserved14: attrs |= 0x00004000
    vh[4:8]=be32(attrs)
    if journaled:
        vh[12:16]=be32(20)  # journalInfoBlock allocation block
    vh[32:36]=be32(4 if journaled else 2)
    vh[36:40]=be32(0)
    vh[40:44]=be32(BS)
    vh[44:48]=be32(BLOCKS)
    vh[48:52]=be32(BLOCKS-len(used))
    vh[112:192]=fork(len(bitmap),1,[(2,1)])
    vh[192:272]=fork(1024 if overflow else 512,1,[(3,1)])
    vh[272:352]=fork(BS*2,2,[(4,2)])
    if journaled:
        jib=bytearray(180)
        jib[0:4]=be32(1)  # kJIJournalInFSMask
        jib[36:44]=be64(21*BS)
        jib[44:52]=be64(4*BS)
        image[20*BS:20*BS+len(jib)]=jib
        # Journal headers may be native or swapped byte order.  Generate a
        # little-endian header to exercise the format's independent endian tag.
        jh=bytearray(44)
        struct.pack_into('<IIQQQIII',jh,0,0x4a4e4c78,0x12345678,
                         512,1024 if dirty_journal else 512,4*BS,BS,0,512)
        check=bytearray(jh); check[36:40]=b'\0'*4
        struct.pack_into('<I',jh,36,journal_checksum(check))
        image[21*BS:21*BS+len(jh)]=jh
    image[1024:1536]=vh
    image[-1024:-512]=vh
    path.write_bytes(image)

def parse_file_extents(path:Path):
    d=path.read_bytes(); node=d[5*BS:6*BS]; count=struct.unpack_from('>H',node,10)[0]
    starts=sorted(struct.unpack_from('>H',node,BS-2*(i+1))[0] for i in range(count))
    result={}
    for start in starts:
        keylen=struct.unpack_from('>H',node,start)[0]
        off=start+2+keylen+(keylen&1)
        typ=struct.unpack_from('>H',node,off)[0]
        if typ!=2: continue
        fid=struct.unpack_from('>I',node,off+8)[0]
        total=struct.unpack_from('>I',node,off+88+12)[0]
        ex=[]
        for i in range(8):
            s,c=struct.unpack_from('>II',node,off+88+16+i*8)
            if c: ex.append((s,c))
        result[fid]=(total,ex)
    # This fixture stores any overflow leaf as node 1 at logical offset 512
    # inside the extents-overflow file in allocation block 3.
    extnode=d[3*BS+512:3*BS+1024]
    if len(extnode)==512 and extnode[8]==0xff:
        count=struct.unpack_from('>H',extnode,10)[0]
        starts=sorted(struct.unpack_from('>H',extnode,512-2*(i+1))[0] for i in range(count))
        for start in starts:
            keylen=struct.unpack_from('>H',extnode,start)[0]
            if keylen<10: continue
            fid=struct.unpack_from('>I',extnode,start+4)[0]
            logical_start=struct.unpack_from('>I',extnode,start+8)[0]
            off=start+2+keylen+(keylen&1)
            if fid not in result: continue
            total,ex=result[fid]
            if sum(c for _,c in ex)!=logical_start: continue
            for i in range(8):
                ss,cc=struct.unpack_from('>II',extnode,off+i*8)
                if cc: ex.append((ss,cc))
            result[fid]=(total,ex)
    return result

def payload(path:Path,file_id:int):
    d=path.read_bytes(); total,ex=parse_file_extents(path)[file_id]
    out=b''.join(d[s*BS:(s+c)*BS] for s,c in ex)
    logical=(9*BS-777 if total==9 else 7000) if file_id==16 else 10000
    return out[:logical]

def verify(path:Path,growth=False):
    d=path.read_bytes(); bitmap=d[2*BS:2*BS+((BLOCKS+7)//8)]
    def used(b): return bool(bitmap[b//8] & (1 << (7-(b%8))))
    exts=parse_file_extents(path)
    for fid,(total,ex) in exts.items():
        assert sum(c for _,c in ex)==total,(fid,total,ex)
        assert all(ex[i-1][0]+ex[i-1][1]==ex[i][0] for i in range(1,len(ex))),(fid,ex)
        for start,count in ex:
            for b in range(start,start+count): assert used(b)
        if growth and fid in {16,17}:
            reserve=(total+9)//10
            end=ex[0][0]+total
            for b in range(end,end+reserve): assert not used(b),(fid,b)
    if 18 in exts or 19 in exts:
        assert exts[18] == (1,[(20,1)])
        assert exts[19] == (4,[(21,4)])
    total16,_=parse_file_extents(path)[16]
    if total16==9:
        expected=b''.join(bytes([0x41+i])*BS for i in range(9))[:9*BS-777]
    else:
        expected=(bytes([0x41])*BS+bytes([0x42])*BS)[:7000]
    assert payload(path,16)==expected
    assert payload(path,17)==(bytes([0x51])*BS+bytes([0x52])*BS+bytes([0x53])*BS)[:10000]

if __name__=='__main__':
    ap=argparse.ArgumentParser(); ap.add_argument('path',type=Path); ap.add_argument('--hfsx',action='store_true'); ap.add_argument('--journaled',action='store_true'); ap.add_argument('--dirty-journal',action='store_true'); ap.add_argument('--overflow',action='store_true'); ap.add_argument('--inconsistent',action='store_true'); ap.add_argument('--reserved14',action='store_true'); ap.add_argument('--verify',action='store_true'); ap.add_argument('--growth',action='store_true'); a=ap.parse_args()
    if a.verify: verify(a.path,a.growth)
    else: build(a.path,a.hfsx,a.journaled,a.dirty_journal,a.overflow,a.inconsistent,a.reserved14)
