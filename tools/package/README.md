# SkyrimPoE 패키지 검사

`plugin/CMakeLists.txt`의 `package_mod`는 빈 패키지 폴더에 파일을 모은 뒤 아래 명령을 순서대로 실행한다. 저장소 루트에서 직접 실행할 때도 같은 순서를 쓴다.

```powershell
python -I tools/package/stamp.py stamp --package "<패키지 폴더>" --esp-name SkyrimPoE.esp
python -I tools/package/stamp.py wrap-tree --package "<패키지 폴더>"
python -I tools/package/stamp.py manifest --package "<패키지 폴더>"
python -I tools/package/stamp.py verify --package "<패키지 폴더>"
```

빌드 ID는 스탬프 전 팩을 포함해 그 시점에 모인 파일의 상대 경로와 SHA-256으로 계산한다. `tree-data.js`는 스탬프를 찍은 트리 팩으로 만들고 최종 해시를 manifest에 기록한다.

`build.bat deploy`는 복사 뒤 `deploy-clean --package "<패키지 폴더>" --target "<모드 폴더>"`를 실행한다. 패키지와 배치 대상에 manifest가 있어야 정리를 시작한다. 두 관리 폴더의 목록 밖 파일을 삭제하고, 그 밖의 파일은 남겨 경로를 출력한다. 마지막으로 배치된 파일의 해시와 manifest를 확인한다.

세대 0 팩은 Path of Building Community 2.68.1의 Path of Exile 3.30 데이터를 사용한다. Path of Exile의 게임 데이터 저작권은 Grinding Gear Games에 있다.
