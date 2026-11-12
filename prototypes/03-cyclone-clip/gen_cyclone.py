"""사이클론 회전 모션(반복 클립)을 숫자로 만들어 hkxc XML로 쓴다.

구성:
  1. 바닐라 양손 대기 자세의 첫 프레임을 바탕으로 삼는다(손가락이 손잡이를 쥔 모양과 뼈 길이를 얻는다).
  2. 축발(왼발)을 몸 바로 아래에 두고 무릎을 굽혀 자세를 낮춘다. 반대쪽 다리는 뒤로 뻗어 땅에서 띄운다.
  3. 가슴을 무기 쪽으로 틀고, 두 팔을 가슴 앞으로 뻗어 무기를 수평으로 든다. 무기는 회전 방향의 뒤쪽으로 끌려온다.
     오른손이 무기의 방향을 정하고, 왼손은 바탕 자세에서 쥐고 있던 손잡이 자리를 그대로 따라간다.
  4. 상체를 무기 반대쪽으로 기울여 무게를 받치고, 머리는 몸이 나아가는 쪽을 보게 한다.
  5. 축발이 캐릭터의 세로축 위에 오도록 옮긴 뒤, 몸통 중심(COM) 뼈에서 통째로 한 바퀴 돌린다.
     축발은 제자리에서 돌기만 하고, 뒤로 뻗은 발은 떠 있으므로 발이 땅을 미끄러지지 않는다.
  6. 한 바퀴 동안 골반이 두 번 가볍게 오르내리게 한다.

OAR의 이동 클립 자리에 재생할 것이므로 제자리 클립이다(앞으로 나아가는 이동이 없다).
실제 이동은 게임의 평소 이동이 맡는다.

사용:
  python gen_cyclone.py --skeleton skeleton.xml --base 2hm_idle.xml --out out/cyclone_loop.xml
"""
from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass
from pathlib import Path

import numpy as np

from hkxanim import (Animation, Skeleton, fit_to_skeleton, load_spline, qaxis, qbetween, qconj, qmul, qnorm, qrot,
                     world_pose)

FPS = 30.0
UP = np.array([0.0, 0.0, 1.0])


@dataclass(frozen=True)
class Design:
    """모션의 생김새를 정하는 값. 길이는 스카이림 단위, 각도는 도."""

    frames_per_turn: int = 18          # 한 바퀴의 프레임 수. 30fps에서 18프레임 = 0.6초
    turns_ccw: bool = True             # 위에서 볼 때 반시계 방향(오른손 무기가 오른쪽에서 왼쪽으로 지나감)
    crouch: float = 5.0                # 골반을 낮추는 양
    bob: float = 1.0                   # 한 바퀴에 두 번 오르내리는 폭
    pivot_forward: float = 3.0         # 축발이 골반보다 앞에 놓이는 거리
    trail_back: float = 34.0           # 뒤로 뻗은 발이 골반보다 뒤에 놓이는 거리
    trail_side: float = 10.0           # 그 발이 오른쪽으로 벌어진 거리
    trail_lift: float = 16.0           # 그 발이 땅에서 뜬 높이
    trail_toe_down: float = 35.0       # 그 발의 발끝을 아래로 내린 각도
    chest_twist: float = 30.0          # 가슴을 오른쪽(무기 쪽)으로 튼 각도
    lean_away: float = 12.0            # 상체를 무기 반대쪽으로 기울인 각도
    weapon_angle: float = 35.0         # 무기가 가슴 정면에서 오른쪽으로 벌어진 각도(회전 방향의 뒤쪽)
    hand_forward: float = 28.0         # 오른손이 가슴 중앙에서 앞으로 나간 거리
    hand_side: float = 3.0             # 오른쪽(+)으로 나간 거리
    hand_drop: float = 8.0             # 어깨보다 낮은 정도


def fail(message: str) -> None:
    print(f"오류: {message}", file=sys.stderr)
    raise SystemExit(1)


