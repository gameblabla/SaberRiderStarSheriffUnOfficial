"""Bake the shooter event schedule from space.c into integer native records."""
import re
import struct
KINDS={'E_MINE':1,'E_LMINE':2,'E_SMINE':3,'E_FIGHT':4,'E_GUN':5}
VALUES={'P_LINE':0,'P_SINE':1,'P_SWOOP':2,'P_REAR':3,'CAP_P':1,'CAP_S':2,'CAP_B':3}
def bake(path):
    source=path.read_text().split('static const Ev TL[] = {',1)[1].split('};',1)[0]
    rows=[]
    for entry in re.findall(r'\{([^{}]+)\}',source):
        parts=[p.strip() for p in re.sub(r'R\(([^)]+)\)',r'\1',entry).split(',')]
        if parts[1] not in KINDS:continue
        parts+=['0']*(7-len(parts))
        def value(s):return VALUES[s] if s in VALUES else float(s.rstrip('f'))
        t,kind,n,y,gap,pat,drop=parts
        rows.append(struct.pack('<HBBhHBB',round(value(t)*60),KINDS[kind],int(n),round(value(y)),round(value(gap)*60) if kind=='E_FIGHT' else round(value(gap)),int(value(pat)),int(value(drop))))
    return bytes([len(rows)])+b''.join(rows)
