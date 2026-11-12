"""hkxc(serde-hkx)가 만든 XML을 읽고 비압축(interleaved) 애니메이션 XML을 쓰는 최소 도구.

좌표계: Skyrim/Havok, Z 위, 쿼터니언은 (x, y, z, w).
"""
import re
import numpy as np

_NUM = r"-?\d+(?:\.\d+)?(?:[eE][-+]?\d+)?"
_TRIPLE = re.compile(r"\((" + _NUM + r") (" + _NUM + r") (" + _NUM + r")\)")
_QTRF = re.compile(
    r"\((" + _NUM + r") (" + _NUM + r") (" + _NUM + r")\)"
    r"\((" + _NUM + r") (" + _NUM + r") (" + _NUM + r") (" + _NUM + r")\)"
    r"\((" + _NUM + r") (" + _NUM + r") (" + _NUM + r")\)"
)


def _param_block(text, name, start=0):
    """<hkparam name="name" ...> ... </hkparam> 내용을 돌려준다."""
    m = re.search(r'<hkparam name="' + name + r'"[^>]*>(.*?)</hkparam>', text[start:], re.S)
    return m.group(1) if m else None


def _parse_qs_transforms(block):
    vals = np.array([[float(g) for g in m.groups()] for m in _QTRF.finditer(block)])
    return vals[:, 0:3], vals[:, 3:7], vals[:, 7:10]


class Skeleton:
    def __init__(self, path):
        text = open(path, encoding="ascii", errors="replace").read()
        start = text.index('class="hkaSkeleton"')  # 첫 스켈레톤 = 애니메이션 스켈레톤
        end = text.index('class="hkaSkeleton"', start + 10)
        sk = text[start:end]
        self.parents = [int(x) for x in _param_block(sk, "parentIndices").split()]
        bones = sk[sk.index('<hkparam name="bones"'):sk.index('<hkparam name="referencePose"')]
        self.names = re.findall(r'<hkparam name="name">([^<]*)</hkparam>', bones)
        self.ref_t, self.ref_q, self.ref_s = _parse_qs_transforms(_param_block(sk, "referencePose"))
        self.index = {n: i for i, n in enumerate(self.names)}

    def idx(self, short):
        """'R UpperArm' 처럼 일부 이름으로 본 번호를 찾는다."""
        for i, n in enumerate(self.names):
            if n == short or n.startswith("NPC " + short + " [") or n == "NPC " + short:
                return i
        raise KeyError(short)


