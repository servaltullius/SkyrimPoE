# Skyrim PoE (가칭)

스카이림 SE/AE에서 Path of Exile 1의 빌드·파밍·엔드게임을 구현하려는 SKSE 모드입니다. 내부 이름은 `SkyrimPoE`이고 공개 제목은 아직 정하지 않았습니다.

A work-in-progress SKSE mod that aims to bring Path of Exile 1 style builds, loot and endgame to Skyrim SE/AE. The project is at the design and prototype stage, and the documents are written in Korean.

## 지금 단계

**설계와 프로토타입 단계입니다. 설치해서 플레이할 수 있는 모드는 아직 없습니다.**

- 본 모드의 코드는 없습니다.
- `prototypes/`에는 위험 요소를 먼저 확인하는 시험용 코드와 모드가 있습니다. 한 모드팩(AE 1.6.1170)에서만 돌려 봤고, 다른 환경에서의 동작은 확인하지 않았습니다.
- 설계 문서에는 아직 정하지 않은 항목과 실행으로 확인하지 못한 가정이 많이 남아 있습니다. 문서 안에서 `[확정]`, `[잠정]`, `[미정]`, `미확인`으로 구분해 적습니다.

## 폴더

| 경로 | 내용 |
| --- | --- |
| `docs/통합-설계서.md` | 지금의 설계. 무엇을 어떻게 만드는지 |
| `docs/설계-현황.md` | 결정 기록, 검증된 사실과 조사 근거, 미결정 사항 |
| `docs/carbon-원본/` | 이전 도구(Gemini Carbon)가 쓴 설계서 원문. 참고용이며 알려진 오류를 그대로 담고 있음 |
| `docs/검토/` | 설계서에 대한 교차 검토 기록 |
| `prototypes/01-cyclone-motion/` | 사이클론 회전 모션을 재생하는 두 방식의 비교 |
| `prototypes/02-skill-input/` | 강공격 입력에 채널링 스킬을 얹는 SKSE 플러그인(회전, 범위 타격, 치명타 시 발동) |
| `prototypes/03-cyclone-clip/` | 회전 자세 애니메이션 클립을 만드는 스크립트 |
| `prototypes/04-passive-tree/` | 패시브 트리를 Canvas 2D로 그리는 화면과 PoB 트리 데이터 변환 스크립트, 그것을 게임 안(Prisma UI)에 띄우는 플러그인 |
| `prototypes/05-item-identity/` | 아이템에 붙인 표시가 옮기기·버리기·장착·저장을 거쳐도 남는지 스스로 시험하는 플러그인 |
| `prototypes/06-projectile/` | 주문 없이 쏜 투사체가 무해한지, 적중을 알 수 있는지, 맞은 자리에서 다시 쏠 수 있는지(연쇄·분기·관통)를 스스로 시험하는 플러그인 |

각 프로토타입 폴더의 `README.md`에 동작 방식, 시험 방법, 시험 결과가 있습니다.

## 저장소에 없는 것

- 다른 사람이 만든 애니메이션 파일(HKX)과 그 복사본은 넣지 않았습니다. 프로토타입 1과 3을 그대로 재현하려면 해당 파일을 따로 구해야 합니다.
- Path of Exile의 게임 파일과 Path of Building의 데이터는 들어 있지 않습니다.
- 문서에 적힌 `G:\...` 같은 경로는 작업한 컴퓨터의 경로입니다. 사용자 이름은 `USERNAME`으로 바꿔 적었습니다.

## 라이선스

소스 코드는 [GPL-3.0](LICENSE)입니다.

프로토타입 2는 엔진의 근접 타격 함수의 주소와 인자를 [Precision](https://github.com/ersh1/Precision)(ersh1, GPL-3.0)의 소스에서 확인해 씁니다. 빌드에는 [CommonLibSSE-NG](https://github.com/alandtse/CommonLibSSE-NG)(alandtse, GPL-3.0-or-later와 모딩 예외)가 필요합니다. 프로토타입은 v9.0.2로 빌드했고 그 판은 이 저장소에 포함하지 않았습니다. 본 모드는 v16.4.0으로 빌드하기로 했고, 그 판을 `external/CommonLibSSE-NG`에 서브모듈로 두었습니다(`git submodule update --init external/CommonLibSSE-NG`로 받습니다).

## 알림

- 이 프로젝트는 팬이 만드는 비공식 모드입니다. Grinding Gear Games와 제휴하거나 승인을 받은 것이 아닙니다. Path of Exile은 Grinding Gear Games의 상표입니다.
- The Elder Scrolls V: Skyrim은 Bethesda Softworks의 상표입니다.
- 이 프로젝트로 수익을 내지 않습니다.
