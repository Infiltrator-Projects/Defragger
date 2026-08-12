#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Create a small valid-enough fragmented exFAT image for raw-writer tests."""

from __future__ import annotations
import argparse
import math
import struct
from pathlib import Path

BPS=512
SPC=1
CS=BPS
FAT_OFF=24
FAT_LEN=8
HEAP_OFF=32
CC=512
ROOT=[30,60]
BITMAP=[10]
UPCASE=[20]
SUBDIR=[100,130]
FILE_A=[150,180,151]
FILE_B=[220,221,222,223]
FILE_C=[250,300]
EOC=0xFFFFFFFF
SERIAL=0x39E0FA71


def p16(b,o,v):struct.pack_into('<H',b,o,v)
def p32(b,o,v):struct.pack_into('<I',b,o,v)
def p64(b,o,v):struct.pack_into('<Q',b,o,v)

def boot_checksum(region):
    checksum=0
    for i,value in enumerate(region[:11*BPS]):
        if i in (106,107,112):continue
        checksum=(((checksum&1)<<31)+(checksum>>1)+value)&0xffffffff
    return checksum

def entry_checksum(data):
    checksum=0
    for i,value in enumerate(data):
        if i in (2,3):continue
        checksum=((checksum>>1)|((checksum&1)<<15))
        checksum=(checksum+value)&0xffff
    return checksum

def table_checksum(data):
    checksum=0
    for value in data:
        checksum=(((checksum&1)<<31)+(checksum>>1)+value)&0xffffffff
    return checksum

def file_set(name,first,clusters,is_dir=False,nofat=False,valid=None):
    encoded=name.encode('utf-16le')
    name_entries=math.ceil(len(encoded)/30)
    secondary=1+name_entries
    out=bytearray((1+secondary)*32)
    out[0]=0x85;out[1]=secondary
    p16(out,4,0x10 if is_dir else 0x20)
    stream=32
    out[stream]=0xC0
    out[stream+1]=0x02 if nofat else 0
    out[stream+3]=len(name)
    length=clusters*CS
    p64(out,stream+8,length if valid is None else valid)
    p32(out,stream+20,first)
    p64(out,stream+24,length)
    for index in range(name_entries):
        off=(2+index)*32
        out[off]=0xC1
        chunk=encoded[index*30:(index+1)*30]
        out[off+2:off+2+len(chunk)]=chunk
    p16(out,2,entry_checksum(out))
    return bytes(out)

def put_chain(fat,chain):
    for i,c in enumerate(chain):
        p32(fat,c*4,chain[i+1] if i+1<len(chain) else EOC)

def coff(c):return (HEAP_OFF+(c-2)*SPC)*BPS

def write_clusters(image,clusters,data):
    padded=data+bytes(len(clusters)*CS-len(data))
    for i,c in enumerate(clusters):
        image[coff(c):coff(c)+CS]=padded[i*CS:(i+1)*CS]

def make(path):
    total_sectors=HEAP_OFF+CC*SPC
    image=bytearray(total_sectors*BPS)
    main=bytearray(12*BPS)
    main[0:3]=b'\xeb\x76\x90';main[3:11]=b'EXFAT   '
    p64(main,64,0);p64(main,72,total_sectors)
    p32(main,80,FAT_OFF);p32(main,84,FAT_LEN);p32(main,88,HEAP_OFF);p32(main,92,CC)
    p32(main,96,ROOT[0]);p32(main,100,SERIAL);p16(main,104,0x0100);p16(main,106,0)
    main[108]=9;main[109]=0;main[110]=1;main[111]=0x80
    allocated=set(BITMAP+UPCASE+ROOT+SUBDIR+FILE_A+FILE_B+FILE_C)
    main[112]=(len(allocated)*100)//CC
    for sector in range(11):
        main[(sector+1)*BPS-2:(sector+1)*BPS]=b'\x55\xaa'
    checksum=boot_checksum(main)
    main[11*BPS:12*BPS]=struct.pack('<I',checksum)*(BPS//4)
    image[:12*BPS]=main
    image[12*BPS:24*BPS]=main

    fat=bytearray(FAT_LEN*BPS)
    p32(fat,0,0xfffffff8);p32(fat,4,0xffffffff)
    for chain in (BITMAP,UPCASE,ROOT,SUBDIR,FILE_A,FILE_C):put_chain(fat,chain)
    image[FAT_OFF*BPS:(FAT_OFF+FAT_LEN)*BPS]=fat

    bitmap=bytearray(math.ceil(CC/8))
    for c in allocated:
        bit=c-2;bitmap[bit>>3]|=1<<(bit&7)
    write_clusters(image,BITMAP,bytes(bitmap))

    upcase=bytearray()
    for code in range(128):
        mapped=code-32 if 97<=code<=122 else code
        upcase+=struct.pack('<H',mapped)
    write_clusters(image,UPCASE,bytes(upcase))

    root=bytearray(len(ROOT)*CS)
    off=0
    root[off]=0x81;p32(root,off+20,BITMAP[0]);p64(root,off+24,len(bitmap));off+=32
    root[off]=0x82;p32(root,off+4,table_checksum(bytes(upcase)));p32(root,off+20,UPCASE[0]);p64(root,off+24,len(upcase));off+=32
    for item in (
        file_set('Sub',SUBDIR[0],len(SUBDIR),True,False),
        file_set('A.bin',FILE_A[0],len(FILE_A),False,False),
        file_set('B.bin',FILE_B[0],len(FILE_B),False,True),
    ):
        root[off:off+len(item)]=item;off+=len(item)
    root[off]=0
    write_clusters(image,ROOT,bytes(root))

    sub=bytearray(len(SUBDIR)*CS)
    child=file_set('C.bin',FILE_C[0],len(FILE_C),False,False)
    sub[:len(child)]=child;sub[len(child)]=0
    write_clusters(image,SUBDIR,bytes(sub))

    payloads={
        'A.bin':bytes((i*7+3)&255 for i in range(len(FILE_A)*CS)),
        'B.bin':bytes((i*11+5)&255 for i in range(len(FILE_B)*CS)),
        'C.bin':bytes((i*13+9)&255 for i in range(len(FILE_C)*CS)),
    }
    write_clusters(image,FILE_A,payloads['A.bin'])
    write_clusters(image,FILE_B,payloads['B.bin'])
    write_clusters(image,FILE_C,payloads['C.bin'])
    path.write_bytes(image)

if __name__=='__main__':
    ap=argparse.ArgumentParser();ap.add_argument('path',type=Path);args=ap.parse_args();make(args.path)
