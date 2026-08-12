#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
from __future__ import annotations
import gzip, json, os, shutil, subprocess, tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
BUILD=Path(os.environ.get('LINUX_DEFRAGGER_BUILD_DIR', ROOT/'build'))
WORKER=BUILD/'linux-defragger-affs-worker'

def run(*args):
    c=subprocess.run([str(WORKER),*map(str,args)],text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,check=False,timeout=30)
    assert c.returncode==0,(c.stdout,c.stderr)
    return c

def analyse(p):
    return json.loads(run('analyse-json',p).stdout)

def fixture(name,td):
    src=ROOT/'tests'/'fixtures'/name
    out=Path(td)/name.removesuffix('.gz')
    with gzip.open(src,'rb') as fi,out.open('wb') as fo: shutil.copyfileobj(fi,fo)
    return out

def main():
  with tempfile.TemporaryDirectory() as td:
    for name,variant,op in [('affs-ffs-fragmented.adf.gz','FFS','defrag'),('affs-ofs-fragmented.adf.gz','OFS','growth-defrag')]:
      p=fixture(name,td); before=analyse(p)
      assert before['variant']==variant and before['fragmented_files']>0
      journal=str(p)+'.journal'
      args=[op,p,'--write','--confirm',p,'--journal',journal,'--live-updates']
      if op=='growth-defrag': args += ['--growth-percent','10']
      c=run(*args)
      assert '@@LIVE_RANGES' in c.stdout and 'allocated blocks only' in c.stdout
      after=analyse(p)
      assert after['fragmented_files']==0
      run('identify',p)
  print('native Amiga OFS/FFS Defrag/Growth Defrag tests passed')
if __name__=='__main__': main()
