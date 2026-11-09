#!/usr/bin/env python3
"""List the absolute peripheral addresses an ARM ELF touches (best effort, no execution).

    python firmware/tools/audit_addrs.py build/km_s3.elf        (arm-none-eabi-objdump must be on PATH)

Reads `objdump -d`, follows the literal pool (`ldr rX,[pc,..]`), movw/movt and add/sub of constants through the
registers of each function, and prints `address R|W function` for every ldr/str whose base it could resolve.
Loops that compute addresses from a counter are not followed (the host test tests/test_sense.c logs the exact
accesses of the sensing driver instead). Used for the M3 address audit in SENSING_BRINGUP_NOTES.md section 5.
"""
import re,subprocess,sys,collections
elf=sys.argv[1]
out=subprocess.run(['arm-none-eabi-objdump','-d','--no-show-raw-insn',elf],capture_output=True,text=True).stdout
lines=out.splitlines()
pool={}
for l in lines:
    m=re.match(r'\s*([0-9a-f]+):\s+\.word\s+0x([0-9a-f]+)',l)
    if m: pool[int(m.group(1),16)]=int(m.group(2),16)
func=None; regs={}
acc=collections.defaultdict(set)
def val(s):
    s=s.strip().lstrip('#')
    return int(s,0)
for l in lines:
    m=re.match(r'^[0-9a-f]+ <(.+)>:',l)
    if m: func=m.group(1); regs={}; continue
    m=re.match(r'\s+([0-9a-f]+):\s+(\S+)\s*(.*)$',l)
    if not m: continue
    pc=int(m.group(1),16); op=m.group(2); rest=m.group(3).split('@')[0].strip()
    base=op.split('.')[0]
    if base=='ldr' and 'pc' in rest:
        mm=re.match(r'(\w+),\s*\[pc',rest); tgt=re.search(r'@\s*\(([0-9a-f]+)',m.group(3))
        if mm and tgt and int(tgt.group(1),16) in pool: regs[mm.group(1)]=pool[int(tgt.group(1),16)]
        continue
    if base=='movw':
        r,v=rest.split(',')[0:2]; regs[r.strip()]=val(v); continue
    if base=='movt':
        r,v=rest.split(',')[0:2]; r=r.strip()
        if r in regs: regs[r]=(regs[r]&0xffff)|(val(v)<<16)
        continue
    if base=='mov' :
        p=[x.strip() for x in rest.split(',')]
        if len(p)==2 and p[1].startswith('#'): regs[p[0]]=val(p[1])
        elif len(p)==2 and p[1] in regs: regs[p[0]]=regs[p[1]]
        else: regs.pop(p[0],None)
        continue
    if base in('add','adds','sub','subs') :
        p=[x.strip() for x in rest.split(',')]
        if len(p)==3 and p[1] in regs and p[2].startswith('#'):
            regs[p[0]]=(regs[p[1]]+(val(p[2]) if base.startswith('add') else -val(p[2])))&0xffffffff
        elif len(p)==2 and p[0] in regs and p[1].startswith('#'):
            regs[p[0]]=(regs[p[0]]+(val(p[1]) if base.startswith('add') else -val(p[1])))&0xffffffff
        else: regs.pop(p[0],None)
        continue
    if re.match(r'(ldr|str)(b|h)?$',base) and '[' in rest:
        mm=re.search(r'\[(\w+)(?:,\s*#(-?\d+))?\]',rest)
        if mm and mm.group(1) in regs:
            a=(regs[mm.group(1)]+int(mm.group(2) or 0))&0xffffffff
            kind='W' if base.startswith('str') else 'R'
            acc[func].add((a,kind))
        continue
    # anything else writing a register: drop knowledge of first operand
    p=rest.split(',')
    if p and p[0].strip() in regs and base not in('cmp','tst','str','b','bl','bx','push','pop','cbz','cbnz'):
        regs.pop(p[0].strip(),None)
    if base=='bl' : 
        for r in ('r0','r1','r2','r3','ip'): regs.pop(r,None)
rows=sorted(((a,k,f) for f,s in acc.items() for (a,k) in s if (a>=0x40000000 and a<0x60000000) or a>=0xE0000000 or a<0x08008000 and a>=0x08000000))
for a,k,f in rows: print('%08x %s %s'%(a,k,f))
