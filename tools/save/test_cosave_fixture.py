from __future__ import annotations

from pathlib import Path
import struct
import zlib

import pytest

import cosave_fixture as cf


def make_strt_payload(ids: list[str]) -> bytes:
    out = struct.pack("<I", len(ids))
    for item in ids:
        raw = item.encode("utf-8")
        out += struct.pack("<I", len(raw)) + raw
    return out


def make_synthetic_cosave(
    *,
    spoe_chunks: list[cf.Chunk] | None = None,
    other_before: bytes = b"\xAA\xBB\xCC\xDD",
    other_after: bytes = b"\x11\x22\x33\x44\x55",
    include_spoe: bool = True,
    duplicate_spoe: bool = False,
) -> bytes:
    if spoe_chunks is None:
        spoe_chunks = [
            cf.Chunk(cf.TAG_CORE, cf.CORE_GEN1_VERSION, b"\x01\x02\x03\x04"),
            cf.Chunk(cf.fourcc("PRAT"), 1, b"\x00" * 16),
            cf.Chunk(cf.TAG_STRT, cf.STRT_VERSION, make_strt_payload(["gem.Spark"])),
            cf.Chunk(
                cf.TAG_GENR,
                cf.GENR_VERSION,
                cf.encode_genr(cf.GenrRecord("poe2", 1, "0.5", "0123456789abcdef", "0.1.0")),
            ),
        ]
    plugins: list[cf.PluginBlock] = [
        cf.PluginBlock(uid=cf.fourcc("OTHA"), num_chunks=1, raw_body=other_before),
    ]
    if include_spoe:
        plugins.append(cf.PluginBlock(uid=cf.SPOE_UID, num_chunks=len(spoe_chunks), raw_body=b"", chunks=spoe_chunks))
    if duplicate_spoe:
        plugins.append(cf.PluginBlock(uid=cf.SPOE_UID, num_chunks=len(spoe_chunks), raw_body=b"", chunks=spoe_chunks))
    plugins.append(cf.PluginBlock(uid=cf.fourcc("OTHB"), num_chunks=2, raw_body=other_after))
    return cf.CosaveFile(format_version=1, skse_version=0x02020600, runtime_version=0x01064920, plugins=plugins).encode()


def write_source_pair(saves_dir: Path, stem: str = "g1_src", skse_bytes: bytes | None = None) -> tuple[Path, Path]:
    ess = saves_dir / f"{stem}.ess"
    skse = saves_dir / f"{stem}.skse"
    ess.write_bytes(b"TESV_SAVEGAME_SYNTHETIC")
    skse.write_bytes(skse_bytes if skse_bytes is not None else make_synthetic_cosave())
    return ess, skse


def test_known_answers_spoe_disk_bytes_and_crc32() -> None:
    # 디스크 리틀 엔디언 'SPOE' == 45 4F 50 53 ("EOPS"), 표준 CRC-32/ISO-HDLC 검증 벡터.
    assert struct.pack("<I", cf.SPOE_UID) == bytes([0x45, 0x4F, 0x50, 0x53])
    assert (zlib.crc32(b"123456789") & 0xFFFFFFFF) == 0xCBF43926
    raw = make_synthetic_cosave()
    assert raw[:4] == b"SKSE"
    assert bytes([0x45, 0x4F, 0x50, 0x53]) in raw