class Animation:
    """frames x bones 배열(t, q, s)과 주석."""

    def __init__(self, t, q, s, duration, track_names, annotations=None):
        self.t, self.q, self.s = t, q, s
        self.duration = duration
        self.track_names = track_names
        self.annotations = annotations or []  # [(time, text)]

    @property
    def nframes(self):
        return self.t.shape[0]

    @classmethod
    def load_interleaved(cls, path):
        text = open(path, encoding="ascii", errors="replace").read()
        a = text[text.index('class="hkaInterleavedUncompressedAnimation"'):]
        duration = float(_param_block(a, "duration"))
        ntracks = int(_param_block(a, "numberOfTransformTracks"))
        names = re.findall(r'<hkparam name="trackName">([^<]*)</hkparam>', a)[:ntracks]
        t, q, s = _parse_qs_transforms(_param_block(a, "transforms"))
        nf = len(t) // ntracks
        return cls(t.reshape(nf, ntracks, 3), q.reshape(nf, ntracks, 4), s.reshape(nf, ntracks, 3),
                   duration, names)

    def save_xml(self, path):
        nf, nb = self.nframes, self.t.shape[1]
        out = []
        w = out.append
        w('<?xml version="1.0" encoding="ascii"?>')
        w('<hkpackfile classversion="8" contentsversion="hk_2010.2.0-r1" toplevelobject="#0001">')
        w('')
        w('\t<hksection name="__data__">')
        w('')
        w('\t\t<hkobject name="#0003" class="hkaInterleavedUncompressedAnimation" signature="0x930af031">')
        w('\t\t\t<hkparam name="type">HK_INTERLEAVED_ANIMATION</hkparam>')
        w(f'\t\t\t<hkparam name="duration">{self.duration:.6f}</hkparam>')
        w(f'\t\t\t<hkparam name="numberOfTransformTracks">{nb}</hkparam>')
        w('\t\t\t<hkparam name="numberOfFloatTracks">0</hkparam>')
        w('\t\t\t<hkparam name="extractedMotion">null</hkparam>')
        w(f'\t\t\t<hkparam name="annotationTracks" numelements="{nb}">')
        for i, n in enumerate(self.track_names):
            anns = self.annotations if i == 0 else []
            w('\t\t\t\t<hkobject>')
            w(f'\t\t\t\t\t<hkparam name="trackName">{n}</hkparam>')
            if anns:
                w(f'\t\t\t\t\t<hkparam name="annotations" numelements="{len(anns)}">')
                for tm, txt in anns:
                    w('\t\t\t\t\t\t<hkobject>')
                    w(f'\t\t\t\t\t\t\t<hkparam name="time">{tm:.6f}</hkparam>')
                    w(f'\t\t\t\t\t\t\t<hkparam name="text">{txt}</hkparam>')
                    w('\t\t\t\t\t\t</hkobject>')
                w('\t\t\t\t\t</hkparam>')
            else:
                w('\t\t\t\t\t<hkparam name="annotations" numelements="0"></hkparam>')
            w('\t\t\t\t</hkobject>')
        w('\t\t\t</hkparam>')
        w(f'\t\t\t<hkparam name="transforms" numelements="{nf * nb}">')
        for f in range(nf):
            for b in range(nb):
                t, q, s = self.t[f, b], self.q[f, b], self.s[f, b]
                w(f'\t\t\t\t({t[0]:.6f} {t[1]:.6f} {t[2]:.6f})'
                  f'({q[0]:.6f} {q[1]:.6f} {q[2]:.6f} {q[3]:.6f})'
                  f'({s[0]:.6f} {s[1]:.6f} {s[2]:.6f})')
        w('\t\t\t</hkparam>')
        w('\t\t\t<hkparam name="floats" numelements="0"></hkparam>')
        w('\t\t</hkobject>')
        w('')
        w('\t\t<hkobject name="#0004" class="hkaAnimationBinding" signature="0x66eac971">')
        w('\t\t\t<hkparam name="originalSkeletonName">NPC Root [Root]</hkparam>')
        w('\t\t\t<hkparam name="animation">#0003</hkparam>')
        w('\t\t\t<hkparam name="transformTrackToBoneIndices" numelements="0"></hkparam>')
        w('\t\t\t<hkparam name="floatTrackToFloatSlotIndices" numelements="0"></hkparam>')
        w('\t\t\t<hkparam name="blendHint">NORMAL</hkparam>')
        w('\t\t</hkobject>')
        w('')
        w('\t\t<hkobject name="#0002" class="hkaAnimationContainer" signature="0x8dc20333">')
        w('\t\t\t<hkparam name="skeletons" numelements="0"></hkparam>')
        w('\t\t\t<hkparam name="animations" numelements="1">')
        w('\t\t\t\t#0003')
        w('\t\t\t</hkparam>')
        w('\t\t\t<hkparam name="bindings" numelements="1">')
        w('\t\t\t\t#0004')
        w('\t\t\t</hkparam>')
        w('\t\t\t<hkparam name="attachments" numelements="0"></hkparam>')
        w('\t\t\t<hkparam name="skins" numelements="0"></hkparam>')
        w('\t\t</hkobject>')
        w('')
        w('\t\t<hkobject name="#0001" class="hkRootLevelContainer" signature="0x2772c11e">')
        w('\t\t\t<hkparam name="namedVariants" numelements="1">')
        w('\t\t\t\t<hkobject>')
        w('\t\t\t\t\t<hkparam name="name">Merged Animation Container</hkparam>')
        w('\t\t\t\t\t<hkparam name="className">hkaAnimationContainer</hkparam>')
        w('\t\t\t\t\t<hkparam name="variant">#0002</hkparam>')
        w('\t\t\t\t</hkobject>')
        w('\t\t\t</hkparam>')
        w('\t\t</hkobject>')
        w('')
        w('\t</hksection>')
        w('')
        w('</hkpackfile>')
        open(path, "w", encoding="ascii", newline="\n").write("\n".join(out) + "\n")


