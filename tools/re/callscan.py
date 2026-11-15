"""실행 파일(Steamless로 푼 것)의 실행 구역에서 직접 호출(E8 rel32)과 점프(E9 rel32)를 훑어, 주어진 ID의 함수로 가는 자리를 찾는다.

BethesdaGhidraScripts의 가상 환경으로 돌린다(pefile이 거기에 있음):
  "G:\\skyrim project\\REA skyrim\\BethesdaGhidraScripts\\.venv\\Scripts\\python.exe" tools\\re\\callscan.py 38586 37335 --where

출력: 대상 ID, 호출 자리의 RVA와 절대 주소(이미지 기준 0x140000000), call/jmp.
--where 를 주면 호출 자리마다 그보다 앞의 가장 가까운 주소 라이브러리 ID(대개 그 자리를 품은 함수)와 이름을 붙인다.
가짜 일치(우연히 E8로 시작하는 바이트)가 섞일 수 있고, 가장 가까운 ID가 함수 시작이 아닐 수도 있으므로 gq.py로 다시 본다.
간접 호출(가상 함수, 함수 포인터)은 찾지 못한다. 대상은 AE 1.6.1170 하나(설계-현황 6장).
"""
import bisect
import sys
from pathlib import Path

import pefile

sys.path.insert(0, r'G:\skyrim project\REA skyrim\perf-probe')
from skydb import load_addrlib, load_all_names  # noqa: E402

REPO = Path(r'G:\skyrim project\REA skyrim\BethesdaGhidraScripts')
EXE = REPO / 'exes' / 'skyrim' / 'ae' / 'SkyrimSE.exe.unpacked.exe'
ADDRLIB = REPO / 'addresslibrary' / 'sse' / 'versionlib-1-6-1170-0.bin'
IMAGE_BASE = 0x140000000


def main() -> int:
    where = '--where' in sys.argv
    ids = [x for x in sys.argv[1:] if x != '--where']
    if not ids:
        print(__doc__)
        return 1
    rva2id = load_addrlib(str(ADDRLIB))
    id2rva = {v: k for k, v in rva2id.items()}
    missing = [x for x in ids if int(x) not in id2rva]
    if missing:
        print(f'주소 라이브러리에 없는 ID: {", ".join(missing)}', file=sys.stderr)
        return 1
    wanted = {id2rva[int(x)]: int(x) for x in ids}
    names = load_all_names() if where else {}
    starts = sorted(rva2id)

    def owner(rva: int) -> str:
        k = bisect.bisect_right(starts, rva) - 1
        fid = rva2id[starts[k]]
        return f'\tid {fid} +{rva - starts[k]:X}\t{names.get(fid, "-")}'

    pe = pefile.PE(str(EXE), fast_load=True)
    for section in pe.sections:
        if not section.Characteristics & 0x20000000:  # IMAGE_SCN_MEM_EXECUTE
            continue
        data = section.get_data()
        start = section.VirtualAddress
        for i in range(len(data) - 5):
            op = data[i]
            if op != 0xE8 and op != 0xE9:
                continue
            rel = int.from_bytes(data[i + 1:i + 5], 'little', signed=True)
            target = start + i + 5 + rel
            if target in wanted:
                rva = start + i
                line = f'{wanted[target]}\t{rva:X}\t{IMAGE_BASE + rva:X}\t{"call" if op == 0xE8 else "jmp"}'
                print(line + (owner(rva) if where else ''))
    return 0


if __name__ == '__main__':
    sys.exit(main())