def test_parse_cosave_boundary_errors() -> None:
    valid = make_synthetic_cosave()

    # 1. 잘린 파일 머리(< 20바이트)
    with pytest.raises(ValueError, match="20바이트"):
        cf.parse_cosave(valid[:19])

    # 2. 잘못된 서명
    with pytest.raises(ValueError, match="서명"):
        cf.parse_cosave(b"XXXX" + valid[4:])

    # 3. 플러그인 길이 초과
    bad_plugin_len = bytearray(valid)
    # 첫 플러그인 헤더(offset 20): uid(4), num_chunks(4), length(4) -> offset 28
    struct.pack_into("<I", bad_plugin_len, 28, 999999)
    with pytest.raises(ValueError, match="범위를 벗어났습니다"):
        cf.parse_cosave(bytes(bad_plugin_len))

    # 4. SPOE 청크 길이 초과
    bad_chunk = struct.pack("<III", cf.TAG_CORE, 3, 999) + b"\x01\x02"
    raw_bad_chunk = (
        struct.pack("<IIIII", cf.SKSE_SIGNATURE, 1, 1, 1, 1)
        + struct.pack("<III", cf.SPOE_UID, 1, len(bad_chunk))
        + bad_chunk
    )
    with pytest.raises(ValueError, match="본문이 범위를 벗어났습니다"):
        cf.parse_cosave(raw_bad_chunk)

    # 5. 청크 수 불일치(실제보다 더 많은 청크 선언 / 적은 청크 선언으로 남는 바이트)
    good_chunk = struct.pack("<III", cf.TAG_CORE, 3, 2) + b"\x01\x02"
    too_many = (
        struct.pack("<IIIII", cf.SKSE_SIGNATURE, 1, 1, 1, 1)
        + struct.pack("<III", cf.SPOE_UID, 2, len(good_chunk))
        + good_chunk
    )
    with pytest.raises(ValueError, match="청크 머리"):
        cf.parse_cosave(too_many)

    too_few = (
        struct.pack("<IIIII", cf.SKSE_SIGNATURE, 1, 1, 1, 1)
        + struct.pack("<III", cf.SPOE_UID, 1, len(good_chunk) * 2)
        + good_chunk
        + good_chunk
    )
    with pytest.raises(ValueError, match="남았습니다"):
        cf.parse_cosave(too_few)

    # 6. 파일 끝 남는 바이트
    with pytest.raises(ValueError, match="남았습니다"):
        cf.parse_cosave(valid + b"\xFF")


def test_strt_validation_matches_cpp_rules() -> None:
    # 정상 ID
    ok_chunk = cf.Chunk(cf.TAG_STRT, cf.STRT_VERSION, make_strt_payload(["gem.Spark", "sup.Chain_I.v2"]))
    assert cf.validate_strt_chunk(ok_chunk) == ["gem.Spark", "sup.Chain_I.v2"]

    # 접두어 대문자·숫자·비ASCII·빈 구간·허용 외 기호·64바이트 초과·중복 모두 거부
    for bad_ids in [
        ["Gem.Spark"],
        ["gem1.Spark"],
        [".Spark"],
        ["gem."],
        ["gem.Spark!"],
        ["gem.스파크"],
        ["gem." + "a" * 61],
        ["gem.Spark", "gem.Spark"],
    ]:
        bad_chunk = cf.Chunk(cf.TAG_STRT, cf.STRT_VERSION, make_strt_payload(bad_ids))
        with pytest.raises(ValueError):
            cf.validate_strt_chunk(bad_chunk)


