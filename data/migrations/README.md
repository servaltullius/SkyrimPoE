# 콘텐츠 세대 대응표

`gen<N>-to-gen<N+1>.json`에 한 세대씩 대응을 적습니다. `schemaVersion`은 1입니다. `fromGeneration`과 `toGeneration`은 부호 없는 정수이며 파일명과 일치해야 합니다. `kinds`는 대응할 종류 목록입니다. `entries`에는 각 종류 아래 출발 세대의 ID를 하나씩 기록합니다. 여러 세대를 건너갈 때는 인접 대응표를 차례로 적용합니다.

| 종류 | ID |
| --- | --- |
| `cls`, `asc`, `gem`, `sup`, `affix`, `unique` | 종류 접두어가 붙은 콘텐츠 ID. ASCII 영숫자, 점, 밑줄로 최대 64바이트 |
| `node`, `mastery` | 양의 u32. 출발 ID는 JSON 속성명이므로 십진 문자열, 목표 ID는 정수 |
| `jewelColor`, `weaponTag`, `slotGroup`, `charm` | 출발 팩과 목표 팩이 정한 ASCII 영숫자·밑줄 키 |

`{"to": ID}`는 목표 ID를 뜻합니다. 그대로 유지할 때도 같은 ID를 적습니다. `{"retire": true}`는 폐기입니다. 환불이 있는 폐기 항목은 `refund`에 단위, 받는 곳, 출발 건당 상한, 중복 방지 기준을 적습니다. 실제 지급량은 종전 할당이나 아이템에서 계산하고 `MIGR`에 기록합니다.

| `unit` | `destination` | `oncePer` |
| --- | --- | --- |
| `passivePoint` | `passivePoints` | `sourceAllocation` |
| `ascendancyPoint` | `ascendancyPoints` | `sourceAllocation` |
| `item` | `inventory` | `sourceInstance` |

`cap`은 양의 u32입니다. `sourceAllocation`은 해당 캐릭터의 출발 세대 할당, `sourceInstance`는 출발 세대의 저장된 실물 개체입니다. 이 값과 출발 세대·종류·ID를 기록해 재시도 시 같은 건을 두 번 지급하지 않습니다.

다음은 가상 세대 1의 ID 세 개만 있는 예시 `gen1-to-gen2.json`입니다.

```json
{
  "schemaVersion": 1,
  "fromGeneration": 1,
  "toGeneration": 2,
  "kinds": ["cls", "node", "gem"],
  "entries": {
    "cls": {"cls.Witch": {"to": "cls.Witch"}},
    "node": {"17788": {"to": 17788}},
    "gem": {
      "gem.OldSpell": {
        "retire": true,
        "refund": {"unit": "item", "destination": "inventory", "cap": 1, "oncePer": "sourceInstance"}
      }
    }
  }
}
```

`python tools/compiler/validate_migration.py <표 경로>`로 스키마를 검사합니다. `--source-baseline <기준선 경로>`를 추가하면 기준선에 있는 종류의 출발 ID가 모두 한 번씩 등장하는지도 검사합니다. 팩 빌드는 모든 출발 ID의 대응과 목표 팩에서의 유효성을 확인한 뒤 다음 세대를 생성해야 합니다.

## 세이브 필드별 처리

| 필드 | 처리 |
| --- | --- |
| 클래스·전직 | 트리보다 먼저 변환합니다. 폐기된 전직은 기존 할당에서 환불액을 계산한 뒤 포인트를 반환합니다. 클래스 사본은 새 클래스에서 채웁니다. |
| TREE 노드·GEAR 주얼 소켓 키 | 같은 노드 대응을 적용한 뒤 트리 연결과 소켓 적격성을 재검증합니다. 소켓 폐기 또는 합류로 밀려난 주얼은 가방으로 반환합니다. |
| 마스터리 | 세대 1부터 초기화합니다. |
| 스킬 칸·GKEY 중첩 젬 | 유지 또는 변환합니다. 폐기는 장착 칸에서 제거하고 실물을 반환합니다. 관련 발동 칸 연결과 오라 비트를 해제합니다. |
| 장비·장착 주얼·지도 어픽스 | 값 배열 구조가 같으면 유지합니다. 구조 변경은 목표 정의로 재검증하고 범위 밖이면 다시 굴립니다. 폐기된 어픽스는 제거합니다. |
| 고유 ID | 변환하거나 폐기합니다. 폐기한 개체는 7단계 계약의 구판 아이템 처리 규칙을 따릅니다. |
| 주얼 색·무기 태그·부위군 | 종류별 대응을 적용합니다. |
| 참 종류·충전 | 새 종류와 그 상한으로 변환·재검증합니다. 폐기된 참은 실물로 반환합니다. |
| 아틀라스 이정표 | 유지합니다. |
| 가방 지도·진행 중 지도 | 지도 아이템은 어픽스 규칙을 따릅니다. 회차는 계약 4.4 단계 1에 따라 종료합니다. |

P7 적용 순서는 대응표 적용, 상태 정화, 포인트 재계산입니다. `MIGR`로 환불과 단계 진행을 기록하고, 전체 이전이 끝나면 `GENR`을 목표 세대로 갱신합니다.