def unit(v) -> np.ndarray:
    v = np.asarray(v, float)
    n = np.linalg.norm(v)
    if n < 1e-9:
        raise ValueError("길이가 0인 벡터를 정규화하려 함")
    return v / n


def quat_from_axes(x, y, z) -> np.ndarray:
    """세 축(열 벡터)으로 이뤄진 회전 행렬을 쿼터니언 (x, y, z, w)로."""
    m = np.stack([x, y, z], axis=1)
    tr = m[0, 0] + m[1, 1] + m[2, 2]
    if tr > 0:
        s = np.sqrt(tr + 1.0) * 2
        return qnorm(np.array([(m[2, 1] - m[1, 2]) / s, (m[0, 2] - m[2, 0]) / s, (m[1, 0] - m[0, 1]) / s, 0.25 * s]))
    i = int(np.argmax([m[0, 0], m[1, 1], m[2, 2]]))
    j, k = (i + 1) % 3, (i + 2) % 3
    s = np.sqrt(1.0 + m[i, i] - m[j, j] - m[k, k]) * 2
    q = np.zeros(4)
    q[i] = 0.25 * s
    q[j] = (m[j, i] + m[i, j]) / s
    q[k] = (m[k, i] + m[i, k]) / s
    q[3] = (m[k, j] - m[j, k]) / s
    return qnorm(q)


class Rig:
    """한 프레임의 로컬 자세(lt, lq)를 월드 기준으로 다루는 도구."""

    def __init__(self, sk: Skeleton, lt: np.ndarray, lq: np.ndarray):
        self.sk = sk
        self.lt = lt.copy()
        self.lq = lq.copy()

    def world(self):
        return world_pose(self.sk.parents, self.lt, self.lq)

    def rotate_world(self, bone: int, rotation: np.ndarray) -> None:
        """뼈(와 그 자식 전체)를 월드 기준 회전만큼 돌린다."""
        _, wq = self.world()
        parent = self.sk.parents[bone]
        pq = wq[parent] if parent >= 0 else np.array([0.0, 0.0, 0.0, 1.0])
        self.lq[bone] = qnorm(qmul(qmul(qconj(pq), qmul(rotation, pq)), self.lq[bone]))

    def set_world_rotation(self, bone: int, target: np.ndarray) -> None:
        _, wq = self.world()
        parent = self.sk.parents[bone]
        pq = wq[parent] if parent >= 0 else np.array([0.0, 0.0, 0.0, 1.0])
        self.lq[bone] = qnorm(qmul(qconj(pq), target))

    def two_bone_ik(self, upper: int, lower: int, end: int, target: np.ndarray) -> float:
        """상완-전완-손(또는 허벅지-종아리-발)을 목표에 닿게 한다. 굽는 방향은 지금 굽은 쪽을 따른다.
        돌려주는 값은 목표까지의 거리를 팔다리 길이로 나눈 비율(1에 가까우면 거의 다 편 것)."""
        wp, _ = self.world()
        s, e0, h0 = wp[upper], wp[lower], wp[end]
        a, b = np.linalg.norm(e0 - s), np.linalg.norm(h0 - e0)
        d = target - s
        reach = np.linalg.norm(d) / (a + b)
        dist = min(np.linalg.norm(d), (a + b) * 0.999)
        dn = unit(d)
        cos_a = np.clip((a * a + dist * dist - b * b) / (2 * a * dist), -1.0, 1.0)
        # 지금 팔꿈치(무릎)가 나와 있는 방향을 유지한다.
        along = unit(h0 - s)
        bend = (e0 - s) - np.dot(e0 - s, along) * along
        if np.linalg.norm(bend) < 0.3:
            bend = np.array([0.0, 1.0, 0.0])
        pole = bend - np.dot(bend, dn) * dn
        pole = unit(pole)
        elbow = s + dn * (a * cos_a) + pole * (a * np.sqrt(max(0.0, 1.0 - cos_a * cos_a)))
        self.rotate_world(upper, qbetween(e0 - s, elbow - s))
        wp, _ = self.world()
        self.rotate_world(lower, qbetween(wp[end] - wp[lower], (s + dn * dist) - wp[lower]))
        return float(reach)