def test_make_all_cases_preserves_other_plugins_and_untouched_spoe_chunks(tmp_path: Path) -> None:
    src_ess, src_skse = write_source_pair(tmp_path)
    src_parsed = cf.parse_cosave(src_skse.read_bytes())
    _, src_spoe = cf.find_single_spoe(src_parsed, "src")
    assert src_spoe.chunks is not None
    orig_core = [c for c in src_spoe.chunks if c.chunk_type == cf.TAG_CORE][0]
    orig_prat = [c for c in src_spoe.chunks if c.chunk_type == cf.fourcc("PRAT")][0]
    orig_genr = [c for c in src_spoe.chunks if c.chunk_type == cf.TAG_GENR][0]
    orig_strt = [c for c in src_spoe.chunks if c.chunk_type == cf.TAG_STRT][0]

    rc = cf.main(["make", "--saves", str(tmp_path), "--source", "g1_src", "--out-prefix", "neg"])
    assert rc == 0

    for case in cf.ALL_CASES:
        out_ess = tmp_path / f"neg_{case}.ess"
        out_skse = tmp_path / f"neg_{case}.skse"
        assert out_ess.read_bytes() == src_ess.read_bytes()

        parsed = cf.parse_cosave(out_skse.read_bytes())
        # SPOE 앞뒤 플러그인 바이트와 순서는 완전히 그대로 유지된다.
        assert len(parsed.plugins) == 3
        assert parsed.plugins[0] == src_parsed.plugins[0]
        assert parsed.plugins[2] == src_parsed.plugins[2]

        _, spoe = cf.find_single_spoe(parsed, case)
        assert spoe.chunks is not None
        assert spoe.num_chunks == len(spoe.chunks)

        # 모든 케이스에서 대상이 아닌 CORE·PRAT 청크는 바이트 그대로 유지된다.
        assert [c for c in spoe.chunks if c.chunk_type == cf.TAG_CORE] == [orig_core]
        assert [c for c in spoe.chunks if c.chunk_type == cf.fourcc("PRAT")] == [orig_prat]

        genrs = [c for c in spoe.chunks if c.chunk_type == cf.TAG_GENR]
        strts = [c for c in spoe.chunks if c.chunk_type == cf.TAG_STRT]

        if case.startswith("genr") or case == "nogenr":
            assert strts == [orig_strt]
        else:
            assert genrs == [orig_genr]

        if case == "nogenr":
            assert len(genrs) == 0
            assert len(spoe.chunks) == len(src_spoe.chunks) - 1
        elif case == "genr-corrupt":
            assert len(genrs) == 1 and len(genrs[0].payload) == len(orig_genr.payload)
            assert genrs[0].payload[:-1] == orig_genr.payload[:-1]
            assert genrs[0].payload[-1] == (orig_genr.payload[-1] ^ 0xFF)
            with pytest.raises(ValueError, match="CRC32"):
                cf.decode_genr(genrs[0])
        elif case == "genr-trunc":
            assert len(genrs) == 1 and len(genrs[0].payload) == len(orig_genr.payload) - 2
            with pytest.raises(ValueError):
                cf.decode_genr(genrs[0])
        elif case == "genr-highgen":
            assert len(genrs) == 1
            rec = cf.decode_genr(genrs[0])
            assert rec.game == "poe2" and rec.content_generation == 2 and rec.build_id == "0123456789abcdef"
        elif case == "strt-missing":
            assert len(strts) == 0
            assert len(spoe.chunks) == len(src_spoe.chunks) - 1
        elif case == "strt-dup":
            assert len(strts) == 2
            assert strts[0] == orig_strt and strts[1] == orig_strt
            idx0 = spoe.chunks.index(strts[0])
            assert spoe.chunks[idx0 + 1] == strts[1]
        elif case == "strt-corrupt":
            assert len(strts) == 1 and len(strts[0].payload) == len(orig_strt.payload)
            bad_count = struct.unpack_from("<I", strts[0].payload, 0)[0]
            assert bad_count == cf.MAX_STARTER_IDS + 1
            with pytest.raises(ValueError):
                cf.validate_strt_chunk(strts[0])


def test_make_partial_cases_unknown_case_and_name_restrictions(tmp_path: Path) -> None:
    write_source_pair(tmp_path, "src_ok")

    # --case 부분 선택
    rc = cf.main([
        "make", "--saves", str(tmp_path), "--source", "src_ok",
        "--out-prefix", "part", "--case", "nogenr", "--case", "strt-dup",
    ])
    assert rc == 0
    assert (tmp_path / "part_nogenr.skse").is_file()
    assert (tmp_path / "part_strt-dup.skse").is_file()
    assert not (tmp_path / "part_genr-corrupt.skse").exists()

    # 모르는 케이스 거부
    assert cf.main([
        "make", "--saves", str(tmp_path), "--source", "src_ok",
        "--out-prefix", "badcase", "--case", "no-such-case",
    ]) == 2

    # 출력 이름 == 원본 이름 거부
    write_source_pair(tmp_path, "dup_nogenr")
    assert cf.main([
        "make", "--saves", str(tmp_path), "--source", "dup_nogenr",
        "--out-prefix", "dup", "--case", "nogenr",
    ]) == 2

    # 콜론(:)·경로 구분자·Windows 금지 문자 거부
    for bad in ["C:src_ok", "src:ads", "sub/src_ok", "sub\\src_ok", "bad*name", "bad?name", "bad<name>", "bad|name", 'bad"name']:
        assert cf.main(["make", "--saves", str(tmp_path), "--source", bad, "--out-prefix", "x"]) == 2
        assert cf.main(["make", "--saves", str(tmp_path), "--source", "src_ok", "--out-prefix", bad]) == 2