# ---------------- spline 압축 해제 (hkaSplineCompressedAnimation) ----------------

_ROT_ALIGN = {0: 4, 1: 1, 2: 2, 3: 1, 4: 2, 5: 4}
_ROT_SIZE = {0: 4, 1: 5, 2: 6, 3: 3, 4: 2, 5: 16}


def _place_w(v, w, shift):
    x, y, z = v
    if shift == 0:
        return np.array([w, x, y, z])
    if shift == 1:
        return np.array([x, w, y, z])
    if shift == 2:
        return np.array([x, y, w, z])
    return np.array([x, y, z, w])


def _read_quat(buf, o, qt):
    if qt == 1:  # THREECOMP40
        c = int.from_bytes(buf[o:o + 5], "little")
        mask = (1 << 12) - 1
        v = np.array([c & mask, (c >> 12) & mask, (c >> 24) & mask], float) - (mask >> 1)
        v *= 0.000345436
        w = np.sqrt(max(0.0, 1 - v.dot(v)))
        if (c >> 38) & 1:
            w = -w
        return qnorm(_place_w(v, w, (c >> 36) & 3))
    if qt == 2:  # THREECOMP48
        d = [int.from_bytes(buf[o + 2 * i:o + 2 * i + 2], "little") for i in range(3)]
        mask = (1 << 15) - 1
        v = np.array([d[0] & mask, d[1] & mask, d[2] & mask], float) - (mask >> 1)
        v *= 0.000043161
        w = np.sqrt(max(0.0, 1 - v.dot(v)))
        if d[2] >> 15:
            w = -w
        shift = ((d[1] >> 14) & 2) | ((d[0] >> 15) & 1)
        return qnorm(_place_w(v, w, shift))
    if qt == 5:  # UNCOMPRESSED
        return qnorm(np.frombuffer(bytes(buf[o:o + 16]), "<f4").astype(float))
    raise NotImplementedError(f"rotation quantization {qt}")


def _bspline(u, degree, knots, cps):
    """표준 de Boor. cps: (n+1, d) 배열."""
    n = len(cps) - 1
    if u >= knots[n + 1]:
        k = n
    else:
        k = degree
        while k < n and not (knots[k] <= u < knots[k + 1]):
            k += 1
    d = [cps[j + k - degree].copy() for j in range(degree + 1)]
    for r in range(1, degree + 1):
        for j in range(degree, r - 1, -1):
            i = j + k - degree
            den = knots[i + degree - r + 1] - knots[i]
            a = 0.0 if den == 0 else (u - knots[i]) / den
            d[j] = (1 - a) * d[j - 1] + a * d[j]
    return d[degree]


class _Reader:
    def __init__(self, buf, pos):
        self.b, self.p = buf, pos

    def align(self, n):
        self.p = (self.p + n - 1) // n * n

    def u8(self):
        v = self.b[self.p]; self.p += 1; return v

    def u16(self):
        v = int.from_bytes(self.b[self.p:self.p + 2], "little"); self.p += 2; return v

    def f32(self):
        v = float(np.frombuffer(bytes(self.b[self.p:self.p + 4]), "<f4")[0]); self.p += 4; return v


