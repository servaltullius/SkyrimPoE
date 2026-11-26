"""SKSE 코세이브(.skse) SPOE 보존 음성 시험 픽스처 생성·비교·검사 도구 (결정 256).

SKSE 코세이브 이진 형식(모두 리틀 엔디언 u32):
  - 파일 머리(20바이트): signature(0x45534B53 'SKSE'), formatVersion, skseVersion, runtimeVersion, numPlugins
  - 플러그인마다(12바이트 + length): uid, numChunks, length(뒤따르는 청크 머리+본문 합)
  - 청크마다(12바이트 + length): type, version, length, payload(length 바이트)

SkyrimPoE 플러그인 UID는 C++ 다중 문자 상수 'SPOE'(0x53504F45)이며,
청크 type도 'CORE'(0x434F5245), 'GENR'(0x47454E52), 'STRT'(0x53545254) 등의 값을 리틀 엔디언 u32로 저장한다.

손상 케이스 상세:
  - nogenr:       GENR 청크를 제거한다(CORE v3는 유지 → DecideSave가 Preserve 판정).
  - genr-corrupt: GENR 본문 길이는 그대로 두고 끝 4바이트 CRC32의 마지막 바이트를 XOR 0xFF로 뒤집는다.
                  ReadGenerationRecord(SaveGeneration.cpp:89)의 CRC32 대조가 즉시 실패해 Damaged를 반환한다.
  - genr-trunc:   GENR 본문 끝에서 2바이트를 잘라(길이가 2 이하이면 1바이트만 남김) chunk/plugin length를 줄인다.
                  ReadGenerationRecord가 크기·CRC 검증에 실패해 Damaged를 반환한다.
  - genr-highgen: GENR 구조와 문자열은 유지하고 contentGeneration만 1 → 2로 바꾼 뒤 CRC32를 다시 계산한다.
                  DecideSaveGeneration이 contentGeneration > packGeneration으로 Preserve를 판정한다.
  - strt-missing: STRT 청크를 제거한다.
  - strt-dup:     기존 STRT 청크와 동일한 청크를 바로 뒤에 한 개 더 넣는다.
  - strt-corrupt: STRT 본문 길이는 그대로 두고 맨 앞 u32(ID 개수)를 257(kMaxStarterGrantedIds=256 초과)로 덮어쓴다.
                  ReadStarterRecord(SaveRecordFormat.cpp:439)가 상한 초과로 즉시 거부한다.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import os
from pathlib import Path
import shutil
import struct
import sys
import tempfile
import zlib

SKSE_SIGNATURE = 0x45534B53  # 'SKSE' little-endian u32


def fourcc(tag: str) -> int:
    if len(tag) != 4 or not tag.isascii():
        raise ValueError(f"4바이트 ASCII 태그가 필요합니다: {tag!r}")
    return (ord(tag[0]) << 24) | (ord(tag[1]) << 16) | (ord(tag[2]) << 8) | ord(tag[3])


def format_fourcc(value: int) -> str:
    chars = bytes([(value >> 24) & 0xFF, (value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF])
    if all(32 <= b < 127 for b in chars):
        return chars.decode("ascii")
    return f"0x{value:08X}"


SPOE_UID = fourcc("SPOE")
TAG_CORE = fourcc("CORE")
TAG_GENR = fourcc("GENR")
TAG_STRT = fourcc("STRT")
GENR_VERSION = 1
STRT_VERSION = 1
CORE_GEN1_VERSION = 3
MAX_STARTER_IDS = 256
MAX_STARTER_ID_BYTES = 64
MAX_SAVE_RECORD_BYTES = 16 * 1024 * 1024
WINDOWS_FORBIDDEN_CHARS = set('<>:"/\\|?*')

ALL_CASES = (
    "nogenr",
    "genr-corrupt",
    "genr-trunc",
    "genr-highgen",
    "strt-missing",
    "strt-dup",
    "strt-corrupt",
)


@dataclass
class Chunk:
    chunk_type: int
    version: int
    payload: bytes

    def encode(self) -> bytes:
        return struct.pack("<III", self.chunk_type, self.version, len(self.payload)) + self.payload


@dataclass
class PluginBlock:
    uid: int
    num_chunks: int
    raw_body: bytes
    chunks: list[Chunk] | None = None

    def encode(self) -> bytes:
        if self.chunks is not None:
            body = b"".join(c.encode() for c in self.chunks)
            return struct.pack("<III", self.uid, len(self.chunks), len(body)) + body
        return struct.pack("<III", self.uid, self.num_chunks, len(self.raw_body)) + self.raw_body


@dataclass
class CosaveFile:
    format_version: int
    skse_version: int
    runtime_version: int
    plugins: list[PluginBlock]

    def encode(self) -> bytes:
        header = struct.pack(
            "<IIIII",
            SKSE_SIGNATURE,
            self.format_version,
            self.skse_version,
            self.runtime_version,
            len(self.plugins),
        )
        return header + b"".join(p.encode() for p in self.plugins)


@dataclass
class GenrRecord:
    game: str
    content_generation: int
    game_version: str
    build_id: str
    dll_version: str


def _read_u32(buf: bytes, offset: int) -> tuple[int, int]:
    if offset + 4 > len(buf):
        raise ValueError("예상보다 빨리 데이터가 끝났습니다(u32).")
    return struct.unpack_from("<I", buf, offset)[0], offset + 4


def _read_str(buf: bytes, offset: int) -> tuple[str, int]:
    length, offset = _read_u32(buf, offset)
    if offset + length > len(buf):
        raise ValueError("문자열 길이가 본문을 넘었습니다.")
    raw = buf[offset : offset + length]
    return raw.decode("utf-8"), offset + length


def _write_str(value: str) -> bytes:
    raw = value.encode("utf-8")
    return struct.pack("<I", len(raw)) + raw


def parse_chunks(body: bytes, expected_count: int) -> list[Chunk]:
    chunks: list[Chunk] = []
    pos = 0
    for _ in range(expected_count):
        if pos + 12 > len(body):
            raise ValueError("청크 머리가 본문 범위를 벗어났습니다.")
        ctype, version, length = struct.unpack_from("<III", body, pos)
        pos += 12
        if pos + length > len(body):
            raise ValueError(f"청크 {format_fourcc(ctype)} 본문이 범위를 벗어났습니다.")
        payload = body[pos : pos + length]
        pos += length
        chunks.append(Chunk(ctype, version, payload))
    if pos != len(body):
        raise ValueError(f"SPOE 플러그인 본문 뒤에 알 수 없는 바이트 {len(body) - pos}바이트가 남았습니다.")
    return chunks


def parse_cosave(data: bytes) -> CosaveFile:
    if len(data) < 20:
        raise ValueError("SKSE 코세이브 파일이 20바이트보다 짧습니다.")
    sig, fmt_ver, skse_ver, rt_ver, num_plugins = struct.unpack_from("<IIIII", data, 0)
    if sig != SKSE_SIGNATURE:
        raise ValueError(f"SKSE 서명이 아닙니다: 0x{sig:08X}")
    pos = 20
    plugins: list[PluginBlock] = []
    for _ in range(num_plugins):
        if pos + 12 > len(data):
            raise ValueError("플러그인 머리가 파일 범위를 벗어났습니다.")
        uid, num_chunks, length = struct.unpack_from("<III", data, pos)
        pos += 12
        if pos + length > len(data):
            raise ValueError(f"플러그인 0x{uid:08X} 본문이 파일 범위를 벗어났습니다.")
        body = data[pos : pos + length]
        pos += length
        chunks = parse_chunks(body, num_chunks) if uid == SPOE_UID else None
        plugins.append(PluginBlock(uid=uid, num_chunks=num_chunks, raw_body=body, chunks=chunks))
    if pos != len(data):
        raise ValueError(f"코세이브 끝에 해석되지 않은 {len(data) - pos}바이트가 남았습니다.")
    return CosaveFile(fmt_ver, skse_ver, rt_ver, plugins)


def decode_genr(chunk: Chunk) -> GenrRecord:
    if chunk.chunk_type != TAG_GENR or chunk.version != GENR_VERSION or len(chunk.payload) < 4:
        raise ValueError("GENR 청크 판 또는 길이가 올바르지 않습니다.")
    body = chunk.payload[:-4]
    expected_crc = struct.unpack_from("<I", chunk.payload, len(chunk.payload) - 4)[0]
    actual_crc = zlib.crc32(body) & 0xFFFFFFFF
    if actual_crc != expected_crc:
        raise ValueError(f"GENR CRC32 불일치 (기대 0x{expected_crc:08X}, 실제 0x{actual_crc:08X}).")
    pos = 0
    game, pos = _read_str(body, pos)
    content_gen, pos = _read_u32(body, pos)
    game_ver, pos = _read_str(body, pos)
    build_id, pos = _read_str(body, pos)
    dll_ver, pos = _read_str(body, pos)
    if pos != len(body) or not game_ver or not build_id or not dll_ver:
        raise ValueError("GENR 본문 구문 또는 필수 필드가 올바르지 않습니다.")
    return GenrRecord(game, content_gen, game_ver, build_id, dll_ver)


def encode_genr(rec: GenrRecord) -> bytes:
    body = (
        _write_str(rec.game)
        + struct.pack("<I", rec.content_generation)
        + _write_str(rec.game_version)
        + _write_str(rec.build_id)
        + _write_str(rec.dll_version)
    )
    crc = zlib.crc32(body) & 0xFFFFFFFF
    return body + struct.pack("<I", crc)


def is_valid_starter_content_id(item: str) -> bool:
    """C++ IsValidStarterContentId(SaveRecordFormat.cpp:405-429)와 동일한 규칙."""
    if not item or not item.isascii():
        return False
    if len(item) > MAX_STARTER_ID_BYTES:
        return False
    dot = item.find(".")
    if dot <= 0 or dot + 1 >= len(item):
        return False
    if not all("a" <= ch <= "z" for ch in item[:dot]):
        return False
    for ch in item[dot + 1 :]:
        if not (("a" <= ch <= "z") or ("A" <= ch <= "Z") or ("0" <= ch <= "9") or ch in (".", "_")):
            return False
    return True


def validate_strt_chunk(chunk: Chunk) -> list[str]:
    if (
        chunk.chunk_type != TAG_STRT
        or chunk.version != STRT_VERSION
        or len(chunk.payload) < 4
        or len(chunk.payload) > MAX_SAVE_RECORD_BYTES
    ):
        raise ValueError("STRT 청크 판 또는 길이가 올바르지 않습니다.")
    count, pos = _read_u32(chunk.payload, 0)
    if count > MAX_STARTER_IDS:
        raise ValueError(f"STRT ID 개수가 상한({MAX_STARTER_IDS})을 초과했습니다: {count}")
    ids: list[str] = []
    seen: set[str] = set()
    for _ in range(count):
        item, pos = _read_str(chunk.payload, pos)
        if not is_valid_starter_content_id(item) or item in seen:
            raise ValueError(f"STRT ID 형식이 올바르지 않거나 중복입니다: {item!r}")
        seen.add(item)
        ids.append(item)
    if pos != len(chunk.payload):
        raise ValueError("STRT 본문 뒤에 여분 바이트가 있습니다.")
    return ids


def find_single_spoe(cosave: CosaveFile, label: str) -> tuple[int, PluginBlock]:
    matches = [(idx, p) for idx, p in enumerate(cosave.plugins) if p.uid == SPOE_UID]
    if len(matches) != 1:
        raise ValueError(f"{label}: SPOE 플러그인 블록이 정확히 1개여야 합니다 (관측={len(matches)}).")
    idx, block = matches[0]
    if block.chunks is None:
        block.chunks = parse_chunks(block.raw_body, block.num_chunks)
    return idx, block


def validate_stem(name: str, param: str) -> str:
    cleaned = name.strip()
    if not cleaned:
        raise ValueError(f"{param} 값이 비어 있습니다.")
    if ".." in cleaned or any(ord(ch) < 32 or ch in WINDOWS_FORBIDDEN_CHARS for ch in cleaned):
        raise ValueError(f"{param}에 허용되지 않은 경로/파일명 문자가 포함되어 있습니다: {name!r}")
    if cleaned.lower().endswith(".ess") or cleaned.lower().endswith(".skse"):
        cleaned = cleaned.rsplit(".", 1)[0]
    if not cleaned:
        raise ValueError(f"{param} 스템이 비어 있습니다: {name!r}")
    return cleaned


def mutate_spoe_chunks(chunks: list[Chunk], case: str) -> list[Chunk]:
    out = [Chunk(c.chunk_type, c.version, bytes(c.payload)) for c in chunks]
    genr_indices = [i for i, c in enumerate(out) if c.chunk_type == TAG_GENR]
    strt_indices = [i for i, c in enumerate(out) if c.chunk_type == TAG_STRT]
    if len(genr_indices) != 1 or len(strt_indices) != 1:
        raise ValueError(f"원본 SPOE 블록에 GENR({len(genr_indices)})·STRT({len(strt_indices)})가 각 1개여야 합니다.")
    gi = genr_indices[0]
    si = strt_indices[0]

    if case == "nogenr":
        del out[gi]
    elif case == "genr-corrupt":
        # 길이 유지 + CRC32 마지막 바이트 반전 → ReadGenerationRecord가 CRC 불일치로 Damaged 판정.
        payload = bytearray(out[gi].payload)
        payload[-1] ^= 0xFF
        out[gi].payload = bytes(payload)
    elif case == "genr-trunc":
        # 본문을 잘라 chunk/plugin 길이를 줄임 → ReadGenerationRecord가 Damaged 판정.
        old = out[gi].payload
        new_len = max(1, len(old) - 2) if len(old) > 1 else 0
        out[gi].payload = old[:new_len]
    elif case == "genr-highgen":
        rec = decode_genr(out[gi])
        rec.content_generation = 2
        out[gi].payload = encode_genr(rec)
    elif case == "strt-missing":
        del out[si]
    elif case == "strt-dup":
        dup = Chunk(out[si].chunk_type, out[si].version, bytes(out[si].payload))
        out.insert(si + 1, dup)
    elif case == "strt-corrupt":
        # 길이 유지 + 맨 앞 u32(ID 개수)를 257(kMaxStarterGrantedIds=256 초과)로 덮어써 ReadStarterRecord가 거부.
        payload = bytearray(out[si].payload)
        struct.pack_into("<I", payload, 0, MAX_STARTER_IDS + 1)
        out[si].payload = bytes(payload)
    else:
        raise ValueError(f"지원하지 않는 케이스입니다: {case}")
    return out


def _move_no_clobber(tmp_path: Path, dst: Path) -> None:
    """대상이 이미 있으면 덮어쓰지 않고 FileExistsError로 실패한다(Windows os.rename 원자적 미덮어쓰기)."""
    if os.name == "nt":
        os.rename(tmp_path, dst)
    else:
        os.link(tmp_path, dst)
        tmp_path.unlink(missing_ok=True)


def atomic_write_bytes(path: Path, data: bytes) -> None:
    fd, tmp_name = tempfile.mkstemp(prefix=".cosave_tmp_", dir=str(path.parent))
    tmp_path = Path(tmp_name)
    try:
        with os.fdopen(fd, "wb") as handle:
            handle.write(data)
            handle.flush()
            os.fsync(handle.fileno())
        _move_no_clobber(tmp_path, path)
    except BaseException:
        if tmp_path.exists():
            tmp_path.unlink(missing_ok=True)
        raise


def atomic_copy_file(src: Path, dst: Path) -> None:
    fd, tmp_name = tempfile.mkstemp(prefix=".cosave_tmp_", dir=str(dst.parent))
    tmp_path = Path(tmp_name)
    try:
        with os.fdopen(fd, "wb") as dst_f, src.open("rb") as src_f:
            shutil.copyfileobj(src_f, dst_f)
            dst_f.flush()
            os.fsync(dst_f.fileno())
        _move_no_clobber(tmp_path, dst)
    except BaseException:
        if tmp_path.exists():
            tmp_path.unlink(missing_ok=True)
        raise


def cmd_make(saves_dir: Path, source: str, out_prefix: str, cases: list[str]) -> int:
    src_stem = validate_stem(source, "--source")
    prefix = validate_stem(out_prefix, "--out-prefix")
    selected = list(dict.fromkeys(cases)) if cases else list(ALL_CASES)
    for c in selected:
        if c not in ALL_CASES:
            raise ValueError(f"알 수 없는 케이스입니다: {c} (허용: {', '.join(ALL_CASES)})")

    src_ess = saves_dir / f"{src_stem}.ess"
    src_skse = saves_dir / f"{src_stem}.skse"
    if not src_ess.is_file() or not src_skse.is_file():
        raise FileNotFoundError(f"원본 세이브 쌍이 없습니다: {src_ess}, {src_skse}")

    cosave = parse_cosave(src_skse.read_bytes())
    spoe_idx, spoe = find_single_spoe(cosave, str(src_skse))
    assert spoe.chunks is not None

    genr_chunks = [c for c in spoe.chunks if c.chunk_type == TAG_GENR]
    strt_chunks = [c for c in spoe.chunks if c.chunk_type == TAG_STRT]
    if len(genr_chunks) != 1:
        raise ValueError(f"원본 세이브에 GENR 청크가 정확히 1개여야 합니다 (관측={len(genr_chunks)}).")
    genr = decode_genr(genr_chunks[0])
    if genr.game != "poe2" or genr.content_generation != 1:
        raise ValueError(f"원본 세이브의 GENR은 poe2/1이어야 합니다 (관측={genr.game}/{genr.content_generation}).")
    if len(strt_chunks) != 1:
        raise ValueError(f"원본 세이브에 STRT 청크가 정확히 1개여야 합니다 (관측={len(strt_chunks)}).")
    validate_strt_chunk(strt_chunks[0])

    # 출력 충돌 및 덮어쓰기 사전 검사(하나라도 걸리면 아무 파일도 쓰지 않는다).
    targets: list[tuple[str, Path, Path]] = []
    for case in selected:
        out_stem = validate_stem(f"{prefix}_{case}", "출력 스템")
        if out_stem.lower() == src_stem.lower():
            raise ValueError(f"출력 스템이 원본과 같습니다: {out_stem}")
        out_ess = saves_dir / f"{out_stem}.ess"
        out_skse = saves_dir / f"{out_stem}.skse"
        if out_ess.exists() or out_skse.exists():
            raise FileExistsError(f"이미 존재하는 출력 파일은 덮어쓰지 않습니다: {out_stem}")
        targets.append((case, out_ess, out_skse))

    created: list[Path] = []
    try:
        for case, out_ess, out_skse in targets:
            mutated_plugins: list[PluginBlock] = []
            for idx, plugin in enumerate(cosave.plugins):
                if idx == spoe_idx:
                    new_chunks = mutate_spoe_chunks(spoe.chunks, case)
                    mutated_plugins.append(
                        PluginBlock(uid=SPOE_UID, num_chunks=len(new_chunks), raw_body=b"", chunks=new_chunks)
                    )
                else:
                    mutated_plugins.append(
                        PluginBlock(uid=plugin.uid, num_chunks=plugin.num_chunks, raw_body=plugin.raw_body)
                    )
            mutated_file = CosaveFile(
                cosave.format_version, cosave.skse_version, cosave.runtime_version, mutated_plugins
            )
            atomic_copy_file(src_ess, out_ess)
            created.append(out_ess)
            atomic_write_bytes(out_skse, mutated_file.encode())
            created.append(out_skse)
            print(f"CREATED {out_ess.stem} ({case})")
    except BaseException:
        # 중간에 실패하면 이번 실행이 만든 파일(짝 없는 .ess 포함)을 모두 지우고 되돌린다.
        for path in reversed(created):
            path.unlink(missing_ok=True)
        raise
    return 0


def compare_spoe_blocks(in_cosave: CosaveFile, out_cosave: CosaveFile) -> tuple[bool, str]:
    _, in_spoe = find_single_spoe(in_cosave, "input")
    _, out_spoe = find_single_spoe(out_cosave, "output")
    assert in_spoe.chunks is not None and out_spoe.chunks is not None
    if len(in_spoe.chunks) != len(out_spoe.chunks):
        return False, f"SPOE 청크 수 불일치: input={len(in_spoe.chunks)}, output={len(out_spoe.chunks)}"
    for i, (a, b) in enumerate(zip(in_spoe.chunks, out_spoe.chunks)):
        if a.chunk_type != b.chunk_type:
            return False, (
                f"청크 #{i} type 불일치: input={format_fourcc(a.chunk_type)}, "
                f"output={format_fourcc(b.chunk_type)}"
            )
        if a.version != b.version:
            return False, (
                f"청크 #{i} ({format_fourcc(a.chunk_type)}) version 불일치: "
                f"input=v{a.version}, output=v{b.version}"
            )
        if len(a.payload) != len(b.payload):
            min_len = min(len(a.payload), len(b.payload))
            diff_pos = next((idx for idx in range(min_len) if a.payload[idx] != b.payload[idx]), min_len)
            in_byte = f"0x{a.payload[diff_pos]:02X}" if diff_pos < len(a.payload) else "EOF"
            out_byte = f"0x{b.payload[diff_pos]:02X}" if diff_pos < len(b.payload) else "EOF"
            return False, (
                f"청크 #{i} ({format_fourcc(a.chunk_type)}) length 불일치: "
                f"input={len(a.payload)}, output={len(b.payload)}, "
                f"첫 차이 오프셋={diff_pos} (input={in_byte}, output={out_byte})"
            )
        if a.payload != b.payload:
            diff_pos = next(idx for idx, (x, y) in enumerate(zip(a.payload, b.payload)) if x != y)
            return False, (
                f"청크 #{i} ({format_fourcc(a.chunk_type)}) 본문 불일치: "
                f"첫 차이 오프셋={diff_pos} (input=0x{a.payload[diff_pos]:02X}, output=0x{b.payload[diff_pos]:02X})"
            )
    return True, f"SPOE 청크 {len(in_spoe.chunks)}개 완전 일치"


def cmd_compare(saves_dir: Path, input_stem: str, output_stem: str) -> int:
    in_name = validate_stem(input_stem, "--input")
    out_name = validate_stem(output_stem, "--output")
    in_path = saves_dir / f"{in_name}.skse"
    out_path = saves_dir / f"{out_name}.skse"
    in_cosave = parse_cosave(in_path.read_bytes())
    out_cosave = parse_cosave(out_path.read_bytes())
    same, detail = compare_spoe_blocks(in_cosave, out_cosave)
    print(f"{'MATCH' if same else 'MISMATCH'}: {in_name} vs {out_name} — {detail}")
    return 0 if same else 1


def cmd_inspect(saves_dir: Path, stem: str) -> int:
    name = validate_stem(stem, "--stem")
    skse_path = saves_dir / f"{name}.skse"
    cosave = parse_cosave(skse_path.read_bytes())
    _, spoe = find_single_spoe(cosave, str(skse_path))
    assert spoe.chunks is not None
    print(f"SKSE {name}.skse: format={cosave.format_version}, plugins={len(cosave.plugins)}, SPOE chunks={len(spoe.chunks)}")
    for idx, chunk in enumerate(spoe.chunks):
        extra = ""
        if chunk.chunk_type == TAG_GENR:
            try:
                rec = decode_genr(chunk)
                extra = f" [valid {rec.game}/gen{rec.content_generation} {rec.game_version} build={rec.build_id}]"
            except ValueError as exc:
                extra = f" [invalid: {exc}]"
        elif chunk.chunk_type == TAG_STRT:
            try:
                ids = validate_strt_chunk(chunk)
                extra = f" [valid ids={ids}]"
            except ValueError as exc:
                extra = f" [invalid: {exc}]"
        print(f"  #{idx}: {format_fourcc(chunk.chunk_type)}(0x{chunk.chunk_type:08X}) v{chunk.version} len={len(chunk.payload)}{extra}")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="SKSE 코세이브 SPOE 보존 음성 시험 픽스처 도구")
    sub = parser.add_subparsers(dest="command", required=True)

    p_make = sub.add_parser("make", help="정상 세대 1 세이브에서 보존 음성 손상 픽스처 쌍(.ess/.skse)을 생성")
    p_make.add_argument("--saves", required=True, type=Path)
    p_make.add_argument("--source", required=True)
    p_make.add_argument("--out-prefix", required=True)
    p_make.add_argument("--case", action="append", default=[])

    p_cmp = sub.add_parser("compare", help="두 .skse의 SPOE 청크 순서·종류·판·본문 완전 일치 비교")
    p_cmp.add_argument("--saves", required=True, type=Path)
    p_cmp.add_argument("--input", required=True)
    p_cmp.add_argument("--output", required=True)

    p_ins = sub.add_parser("inspect", help=".skse의 SPOE 청크 목록 출력")
    p_ins.add_argument("--saves", required=True, type=Path)
    p_ins.add_argument("--stem", required=True)
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        if args.command == "make":
            return cmd_make(args.saves, args.source, args.out_prefix, args.case)
        if args.command == "compare":
            return cmd_compare(args.saves, args.input, args.output)
        if args.command == "inspect":
            return cmd_inspect(args.saves, args.stem)
        parser.error(f"알 수 없는 명령: {args.command}")
        return 2
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
