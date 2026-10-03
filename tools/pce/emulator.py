"""Persistent control for the supplied accurate-core Mednafen PCE emulator."""
import json
from pathlib import Path
import subprocess
from urllib.parse import quote

ROOT = Path(__file__).resolve().parents[3]
BIOS = ROOT / 'PCE/[BIOS] Super CD-ROM System (Japan) (En) (v3.0).pce'
BINARY = ROOT / 'PCE/mednafen-pce-headless'

class Emulator:
    def __init__(self, cue, base, bios=BIOS, arcade=True):
        base = Path(base).resolve(); base.mkdir(parents=True, exist_ok=True)
        # These are accurate-core hardware settings; never use unlimited SAT.
        (base/'pce.cfg').write_text(f'pce.arcadecard {int(arcade)}\npce.nospritelimit 0\n')
        self.cue=Path(cue).resolve()
        self.log = open(base/'emulator.log', 'w')
        self.proc = subprocess.Popen([str(BINARY), '--rom', str(Path(cue).resolve()), '--bios', str(bios),
                                      '--base-dir', str(base), '--frames', '0', '--rpc'],
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.log, text=True, bufsize=1)
    def call(self, *args):
        self.proc.stdin.write('\t'.join(str(a) for a in args)+'\n'); self.proc.stdin.flush()
        while True:
            line=self.proc.stdout.readline()
            if not line: raise RuntimeError('Emulator exited; see emulator.log')
            if not line.startswith('{'): continue
            result=json.loads(line)
            if not result.get('ok'): raise RuntimeError(result)
            return result
    def run(self, n): return self.call('run', n)
    def input(self, mask): return self.call('input', mask)
    def memory(self, address, size, logical=True):
        return bytes.fromhex(self.call('memread', address, size, int(logical))['hex'])
    def write(self,address,data,space="cpu"):
        return self.call("aswrite",space,address,bytes(data).hex())
    def screenshot(self, path):
        Path(path).unlink(missing_ok=True)
        return self.call('screenshot', quote(str(Path(path).resolve()), safe='/'))
    def close(self):
        try:
            if self.proc.poll() is None: self.call('quit')
        finally: self.proc.wait(timeout=10); self.log.close()
    def __enter__(self): return self
    def __exit__(self, *args): self.close()

def symbol(elf, name):
    out=subprocess.check_output([str(ROOT/'PCE/llvm-mos8/bin/llvm-nm'),str(elf)],text=True)
    for line in out.splitlines():
        fields=line.split()
        if len(fields)==3 and fields[2]==name: return int(fields[0],16)&65535
    raise ValueError(f'Missing ELF symbol: {name}')

def boot(emu, address, limit=20000):
    emu.run(120); emu.input(8); emu.run(5); emu.input(0)
    ui=symbol(Path(emu.cue).parent/'app.elf','pce_ui_state')
    for n in range(0,limit,120):
        emu.run(120)
        data=emu.memory(address,36)
        if data[:4]==b'SRPC' and data[5]: return data
        state=emu.memory(ui,1)[0] if data[:4]==b'SRPC' else 0
        if state:
            # Menus are real user input paths; select resumes an existing save,
            # otherwise start, select the default sheriff and page the briefing.
            emu.run(240)
            key=4 if state==1 else 1
            emu.input(key);emu.run(30);emu.input(0);emu.run(30)
            if state==1 and emu.memory(ui,1)[0]==1:
                emu.input(1);emu.run(30);emu.input(0);emu.run(30)
        if data[:4]==b'SRPC' and int.from_bytes(data[22:24],'little'): raise RuntimeError(f'CD load error: {data.hex()}')
    raise RuntimeError(f'Boot timed out: {emu.memory(address,32).hex()}')
