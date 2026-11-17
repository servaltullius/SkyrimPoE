r"""BethesdaGhidraScripts의 AE 1.6.1170 프로젝트를 읽기 전용으로 열어 함수를 본다(기드라 창 없이).

그 저장소의 가상 환경으로 돌린다(pyghidra가 거기에 있음). 아래 python은 그 가상 환경의 것:
  "G:\skyrim project\REA skyrim\BethesdaGhidraScripts\.venv\Scripts\python.exe" tools\re\gq.py ...

  python tools\re\gq.py dec 1406b7bc0 id:38627      디컴파일(주소 또는 id:번호)
  python tools\re\gq.py where id:38586              함수의 시작 주소, ID, 이름만
  python tools\re\gq.py calls 1406ba870             함수 안의 호출 자리(함수 시작부터의 오프셋, 대상, 대상의 ID)
  python tools\re\gq.py callers 1406b7bc0           이 함수를 부르는 곳(부르는 함수와 그 ID, 오프셋)
여러 명령을 '--'로 이어 한 번에 돌린다. --max N 으로 디컴파일 줄 수 제한.

주의(2026-11-15 확인): 이 기드라 프로젝트에는 함수의 시작만 있고 몸체 범위와 호출 참조가 분석되어 있지 않다.
그래서 'calls'와 'callers'는 비어 나온다. 호출 관계는 callscan.py로 찾는다.
함수가 정의되지 않은 주소는 'dec'와 'where'에서, 그 주소가 주소 라이브러리의 ID 자리일 때만 임시로 함수를 만들어 본다
(저장하지 않음). 함수 중간의 주소에서 만들면 앞뒤가 잘린 가짜 함수가 되므로, 그런 주소는 앞의 가장 가까운 ID를 알려 주고 넘어간다.
"""
import bisect
import sys
from pathlib import Path

REPO = Path(r'G:\skyrim project\REA skyrim\BethesdaGhidraScripts')
sys.path.insert(0, r'G:\skyrim project\REA skyrim\perf-probe')
from skydb import load_addrlib, load_all_names  # noqa: E402

ADDRLIB = REPO / 'addresslibrary' / 'sse' / 'versionlib-1-6-1170-0.bin'
IMAGE_BASE = 0x140000000

args = sys.argv[1:]
max_lines = 400
if '--max' in args:
    i = args.index('--max')
    max_lines = int(args[i + 1])
    del args[i:i + 2]
commands = []
current = []
for a in args:
    if a == '--':
        if current:
            commands.append(current)
        current = []
    else:
        current.append(a)
if current:
    commands.append(current)

rva2id = load_addrlib(str(ADDRLIB))
id2rva = {v: k for k, v in rva2id.items()}
id_starts = sorted(rva2id)
names = load_all_names()

import pyghidra  # noqa: E402
pyghidra.start(install_dir=REPO / 'tools' / 'ghidra')
from ghidra.util.task import TaskMonitor  # noqa: E402
from ghidra.app.decompiler import DecompInterface  # noqa: E402
from ghidra.app.cmd.function import CreateFunctionCmd  # noqa: E402
import java.lang  # noqa: E402


def label(rva):
    i = rva2id.get(rva)
    if i is None:
        return '(id 없음)'
    return f'id {i}' + (f' {names[i]}' if i in names else '')


def nearest_before(rva):
    """rva보다 앞에서 가장 가까운 주소 라이브러리 ID와 거리. 없으면 빈 문자열."""
    k = bisect.bisect_right(id_starts, rva) - 1
    if k < 0:
        return ''
    start = id_starts[k]
    return f'{label(start)} +0x{rva - start:X}'


