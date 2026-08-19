#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
from __future__ import annotations
import argparse
import json
import struct
import subprocess
from pathlib import Path

BPS=512
HEAP_OFF=32
CS=512
EOC=0xFFFFFFFF
SERIAL=0x39E0FA71

def coff(cluster: int) -> int:
    return (HEAP_OFF + cluster - 2) * BPS

def u32(data: bytes, off: int) -> int:
    return struct.unpack_from('<I', data, off)[0]

def expected_payload(mult: int, add: int, clusters: int) -> bytes:
    return bytes((i * mult + add) & 255 for i in range(clusters * CS))

def main() -> None:
    ap=argparse.ArgumentParser()
    ap.add_argument('worker', type=Path)
    ap.add_argument('image', type=Path)
    ap.add_argument('mode', choices=('defrag','growth'))
    args=ap.parse_args()
    image=args.image.read_bytes()
    assert u32(image,100)==SERIAL
    assert u32(image,96)==4
    fat=image[24*BPS:32*BPS]
    assert u32(fat,2*4)==EOC
    assert u32(fat,3*4)==EOC
    assert u32(fat,4*4)==5 and u32(fat,5*4)==EOC

    root=image[coff(4):coff(6)]
    assert u32(root,20)==2
    assert u32(root,32+20)==3
    assert u32(root,96+20)==6
    a_first=8
    b_first=12 if args.mode=='growth' else 11
    c_first=17 if args.mode=='growth' else 15
    assert u32(root,192+20)==a_first
    assert u32(root,288+20)==b_first
    assert root[96+1] & 0x02
    assert root[192+1] & 0x02
    assert root[288+1] & 0x02
    sub=image[coff(6):coff(8)]
    assert u32(sub,32+20)==c_first
    assert sub[32+1] & 0x02

    assert image[coff(a_first):coff(a_first+3)] == expected_payload(7,3,3)
    assert image[coff(b_first):coff(b_first+4)] == expected_payload(11,5,4)
    assert image[coff(c_first):coff(c_first+2)] == expected_payload(13,9,2)

    bitmap=image[coff(2):coff(3)]
    def allocated(cluster: int) -> bool:
        bit=cluster-2
        return bool(bitmap[bit>>3] & (1 << (bit & 7)))
    if args.mode=='growth':
        expected=set(range(2,11)) | set(range(12,16)) | set(range(17,19))
        for cluster in range(2,20):
            assert allocated(cluster) == (cluster in expected), cluster
    else:
        for cluster in range(2,17): assert allocated(cluster), cluster
        assert not allocated(17)

    analysed=subprocess.run([str(args.worker),'analyse-json',str(args.image)],
                             check=True,text=True,stdout=subprocess.PIPE).stdout
    payload=json.loads(analysed)
    assert payload['fragmented_files']==0
    assert payload['fragmented_directories']==0
    if args.mode=='growth': assert payload['growth_10_satisfied'] is True
    print(f'verified exFAT {args.mode} canonical layout, metadata and payload integrity')

if __name__=='__main__':
    main()