def _read_vec_track(r, types, quant, default):
    """위치·스케일 트랙. 돌려주는 값: frame(float) → vec3 함수."""
    spline = types & 0xF0
    if spline:
        nitems = r.u16(); degree = r.u8()
        knots = [r.u8() for _ in range(nitems + degree + 2)]
        r.align(4)
        mins, maxs, statics = {}, {}, {}
        for i in range(3):
            if types & (1 << (4 + i)):
                mins[i] = r.f32(); maxs[i] = r.f32()
            elif types & (1 << i):
                statics[i] = r.f32()
        comps = sorted(mins)
        cps = np.zeros((nitems + 1, 3))
        for c in range(nitems + 1):
            for i in range(3):
                if i in mins:
                    q = r.u8() / 255.0 if quant == 0 else r.u16() / 65535.0
                    cps[c, i] = mins[i] + (maxs[i] - mins[i]) * q
                else:
                    cps[c, i] = statics.get(i, default)
        r.align(4)
        return lambda f: _bspline(f, degree, knots, cps)
    vec = np.full(3, float(default))
    for i in range(3):
        if types & (1 << i):
            vec[i] = r.f32()
    return lambda f: vec


def _read_rot_track(r, types, quant):
    al, sz = _ROT_ALIGN[quant], _ROT_SIZE[quant]
    if types & 0xF0:
        nitems = r.u16(); degree = r.u8()
        knots = [r.u8() for _ in range(nitems + degree + 2)]
        r.align(al)
        cps = []
        for _ in range(nitems + 1):
            cps.append(_read_quat(r.b, r.p, quant)); r.p += sz
        cps = np.array(cps)
        for i in range(1, len(cps)):  # 이웃 제어점끼리 같은 반구로
            if np.dot(cps[i - 1], cps[i]) < 0:
                cps[i] = -cps[i]
        r.align(4)
        return lambda f: qnorm(_bspline(f, degree, knots, cps))
    if types & 0x0F:
        r.align(al)
        q = _read_quat(r.b, r.p, quant); r.p += sz
        r.align(4)
        return lambda f: q
    r.align(4)
    ident = np.array([0.0, 0.0, 0.0, 1.0])
    return lambda f: ident