def build_pose(sk: Skeleton, base_t: np.ndarray, base_q: np.ndarray, design: Design, bob: float):
    """한 바퀴 돌리기 전의 자세 하나를 만든다. bob은 이 프레임의 골반 높이 변화량."""
    rig = Rig(sk, base_t, base_q)
    com = sk.idx("COM")
    weapon = sk.index["Weapon"]
    r_arm = (sk.idx("R UpperArm"), sk.idx("R Forearm"), sk.idx("R Hand"))
    l_arm = (sk.idx("L UpperArm"), sk.idx("L Forearm"), sk.idx("L Hand"))
    legs = [(sk.idx(f"{s} Thigh"), sk.idx(f"{s} Calf"), sk.idx(f"{s} Foot")) for s in "LR"]

    base_wp, base_wq = rig.world()
    # 바탕 자세에서 왼손이 무기를 어떻게 쥐고 있는지(무기 기준의 위치와 회전)를 기억해 둔다.
    grip_pos = qrot(qconj(base_wq[weapon]), base_wp[l_arm[2]] - base_wp[weapon])
    grip_rot = qmul(qconj(base_wq[weapon]), base_wq[l_arm[2]])

    # 1) 골반이 향하는 방향(수평)을 기준으로 삼는다.
    pelvis_forward = qrot(base_wq[sk.idx("Pelvis")], [0.0, 1.0, 0.0])
    pelvis_forward[2] = 0.0
    forward = unit(pelvis_forward)
    right = np.cross(forward, UP)

    # 2) 자세를 낮추고, 축발(왼발)은 몸 바로 아래에, 반대쪽 발은 뒤로 뻗어 띄운다.
    rig.lt[com][2] += -design.crouch + bob
    wp, _ = rig.world()
    hips = wp[com].copy()
    (l_thigh, l_calf, l_foot), (r_thigh, r_calf, r_foot) = legs
    ground = float(base_wp[l_foot][2])
    pivot_target = np.array([hips[0], hips[1], ground]) + forward * design.pivot_forward
    trail_target = (np.array([hips[0], hips[1], ground + design.trail_lift])
                    - forward * design.trail_back + right * design.trail_side)
    reaches = {}
    reaches["축다리"] = rig.two_bone_ik(l_thigh, l_calf, l_foot, pivot_target)
    rig.set_world_rotation(l_foot, base_wq[l_foot])
    reaches["뒷다리"] = rig.two_bone_ik(r_thigh, r_calf, r_foot, trail_target)
    # 오른쪽 축을 기준으로 음의 회전이 발끝을 아래로 내린다.
    toe_down = qaxis(right, -np.radians(design.trail_toe_down))
    rig.set_world_rotation(r_foot, qnorm(qmul(toe_down, base_wq[r_foot])))

    # 3) 가슴을 오른쪽(무기 쪽)으로 튼다. 위에서 볼 때 시계 방향이 오른쪽이다.
    twist = np.radians(design.chest_twist)
    for bone in (sk.idx("Spine1"), sk.idx("Spine2")):
        rig.rotate_world(bone, qaxis(UP, -twist / 2.0))
    chest_forward = forward * np.cos(twist) + right * np.sin(twist)
    chest_right = right * np.cos(twist) - forward * np.sin(twist)
    angle = np.radians(design.weapon_angle)
    blade = unit(chest_forward * np.cos(angle) + chest_right * np.sin(angle))

    # 4) 상체를 무기 반대쪽으로 기울인다.
    rig.rotate_world(sk.idx("Spine"), qaxis(np.cross(blade, UP), np.radians(design.lean_away)))

    # 5) 머리는 가슴의 틀림을 되돌려 몸이 나아가는 쪽을 보게 한다.
    for bone in (sk.idx("Neck"), sk.idx("Head")):
        rig.rotate_world(bone, qaxis(UP, twist / 2.0))

    # 6) 두 팔을 가슴 앞으로 뻗어 무기를 수평으로 든다. 손의 위치는 기울인 뒤의 어깨에서 잰다.
    wp, _ = rig.world()
    chest = (wp[r_arm[0]] + wp[l_arm[0]]) / 2.0
    hand_target = (chest + chest_forward * design.hand_forward + chest_right * design.hand_side
                   - UP * design.hand_drop)
    reaches["오른팔"] = rig.two_bone_ik(*r_arm, hand_target)
    wp, wq = rig.world()
    # 무기 뼈: 로컬 +Y가 칼날 방향, +Z가 넓적면. 수평으로 베려면 넓적면이 위나 아래를 봐야 한다.
    # 위·아래 두 가지 중 손목이 덜 꺾이는 쪽을 고른다.
    candidates = []
    for flat in (UP, -UP):
        target_q = quat_from_axes(np.cross(blade, flat), blade, flat)
        turn = qmul(target_q, qconj(wq[weapon]))
        turn_angle = 2.0 * np.arccos(np.clip(abs(turn[3]), 0.0, 1.0))
        candidates.append((turn_angle, turn))
    wrist_twist, turn = min(candidates, key=lambda c: c[0])
    rig.rotate_world(r_arm[2], turn)

    # 7) 왼손은 바탕 자세에서 쥐고 있던 손잡이 자리로 보낸다.
    wp, wq = rig.world()
    grip_target = wp[weapon] + qrot(wq[weapon], grip_pos)
    reaches["왼팔"] = rig.two_bone_ik(*l_arm, grip_target)
    rig.set_world_rotation(l_arm[2], qnorm(qmul(wq[weapon], grip_rot)))

    # 8) 축발이 캐릭터의 세로축 위에 오도록 몸 전체를 옮긴다. COM의 부모는 회전이 없는 루트다.
    wp, _ = rig.world()
    grip_error = float(np.linalg.norm(wp[l_arm[2]] - grip_target))
    rig.lt[com][0] -= wp[l_foot][0]
    rig.lt[com][1] -= wp[l_foot][1]

    wp, wq = rig.world()
    report = {
        "reach": reaches,
        "wrist_twist_deg": float(np.degrees(wrist_twist)),
        "blade_dir": qrot(wq[weapon], [0, 1, 0]),
        "left_grip_error": grip_error,
        "pivot_offset": float(np.linalg.norm(wp[l_foot][:2])),
        "trail_height": float(wp[r_foot][2] - ground),
        "trail_knee_height": float(wp[r_calf][2]),
        "pelvis_height": float(wp[sk.idx("Pelvis")][2]),
        "blade_tip": wp[weapon] + qrot(wq[weapon], [0.0, 100.0, 0.0]),
    }
    return rig.lt, rig.lq, report


