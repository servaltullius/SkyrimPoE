r"""BethesdaGhidraScripts의 AE 1.6.1170 프로젝트를 읽기 전용으로 열어 함수를 본다(기드라 창 없이).

그 저장소의 가상 환경으로 돌린다(pyghidra가 거기에 있음). 아래 python은 그 가상 환경의 것:
  "G:\skyrim project\REA skyrim\BethesdaGhidraScripts\.venv\Scripts\python.exe" tools\re\gq.py ...

  python tools\re\gq.py dec 1406b7bc0 id:38627      디컴파일(주소 또는 id:번호)
  python tools\re\gq.py where id:38586              함수의 시작 주소, ID, 이름만
  python tools\re\gq.py calls 1406ba870             함수 안의 호출 자리(함수 시작부터의 오프셋, 대상, 대상의 ID)
  python tools\re\gq.py callers 1406b7bc0           이 함수를 부르는 곳(부르는 함수와 그 ID, 오프셋)
여러 명령을 '--'로 이어 한 번에 돌린다. --max N 으로 디컴파일 줄 수 제한.

주의(2026-11-15 확인): 이 기드라 프로젝트에는 함수의 시작만 있고 몸체 범위와 호출 참조가 분석되어 있지 않다.
그래서 'calls'와 'callers'는 비어 나오고, 함수가 정의되지 않은 시작 주소는 'dec'와 'where'에서 임시로 만들어 본다(저장하지 않음). 호출 관계는 callscan.py로 찾는다.
"""
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
        return f'(id 없음)'
    return f'id {i}' + (f' {names[i]}' if i in names else '')


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

        def function_at(addr):
            """그 주소의 함수. 정의된 함수가 없으면 임시로 만든다(읽기 전용으로 연 프로그램이고 트랜잭션을 버리므로 저장되지 않음)."""
            f = fm.getFunctionAt(addr) or fm.getFunctionContaining(addr)
            if f is not None:
                return f, None
            tx = program.startTransaction('gq temporary function')
            if CreateFunctionCmd(addr).applyTo(program, TaskMonitor.DUMMY):
                return fm.getFunctionAt(addr), tx
            program.endTransaction(tx, False)
            return None, None

        for cmd in commands:
            op, targets = cmd[0], cmd[1:]
            for t in targets:
                addr = resolve(t)
                if addr is None:
                    print(f'== {t}: 주소 라이브러리에 없는 ID이거나 읽지 못한 주소')
                    continue
                f, tx = function_at(addr) if op in ('dec', 'where') else (fm.getFunctionAt(addr) or fm.getFunctionContaining(addr), None)
                if f is None:
                    print(f'== {t}: 함수 없음')
                    continue
                entry = f.getEntryPoint()
                rva = entry.getOffset() - IMAGE_BASE
                print(f'== {op} {entry} [{label(rva)}] {f.getName(True)}' + (' (임시로 만든 함수)' if tx is not None else ''))
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
                if tx is not None:
                    program.endTransaction(tx, False)
                print()
    finally:
        program.release(consumer)