def test_make_rejects_bad_source_genr_strt_and_rolls_back_on_overwrite_race(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    write_source_pair(tmp_path, "src_ok")
    existing = tmp_path / "out_strt-dup.skse"
    existing.write_bytes(b"KEEP_ME")

    # 사전 검사: 하나라도 이미 있으면 아무 파일도 쓰지 않고 거부(종료 코드 2).
    assert cf.main(["make", "--saves", str(tmp_path), "--source", "src_ok", "--out-prefix", "out"]) == 2
    assert existing.read_bytes() == b"KEEP_ME"
    assert not (tmp_path / "out_nogenr.skse").exists()

    # 덮어쓰기 경합: 첫 파일이 써진 뒤 두 번째 .skse 이동 직전에 대상 파일이 생기면
    # 기존 파일은 덮지 않고, 이번 실행이 만든 모든 파일(짝 없는 .ess 포함)을 지운 뒤 종료 코드 2 반환.
    real_move = cf._move_no_clobber
    race_target = tmp_path / "race_genr-corrupt.skse"

    def racing_move(tmp_file: Path, dst: Path) -> None:
        if dst == race_target:
            dst.write_bytes(b"RACE_EXISTING")
        real_move(tmp_file, dst)

    monkeypatch.setattr(cf, "_move_no_clobber", racing_move)
    rc = cf.main([
        "make", "--saves", str(tmp_path), "--source", "src_ok",
        "--out-prefix", "race", "--case", "nogenr", "--case", "genr-corrupt",
    ])
    assert rc == 2
    assert race_target.read_bytes() == b"RACE_EXISTING"
    assert not (tmp_path / "race_nogenr.ess").exists()
    assert not (tmp_path / "race_nogenr.skse").exists()
    assert not (tmp_path / "race_genr-corrupt.ess").exists()

    # 원본 GENR 0개·2개, STRT 0개·2개, 세대 0, SPOE 0개·2개 모두 거부.
    genr_c = cf.Chunk(cf.TAG_GENR, 1, cf.encode_genr(cf.GenrRecord("poe2", 1, "0.5", "0123456789abcdef", "0.1.0")))
    strt_c = cf.Chunk(cf.TAG_STRT, 1, make_strt_payload(["gem.Spark"]))
    core_c = cf.Chunk(cf.TAG_CORE, 3, b"\x01\x02")
    bad_sources = {
        "src_nogenr": make_synthetic_cosave(spoe_chunks=[core_c, strt_c]),
        "src_dupgenr": make_synthetic_cosave(spoe_chunks=[core_c, strt_c, genr_c, genr_c]),
        "src_nostrt": make_synthetic_cosave(spoe_chunks=[core_c, genr_c]),
        "src_dupstrt": make_synthetic_cosave(spoe_chunks=[core_c, strt_c, strt_c, genr_c]),
        "src_gen0": make_synthetic_cosave(spoe_chunks=[
            cf.Chunk(cf.TAG_CORE, 2, b"\x01\x02"),
            strt_c,
            cf.Chunk(cf.TAG_GENR, 1, cf.encode_genr(cf.GenrRecord("poe1", 0, "3.30", "0123456789abcdef", "0.1.0"))),
        ]),
        "src_nospoe": make_synthetic_cosave(include_spoe=False),
        "src_dupspoe": make_synthetic_cosave(duplicate_spoe=True),
    }
    for name, payload in bad_sources.items():
        write_source_pair(tmp_path, name, payload)
        assert cf.main(["make", "--saves", str(tmp_path), "--source", name, "--out-prefix", f"p_{name}"]) == 2


def test_compare_reports_chunk_count_length_first_diff_and_spoe_errors(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    base_chunks = [
        cf.Chunk(cf.TAG_CORE, 3, b"\x10\x20\x30"),
        cf.Chunk(cf.TAG_STRT, 1, make_strt_payload(["gem.Spark"])),
        cf.Chunk(cf.TAG_GENR, 1, cf.encode_genr(cf.GenrRecord("poe2", 1, "0.5", "0123456789abcdef", "0.1.0"))),
    ]
    write_source_pair(tmp_path, "in_save", make_synthetic_cosave(spoe_chunks=base_chunks, other_before=b"\x01\x02"))
    # 다른 플러그인 바이트가 달라도 SPOE 블록이 같으면 일치(0).
    write_source_pair(tmp_path, "out_same", make_synthetic_cosave(spoe_chunks=base_chunks, other_before=b"\x99\x88\x77"))
    assert cf.main(["compare", "--saves", str(tmp_path), "--input", "in_save", "--output", "out_same"]) == 0

    # 청크 수 불일치 → 1.
    write_source_pair(tmp_path, "out_count", make_synthetic_cosave(spoe_chunks=base_chunks[:2]))
    assert cf.main(["compare", "--saves", str(tmp_path), "--input", "in_save", "--output", "out_count"]) == 1
    assert "청크 수 불일치" in capsys.readouterr().out

    # 순서 다름 → 1.
    write_source_pair(tmp_path, "out_order", make_synthetic_cosave(spoe_chunks=[base_chunks[1], base_chunks[0], base_chunks[2]]))
    assert cf.main(["compare", "--saves", str(tmp_path), "--input", "in_save", "--output", "out_order"]) == 1

    # 판 다름 → 1.
    write_source_pair(tmp_path, "out_ver", make_synthetic_cosave(spoe_chunks=[cf.Chunk(cf.TAG_CORE, 2, b"\x10\x20\x30"), base_chunks[1], base_chunks[2]]))
    assert cf.main(["compare", "--saves", str(tmp_path), "--input", "in_save", "--output", "out_ver"]) == 1

    # 본문 1바이트 다름 → 1 + 첫 차이 오프셋 출력.
    write_source_pair(tmp_path, "out_byte", make_synthetic_cosave(spoe_chunks=[cf.Chunk(cf.TAG_CORE, 3, b"\x10\x21\x30"), base_chunks[1], base_chunks[2]]))
    assert cf.main(["compare", "--saves", str(tmp_path), "--input", "in_save", "--output", "out_byte"]) == 1
    assert "첫 차이 오프셋=1" in capsys.readouterr().out

    # 길이 불일치(접두 동일) → 1 + 첫 차이 오프셋(=min_len) 출력.
    write_source_pair(tmp_path, "out_len_prefix", make_synthetic_cosave(spoe_chunks=[cf.Chunk(cf.TAG_CORE, 3, b"\x10\x20\x30\x40"), base_chunks[1], base_chunks[2]]))
    assert cf.main(["compare", "--saves", str(tmp_path), "--input", "in_save", "--output", "out_len_prefix"]) == 1
    out_text = capsys.readouterr().out
    assert "length 불일치" in out_text and "첫 차이 오프셋=3" in out_text

    # 길이 불일치(앞 바이트도 다름) → 1 + 실제 첫 차이 오프셋 출력.
    write_source_pair(tmp_path, "out_len_diff", make_synthetic_cosave(spoe_chunks=[cf.Chunk(cf.TAG_CORE, 3, b"\x10\x99\x30\x40"), base_chunks[1], base_chunks[2]]))
    assert cf.main(["compare", "--saves", str(tmp_path), "--input", "in_save", "--output", "out_len_diff"]) == 1
    out_text = capsys.readouterr().out
    assert "length 불일치" in out_text and "첫 차이 오프셋=1" in out_text

    # SPOE 없음·두 개 → 종료 코드 2.
    write_source_pair(tmp_path, "out_nospoe", make_synthetic_cosave(include_spoe=False))
    assert cf.main(["compare", "--saves", str(tmp_path), "--input", "in_save", "--output", "out_nospoe"]) == 2
    write_source_pair(tmp_path, "out_dupspoe", make_synthetic_cosave(duplicate_spoe=True))
    assert cf.main(["compare", "--saves", str(tmp_path), "--input", "in_save", "--output", "out_dupspoe"]) == 2

    # inspect 동작 확인.
    assert cf.main(["inspect", "--saves", str(tmp_path), "--stem", "in_save"]) == 0