def load_spline(path):
    text = open(path, encoding="ascii", errors="replace").read()
    a = text[text.index('class="hkaSplineCompressedAnimation"'):]
    duration = float(_param_block(a, "duration"))
    ntr = int(_param_block(a, "numberOfTransformTracks"))
    nfl = int(_param_block(a, "numberOfFloatTracks"))
    names = re.findall(r'<hkparam name="trackName">([^<]*)</hkparam>', a)[:ntr]
    nframes = int(_param_block(a, "numFrames"))
    nblocks = int(_param_block(a, "numBlocks"))
    mfpb = int(_param_block(a, "maxFramesPerBlock"))
    boffs = [int(x) for x in _param_block(a, "blockOffsets").split()]
    fboffs = [int(x) for x in _param_block(a, "floatBlockOffsets").split()]
    buf = bytearray(int(x) for x in _param_block(a, "data").split())
    # 주석(이벤트)은 모든 트랙에서 모은다
    anns = []
    for m in re.finditer(r'<hkparam name="time">(' + _NUM + r')</hkparam>\s*<hkparam name="text">([^<]*)</hkparam>', a):
        anns.append((float(m.group(1)), m.group(2)))

    blocks = []
    for b in range(nblocks):
        r = _Reader(buf, boffs[b])
        masks = [tuple(buf[boffs[b] + 4 * i: boffs[b] + 4 * i + 4]) for i in range(ntr)]
        r.p = boffs[b] + 4 * ntr + nfl
        r.align(4)
        tracks = []
        for (qt, pt, rt, st) in masks:
            pq, rq, sq = qt & 3, (qt >> 2) & 0xF, (qt >> 6) & 3
            fp = _read_vec_track(r, pt, pq, 0.0)
            fr = _read_rot_track(r, rt, rq)
            fs = _read_vec_track(r, st, sq, 1.0)
            tracks.append((fp, fr, fs))
        if nfl and r.p > boffs[b] + fboffs[b]:
            raise ValueError(f"block {b}: transform data overran float block ({r.p} > {boffs[b] + fboffs[b]})")
        blocks.append((tracks, r.p - boffs[b], fboffs[b]))

    t = np.zeros((nframes, ntr, 3)); q = np.zeros((nframes, ntr, 4)); s = np.ones((nframes, ntr, 3))
    for f in range(nframes):
        b = min(f // (mfpb - 1), nblocks - 1)
        lf = float(f - b * (mfpb - 1))
        for i, (fp, fr, fs) in enumerate(blocks[b][0]):
            t[f, i] = fp(lf); q[f, i] = fr(lf); s[f, i] = fs(lf)
    anim = Animation(t, q, s, duration, names, sorted(anns))
    anim.block_info = [(used, fl) for _, used, fl in blocks]
    m = re.search(r'<hkparam name="transformTrackToBoneIndices" numelements="(\d+)">(.*?)</hkparam>', text, re.S)
    anim.track_to_bone = [int(x) for x in m.group(2).split()] if m and int(m.group(1)) else list(range(ntr))
    return anim


def fit_to_skeleton(anim, sk):
    """트랙이 일부 뼈만 있는 애니메이션을 스켈레톤 전체 크기로 맞춘다. 빠진 뼈는 기준 포즈."""
    nf, nb = anim.nframes, len(sk.names)
    t = np.tile(sk.ref_t, (nf, 1, 1)); q = np.tile(sk.ref_q, (nf, 1, 1)); s = np.ones((nf, nb, 3))
    for tr, b in enumerate(getattr(anim, "track_to_bone", range(anim.t.shape[1]))):
        t[:, b], q[:, b], s[:, b] = anim.t[:, tr], anim.q[:, tr], anim.s[:, tr]
    out = Animation(t, q, s, anim.duration, sk.names, anim.annotations)
    return out


# ---------------- 쿼터니언 (x, y, z, w) ----------------

def qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return np.array([
        aw * bx + ax * bw + ay * bz - az * by,
        aw * by - ax * bz + ay * bw + az * bx,
        aw * bz + ax * by - ay * bx + az * bw,
        aw * bw - ax * bx - ay * by - az * bz,
    ])


def qconj(q):
    return np.array([-q[0], -q[1], -q[2], q[3]])


def qnorm(q):
    return q / np.linalg.norm(q)


def qrot(q, v):
    """벡터 v를 q로 회전."""
    p = np.array([v[0], v[1], v[2], 0.0])
    return qmul(qmul(q, p), qconj(q))[:3]


def qaxis(axis, angle):
    axis = np.asarray(axis, float)
    axis = axis / np.linalg.norm(axis)
    s = np.sin(angle / 2)
    return np.array([axis[0] * s, axis[1] * s, axis[2] * s, np.cos(angle / 2)])


def qbetween(u, v):
    """u 방향을 v 방향으로 돌리는 최소 회전."""
    u = u / np.linalg.norm(u)
    v = v / np.linalg.norm(v)
    d = np.dot(u, v)
    if d < -0.999999:
        ax = np.cross(u, [1, 0, 0])
        if np.linalg.norm(ax) < 1e-6:
            ax = np.cross(u, [0, 1, 0])
        return qaxis(ax, np.pi)
    c = np.cross(u, v)
    return qnorm(np.array([c[0], c[1], c[2], 1 + d]))


def qslerp(a, b, t):
    d = np.dot(a, b)
    if d < 0:
        b, d = -b, -d
    if d > 0.9995:
        return qnorm(a + t * (b - a))
    th = np.arccos(d)
    return (np.sin((1 - t) * th) * a + np.sin(t * th) * b) / np.sin(th)


# ---------------- 순운동학 ----------------

def world_pose(parents, t, q):
    """한 프레임의 로컬 (t, q) → 월드 (pos, rot). 스케일은 1로 가정."""
    n = len(parents)
    wp = np.zeros((n, 3))
    wq = np.zeros((n, 4))
    for i in range(n):
        p = parents[i]
        if p < 0:
            wp[i], wq[i] = t[i], q[i]
        else:
            wp[i] = wp[p] + qrot(wq[p], t[i])
            wq[i] = qnorm(qmul(wq[p], q[i]))
    return wp, wq