def annotations(duration: float) -> list[tuple[float, str]]:
    """한 바퀴에 한 번의 타격.
    weaponSwing과 HitFrame은 게임의 표준 이벤트이고, Collision_* 은 Precision이 읽는 주석이다(없으면 무시된다).
    앞으로 밀려 나가는 이동 주석은 넣지 않는다."""
    # 시각은 한 바퀴 길이에 대한 비율로 둔다. 바퀴 길이를 바꿔도 순서와 간격의 비가 유지된다.
    marks = [
        (0.000, "Collision_AttackStart"),
        (0.055, "Collision_Add.node(WEAPON)|DamageMult(0.8)|scale(2)|NoRecoil"),
        (0.055, "Collision_ClearTargets.Node(WEAPON)"),
        (0.170, "weaponSwing"),
        (0.280, "HitFrame"),
        (0.985, "Collision_AttackEnd"),
    ]
    return [(round(fraction * duration, 4), text) for fraction, text in marks]


def generate(skeleton_xml: Path, base_xml: Path, design: Design):
    sk = Skeleton(str(skeleton_xml))
    base = fit_to_skeleton(load_spline(str(base_xml)), sk)
    com = sk.idx("COM")
    nf = design.frames_per_turn + 1  # 마지막 프레임은 첫 프레임과 같은 자세(고리)
    nb = len(sk.names)
    T = np.zeros((nf, nb, 3))
    Q = np.zeros((nf, nb, 4))
    S = np.ones((nf, nb, 3))
    sign = 1.0 if design.turns_ccw else -1.0
    worst = None
    for f in range(nf):
        phase = f / design.frames_per_turn
        bob = design.bob * np.cos(4.0 * np.pi * phase)
        lt, lq, report = build_pose(sk, base.t[0], base.q[0], design, bob)
        if worst is None:
            worst = report
        else:
            # 골반 높이가 프레임마다 달라지므로, 점검에는 프레임들 가운데 가장 나쁜 값을 쓴다.
            worst["reach"] = {k: max(v, report["reach"][k]) for k, v in worst["reach"].items()}
            worst["left_grip_error"] = max(worst["left_grip_error"], report["left_grip_error"])
            worst["trail_height"] = min(worst["trail_height"], report["trail_height"])
            worst["trail_knee_height"] = min(worst["trail_knee_height"], report["trail_knee_height"])
        # 몸 전체를 캐릭터의 세로축 둘레로 돌린다. COM의 부모는 회전이 없는 루트다.
        spin = qaxis(UP, sign * 2.0 * np.pi * phase)
        lq[com] = qnorm(qmul(spin, lq[com]))
        lt[com] = qrot(spin, lt[com])
        T[f], Q[f] = lt, lq
    # 이웃 프레임의 쿼터니언이 같은 반구에 있게 맞춘다(보간이 먼 길로 돌지 않게).
    for f in range(1, nf):
        flip = np.sum(Q[f] * Q[f - 1], axis=1) < 0
        Q[f][flip] *= -1.0
    duration = (nf - 1) / FPS
    anim = Animation(T, Q, S, duration, sk.names, annotations(duration))
    return sk, anim, worst