with pyghidra.open_project(str(REPO / 'ghidraprojects' / 'BethesdaGhidraScripts'), 'BethesdaGhidraScripts') as project:
    df = project.getProjectData().getRootFolder().getFolder('skyrim').getFolder('ae').getFiles()[0]
    consumer = java.lang.Object()
    program = df.getReadOnlyDomainObject(consumer, -1, TaskMonitor.DUMMY)
    try:
        base = program.getImageBase()
        fm = program.getFunctionManager()
        listing = program.getListing()
        refman = program.getReferenceManager()
        dec = DecompInterface()
        dec.openProgram(program)

        def resolve(text):
            """'id:번호' 또는 16진 주소. 주소 라이브러리에 없는 ID나 읽지 못한 주소면 None."""
            if text.startswith('id:'):
                rva = id2rva.get(int(text[3:])) if text[3:].isdigit() else None
                return base.add(rva) if rva is not None else None
            return program.getAddressFactory().getAddress(text)

        def existing(addr):
            return fm.getFunctionAt(addr) or fm.getFunctionContaining(addr)

        def show(op, f, temporary):
            """함수 하나에 대해 명령을 실행해 찍는다."""
            entry = f.getEntryPoint()
            rva = entry.getOffset() - IMAGE_BASE
            print(f'== {op} {entry} [{label(rva)}] {f.getName(True)}' + (' (임시로 만든 함수)' if temporary else ''))
            if op == 'dec':
                r = dec.decompileFunction(f, 120, TaskMonitor.DUMMY)
                code = r.getDecompiledFunction().getC() if r and r.decompileCompleted() else '(디컴파일 실패)'
                lines = [x for x in code.splitlines() if not x.startswith('/* WARNING')]
                print('\n'.join(lines[:max_lines]))
                if len(lines) > max_lines:
                    print(f'... ({len(lines) - max_lines}줄 더)')
            elif op == 'calls':
                it = listing.getInstructions(f.getBody(), True)
                while it.hasNext():
                    ins = it.next()
                    for ref in ins.getReferencesFrom():
                        if ref.getReferenceType().isCall():
                            to = ref.getToAddress()
                            tf = fm.getFunctionAt(to)
                            trva = to.getOffset() - IMAGE_BASE
                            off = ins.getAddress().subtract(entry)
                            print(f'  +{off:X}  {ins}  -> {to} [{label(trva)}] {tf.getName(True) if tf else ""}')
            elif op == 'callers':
                for ref in refman.getReferencesTo(entry):
                    if not ref.getReferenceType().isCall() and not ref.getReferenceType().isData():
                        continue
                    src = ref.getFromAddress()
                    sf = fm.getFunctionContaining(src)
                    if sf is None:
                        print(f'  {src} (함수 밖, {ref.getReferenceType()})')
                        continue
                    se = sf.getEntryPoint()
                    print(f'  {src} = {se}+{src.subtract(se):X} [{label(se.getOffset() - IMAGE_BASE)}] {sf.getName(True)} ({ref.getReferenceType()})')
            elif op != 'where':
                print(f'  알 수 없는 명령: {op}')

        for cmd in commands:
            op, targets = cmd[0], cmd[1:]
            for t in targets:
                addr = resolve(t)
                if addr is None:
                    print(f'== {t}: 주소 라이브러리에 없는 ID이거나 읽지 못한 주소')
                    print()
                    continue
                rva = addr.getOffset() - IMAGE_BASE
                f = existing(addr)
                if f is not None:
                    show(op, f, False)
                elif op in ('dec', 'where') and rva in rva2id:
                    # 임시 함수: 트랜잭션은 무슨 일이 있어도 버린다(저장하지 않음)
                    tx = program.startTransaction('gq temporary function')
                    try:
                        if CreateFunctionCmd(addr).applyTo(program, TaskMonitor.DUMMY) and fm.getFunctionAt(addr) is not None:
                            show(op, fm.getFunctionAt(addr), True)
                        else:
                            print(f'== {t}: 함수를 만들지 못함({label(rva)})')
                    finally:
                        program.endTransaction(tx, False)
                else:
                    print(f'== {t}: 함수 없음(주소 라이브러리의 ID 자리가 아님. 앞의 가장 가까운 ID: {nearest_before(rva) or "없음"})')
                print()
    finally:
        program.release(consumer)
