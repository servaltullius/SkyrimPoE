# PoE2 원천 팩

저장소 루트에서 Python 3.11 이상으로 실행합니다. 추출기는 PoB 배포 파일을 읽고 Lua 함수와 수식은 실행하지 않은 채 기록합니다.

```powershell
python -I tools/poe2/extract.py --pob "<PoB 폴더>" --tree-version 0_5 --out data/poe2/raw/0.5 --ko "<ko-KR 폴더>" --summary data/poe2/0.5.summary.json
python -I tools/poe2/extract.py --pob "<PoB 폴더>" --tree-version 0_4 --game-version 0.4 --only tree --out data/poe2/raw/tree-0.4
python -I tools/poe2/extract.py --pob "<PoB 폴더>" --tree-version 0_5 --only tree --out data/poe2/raw/tree-0.5
python -I tools/poe2/diffraw.py --old data/poe2/raw/tree-0.4 --new data/poe2/raw/tree-0.5 --out data/poe2/raw/tree-diff.json --md data/poe2/raw/tree-diff.md
python -I -m unittest discover -s tools/poe2 -p "test_*.py"
```

- `--ko`, `--summary`, `--md`는 선택 사항입니다. `--game-version`의 기본값은 `0.5`입니다. `--only tree`는 `meta.json`과 `tree.json`을 만듭니다. `--allow-out-anywhere`로 시험용 임시 폴더에 쓸 수 있습니다.
- `meta.json`은 판과 스키마를 기록합니다. `sourceSha256`은 읽은 파일과 계산 모듈 다섯 개의 SHA-256, `pobManifestSha1`은 매니페스트에 있는 SHA-1입니다. 경로는 PoB 기준입니다. Windows에서 번역 폴더가 다른 드라이브에 있으면 `ko/파일명`으로 기록합니다.
- Lua 표는 `$table`의 `array`와 `fields`에 배열 원소와 키-값 쌍을 각각 선언 순서대로 담습니다. 실수 원문은 `$number`, 호출과 이름 참조는 `$call`과 `$name`, 함수는 `$function`과 시작 줄에 담습니다. `$binary`는 산술식의 연산자와 피연산자를 보존합니다.
- `gems.json`, `bases.json`, `misc.json`은 파일·키·값을 가진 항목 배열입니다. `duplicateOf`는 앞선 같은 젬 선언의 1부터 시작하는 순번입니다. 베이스의 `overrides`는 PoB 로드 순서에서 앞선 키를 덮는 경우입니다. 일반 유효 키는 로드와 덮어쓰기 처리 뒤 `runeforged`와 `hidden`을 제외해 셉니다.
- `skills.json`에는 스킬 대입 `entries`와 리소스 표 `fileReturns`가 있습니다. `tree.json`, `essences.json`, `questRewards.json`은 반환 표 전체, `mods.json`은 Mod 파일별 반환 표입니다. `uniques.json`은 긴 괄호의 원문 블록과 파일·순번·표시명·베이스를 기록합니다.
- `ko.json`은 파일별 두 열 번역과 `parseWarnings`를 담습니다. 엄격 모드 오류가 난 두 열은 기본 CSV 모드의 결과를 쓰고, 두 열이 아닌 레코드는 파일과 물리 줄 번호를 기록합니다.
- 비교는 종류별 원본 키를 사용합니다. 고유 항목 키는 표시명과 베이스의 쌍이며, 중복 키는 마지막 선언부터 맞춥니다. 변경 경로의 배열 순서는 1부터 셉니다. `possibleRename`은 의미 지문이 같은 삭제·추가 쌍이며 대응표 검토에 사용합니다.

`data/poe2/raw/`의 원천 팩과 비교 보고서는 로컬에 둡니다. 공개 저장소에는 도구와 판·개수·해시만 담은 `*.summary.json`을 넣습니다. PoB 데이터의 권리는 Grinding Gear Games와 해당 배포본의 권리자에게 있습니다.