def check(sk: Skeleton, anim: Animation, report: dict) -> list[str]:
    """만든 클립이 의도대로인지 숫자로 확인한다. 문제를 문자열로 돌려준다."""
    problems = []
    for name, reach in report["reach"].items():
        if reach > 0.985:
            problems.append(f"{name}이(가) 거의 다 펴져 있습니다(목표가 팔다리 길이의 {reach:.0%}). 목표에 닿지 않았을 수 있습니다.")
    if report["left_grip_error"] > 1.0:
        problems.append(f"왼손이 손잡이 자리에서 {report['left_grip_error']:.1f} 떨어져 있습니다.")
    if abs(report["blade_dir"][2]) > 0.05:
        problems.append(f"무기가 수평이 아닙니다(위아래 성분 {report['blade_dir'][2]:.2f}).")
    if report["wrist_twist_deg"] > 150:
        problems.append(f"오른 손목을 {report['wrist_twist_deg']:.0f}도 돌려야 해서 손목이 꺾여 보일 수 있습니다.")
    # 고리: 첫 프레임과 마지막 프레임의 월드 자세가 같아야 한다.
    wp0, _ = world_pose(sk.parents, anim.t[0], anim.q[0])
    wp1, _ = world_pose(sk.parents, anim.t[-1], anim.q[-1])
    gap = float(np.max(np.linalg.norm(wp0 - wp1, axis=1)))
    if gap > 0.05:
        problems.append(f"첫 프레임과 마지막 프레임이 {gap:.2f}만큼 어긋납니다(반복할 때 튐).")
    # 발이 땅을 뚫거나 뜨지 않는지.
    feet = [sk.idx("L Foot"), sk.idx("R Foot")]
    positions = np.array([world_pose(sk.parents, anim.t[f], anim.q[f])[0][feet] for f in range(anim.nframes)])
    for i, name in enumerate(("축발", "뒷발")):
        spread = float(positions[:, i, 2].max() - positions[:, i, 2].min())
        if spread > 0.5:
            problems.append(f"{name}의 높이가 프레임마다 {spread:.1f}만큼 달라집니다.")
    # 축발은 모든 프레임에서 캐릭터의 세로축 위에 있어야 한다(땅을 미끄러지지 않게).
    slide = float(np.max(np.linalg.norm(positions[:, 0, :2], axis=1)))
    if slide > 0.1:
        problems.append(f"축발이 세로축에서 최대 {slide:.2f} 벗어납니다(발이 미끄러짐).")
    if report["trail_height"] < 3.0:
        problems.append(f"뒷발이 땅에서 {report['trail_height']:.1f}밖에 뜨지 않아 땅을 끌 수 있습니다.")
    if report["trail_knee_height"] < 14.0:
        problems.append(f"뒷다리 무릎 높이가 {report['trail_knee_height']:.1f}이라 무릎을 꿇은 것처럼 보일 수 있습니다.")
    if not np.all(np.isfinite(anim.t)) or not np.all(np.isfinite(anim.q)):
        problems.append("자세 값에 숫자가 아닌 값이 있습니다.")
    return problems


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--skeleton", type=Path, required=True, help="바닐라 스켈레톤의 hkxc XML")
    parser.add_argument("--base", type=Path, required=True, help="바탕 자세로 쓸 바닐라 양손 대기 모션의 hkxc XML")
    parser.add_argument("--out", type=Path, required=True, help="쓸 XML 경로")
    parser.add_argument("--frames-per-turn", type=int, default=Design.frames_per_turn,
                        help="한 바퀴의 프레임 수(30fps). 기본 18 = 0.6초")
    args = parser.parse_args()

    for label, path in (("스켈레톤", args.skeleton), ("바탕 자세", args.base)):
        if not path.is_file():
            fail(f"{label} 파일이 없습니다: {path}")
    if not 6 <= args.frames_per_turn <= 120:
        fail("--frames-per-turn 은 6~120 사이여야 합니다.")

    design = Design(frames_per_turn=args.frames_per_turn)
    try:
        sk, anim, report = generate(args.skeleton, args.base, design)
    except (KeyError, ValueError, AttributeError, IndexError) as exc:
        fail(f"자세를 만들지 못했습니다(입력 파일이 hkxc가 만든 스켈레톤·모션 XML인지 확인): {exc!r}")
    args.out.parent.mkdir(parents=True, exist_ok=True)
    anim.save_xml(str(args.out))

    print(f"{args.out}: {anim.nframes}프레임, {anim.duration:.3f}초, 한 바퀴 {design.frames_per_turn}프레임")
    print("  팔다리 뻗음(가장 편 프레임): " + ", ".join(f"{k} {v:.0%}" for k, v in report["reach"].items()))
    print(f"  오른 손목 돌림 {report['wrist_twist_deg']:.0f}도, 무기 방향 {np.round(report['blade_dir'], 2)}, "
          f"골반 높이 {report['pelvis_height']:.1f}")
    problems = check(sk, anim, report)
    if problems:
        print("확인 필요:")
        for problem in problems:
            print("  - " + problem)
        raise SystemExit(2)
    print(f"  뒷발 높이 {report['trail_height']:.1f}, 뒷무릎 높이 {report['trail_knee_height']:.1f}, 칼끝(길이 100 기준) 위치 {np.round(report['blade_tip'], 0)}")
    print("  수치 점검 통과(고리 닫힘, 축발 고정, 발 높이 일정, 무기 수평, 왼손이 손잡이에 닿음)")


if __name__ == "__main__":
    main()
