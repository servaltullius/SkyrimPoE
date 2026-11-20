(() => {
  "use strict";

  const TREE = window.POE_TREE || null;
  if (!TREE || !Array.isArray(TREE.nodes)) {
    const showMissingTreeError = () => {
      const wrap = document.getElementById("tree-canvas-wrap");
      if (wrap && !wrap.querySelector(".tree-error-banner")) {
        const msg = document.createElement("div");
        msg.className = "tree-error-banner";
        msg.style.cssText = "position:absolute;inset:0;display:flex;align-items:center;justify-content:center;color:#f5c06f;font-size:15px;padding:24px;text-align:center;";
        msg.textContent = "패시브 트리 데이터(tree_data.js)를 불러오지 못했습니다. 모드 설치 파일을 확인해 주세요.";
        wrap.appendChild(msg);
      }
      if (window.poeShowToast) {
        window.poeShowToast(false, "패시브 트리 데이터가 로드되지 않았습니다.");
      }
    };
    window.poeTreeModule = {
      setActive(active) {
        if (active) showMissingTreeError();
      },
      updateState() {},
      onImeText() {},
      onImeKey() {},
      blurSearch() {},
    };
    return;
  }

  // ---- DOM 참조 ----
  const canvasWrap = document.getElementById("tree-canvas-wrap");
  const canvas = document.getElementById("tree-canvas");
  const ctx = canvas.getContext("2d");
  const tooltipEl = document.getElementById("tree-tooltip");

  const ptsNormalEl = document.getElementById("tree-pts-normal");
  const ptsAscEl = document.getElementById("tree-pts-asc");
  const regretPillEl = document.getElementById("tree-regret-pill");
  const regretCountEl = document.getElementById("tree-regret-count");
  const classPillEl = document.getElementById("tree-class-pill");
  const ascSelectEl = document.getElementById("tree-asc-select");
  const btnFocusAsc = document.getElementById("btn-tree-focus-asc");
  const btnClearAsc = document.getElementById("btn-tree-clear-asc");
  const searchInput = document.getElementById("tree-search-input");
  const searchCountEl = document.getElementById("tree-search-count");
  const btnSearchNext = document.getElementById("btn-tree-search-next");
  const btnSearchClear = document.getElementById("btn-tree-search-clear");
  const btnCenter = document.getElementById("btn-tree-center");
  const btnChangeClass = document.getElementById("btn-tree-change-class");
  const btnRefundAsc = document.getElementById("btn-tree-refund-asc");
  const btnRefundAll = document.getElementById("btn-tree-refund-all");

  const classModal = document.getElementById("tree-class-modal");
  const classGrid = document.getElementById("tree-class-grid");
  const btnClassModalClose = document.getElementById("btn-class-modal-close");

  const masteryModal = document.getElementById("tree-mastery-modal");
  const masteryTitle = document.getElementById("tree-mastery-title");
  const masteryList = document.getElementById("tree-mastery-list");
  const btnMasteryClose = document.getElementById("btn-mastery-modal-close");
  const btnMasteryRefund = document.getElementById("btn-mastery-refund");

  const jewelModal = document.getElementById("tree-jewel-modal");
  const jewelTitle = document.getElementById("tree-jewel-title");
  const jewelCurrent = document.getElementById("tree-jewel-current");
  const jewelBagList = document.getElementById("tree-jewel-bag-list");
  const btnJewelClose = document.getElementById("btn-jewel-modal-close");
  let openJewelNodeId = 0;

  const treeStatsGrid = document.getElementById("tree-stats-grid");
  const treeKeystonesBox = document.getElementById("tree-keystones-box");
  const treeMilestonesBox = document.getElementById("tree-milestones-box");

  // ---- 상수 (결정 57, 154) ----
  const KIND_NORMAL = 0;
  const KIND_NOTABLE = 1;
  const KIND_KEYSTONE = 2;
  const KIND_MASTERY = 3;
  const KIND_SOCKET = 4;
  const KIND_CLASS_START = 5;
  const KIND_ASCEND_START = 6;

  const TILE_SIZE = 512;
  const MAX_TILES = 96;
  const SPATIAL_CELL = 800;
  const MIN_ZOOM = 0.045;
  const MAX_ZOOM = 0.72;

  const NODE_RADIUS = {
    [KIND_NORMAL]: 27,
    [KIND_NOTABLE]: 42,
    [KIND_KEYSTONE]: 58,
    [KIND_MASTERY]: 44,
    [KIND_SOCKET]: 40,
    [KIND_CLASS_START]: 86,
    [KIND_ASCEND_START]: 52,
  };

  // ---- 정적 트리 인덱스 구축 ----
  const nodeById = new Map();
  const classById = new Map();
  const ascById = new Map();
  const hubOptions = new Map();
  const spatialGrid = new Map();
  const edgeSpatialGrid = new Map();

  for (const cls of TREE.classes || []) {
    classById.set(cls.id, cls);
    for (const asc of cls.ascendancies || []) {
      ascById.set(asc.id, { ...asc, classId: cls.id });
    }
  }

  const masteryEffects = TREE.masteryEffects || {};
  const masteryGroups = TREE.masteryGroups || {};

  function cellKey(cx, cy) {
    return `${cx},${cy}`;
  }

  for (const n of TREE.nodes) {
    nodeById.set(n.id, n);
    if (n.mco && n.mcParent) {
      let arr = hubOptions.get(n.mcParent);
      if (!arr) {
        arr = [];
        hubOptions.set(n.mcParent, arr);
      }
      arr.push(n.id);
    }
    const cx = Math.floor(n.x / SPATIAL_CELL);
    const cy = Math.floor(n.y / SPATIAL_CELL);
    const k = cellKey(cx, cy);
    let bucket = spatialGrid.get(k);
    if (!bucket) {
      bucket = [];
      spatialGrid.set(k, bucket);
    }
    bucket.push(n);

    // 검색용 소문자 문자열 캐시(한글 + 영문 + 마스터리 효과 포함)
    const parts = [n.name || "", n.nameKo || ""];
    if (Array.isArray(n.stats)) parts.push(...n.stats);
    if (Array.isArray(n.statsKo)) parts.push(...n.statsKo);
    if (Array.isArray(n.effects)) {
      for (const eid of n.effects) {
        const eff = masteryEffects[String(eid)];
        if (eff) {
          if (Array.isArray(eff.stats)) parts.push(...eff.stats);
          if (Array.isArray(eff.statsKo)) parts.push(...eff.statsKo);
        }
      }
    }
    n._search = parts.join(" ").toLowerCase();
  }

  // 검수 C1: visualEdges는 compile_tree.py 포맷에 맞춰 TREE.nodes 배열 인덱스로 조회:
  //   직선: [idxA, idxB, isAsc]
  //   원호: [idxA, idxB, isAsc, gx, gy, radius, startAngle, endAngle]
  const visualEdges = (TREE.visualEdges || []).map((e) => {
    const a = TREE.nodes[e[0]];
    const b = TREE.nodes[e[1]];
    if (!a || !b) return null;
    const isArc = e.length >= 8 && e[5] > 0;
    const gx = isArc ? e[3] : 0;
    const gy = isArc ? e[4] : 0;
    const rad = isArc ? e[5] : 0;
    const a1 = isArc ? e[6] : 0;
    const a2 = isArc ? e[7] : 0;
    const minX = isArc ? (gx - rad) : Math.min(a.x, b.x);
    const maxX = isArc ? (gx + rad) : Math.max(a.x, b.x);
    const minY = isArc ? (gy - rad) : Math.min(a.y, b.y);
    const maxY = isArc ? (gy + rad) : Math.max(a.y, b.y);
    const edgeObj = { a, b, isArc, gx, gy, rad, a1, a2, minX, maxX, minY, maxY };

    const cMinX = Math.floor(minX / SPATIAL_CELL);
    const cMaxX = Math.floor(maxX / SPATIAL_CELL);
    const cMinY = Math.floor(minY / SPATIAL_CELL);
    const cMaxY = Math.floor(maxY / SPATIAL_CELL);
    for (let cx = cMinX; cx <= cMaxX; cx++) {
      for (let cy = cMinY; cy <= cMaxY; cy++) {
        const k = cellKey(cx, cy);
        let arr = edgeSpatialGrid.get(k);
        if (!arr) {
          arr = [];
          edgeSpatialGrid.set(k, arr);
        }
        arr.push(edgeObj);
      }
    }
    return edgeObj;
  }).filter(Boolean);

  // ---- 동적 상태 ----
  let isActive = false;
  let dirty = true;
  let rafScheduled = false;
  let renderScale = 1.0;

  let camX = 0;
  let camY = 0;
  let camZoom = 0.12;
  let hasAutoCentered = false;
  let hasReceivedStateSinceOpen = false;
  let classModalDismissed = false;

  let dragging = false;
  let dragMoved = false;
  let dragStartX = 0;
  let dragStartY = 0;
  let dragCamStartX = 0;
  let dragCamStartY = 0;

  let hoveredNode = null;
  let hoverPath = [];
  let hoverPathSet = new Set();
  let lastMouseCssX = 0;
  let lastMouseCssY = 0;

  let searchQuery = "";
  let searchMatches = [];
  let searchMatchSet = new Set();
  let searchFocusIdx = -1;

  let panelData = null;
  let treeState = {
    loaded: true,
    classSelected: false,
    classId: 0,
    ascendancyId: "",
    allocatedNodes: [],
    masterySelections: {},
    availableNormalPoints: 0,
    spentNormalPoints: 0,
    grantedPassivePoints: 0,
    earnedAscendancyPoints: 0,
    spentAscendancyPoints: 0,
    availableAscendancyPoints: 0,
    bossKillCount: 0,
    dragonKillCount: 0,
    ascendancyMilestonesBitmask: 0,
  };
  let allocatedSet = new Set();
  let openMasteryNodeId = 0;

  // ---- 512px 정적 타일 캐시 (LRU 96장) ----
  const tileCache = new Map();

  function getZoomBucket(z) {
    const levels = [0.06, 0.11, 0.20, 0.36, 0.60];
    for (const lv of levels) {
      if (z <= lv * 1.18) return lv;
    }
    return levels[levels.length - 1];
  }

  function evictTileCacheIfNeeded() {
    while (tileCache.size > MAX_TILES) {
      const oldestKey = tileCache.keys().next().value;
      tileCache.delete(oldestKey);
    }
  }

  // ---- 좌표 변환 ----
  function worldToScreen(wx, wy) {
    const w = canvas.width;
    const h = canvas.height;
    return {
      x: (wx - camX) * camZoom * renderScale + w * 0.5,
      y: (wy - camY) * camZoom * renderScale + h * 0.5,
    };
  }

  function screenCssToWorld(cssX, cssY) {
    const rect = canvas.getBoundingClientRect();
    const cx = rect.width * 0.5;
    const cy = rect.height * 0.5;
    return {
      x: (cssX - cx) / camZoom + camX,
      y: (cssY - cy) / camZoom + camY,
    };
  }

  function syncCanvasSize() {
    const rect = canvasWrap.getBoundingClientRect();
    if (rect.width <= 0 || rect.height <= 0) return false;
    // 결정 57: 1080p 세로 상한 렌더 스케일
    renderScale = Math.min(1, 1080 / Math.max(1, window.innerHeight));
    const targetW = Math.max(1, Math.round(rect.width * renderScale));
    const targetH = Math.max(1, Math.round(rect.height * renderScale));
    if (canvas.width !== targetW || canvas.height !== targetH) {
      canvas.width = targetW;
      canvas.height = targetH;
      return true;
    }
    return false;
  }

  function requestRedraw() {
    dirty = true;
    if (!isActive || rafScheduled) return;
    rafScheduled = true;
    window.requestAnimationFrame(() => {
      rafScheduled = false;
      if (!isActive || !dirty) return;
      dirty = false;
      renderFrame();
    });
  }

  // ---- 클라이언트 측 규칙/최단 경로(0-1 BFS, C++ PassiveTree와 동일 규칙) ----
  function isNormalSpendable(n) {
    return !n.asc && n.k !== KIND_CLASS_START && n.k !== KIND_MASTERY && !n.mc;
  }

  function isAscSpendable(n) {
    return Boolean(n.asc) && n.k !== KIND_ASCEND_START && n.k !== KIND_MASTERY && !n.mc;
  }

  function isHubUnlocked(hubId) {
    const opts = hubOptions.get(hubId);
    if (!opts) return false;
    for (const oid of opts) {
      if (allocatedSet.has(oid)) return true;
    }
    return false;
  }

  function buildActiveNormalSeeds() {
    const seeds = new Set();
    if (!treeState.classSelected) return seeds;
    const cls = classById.get(treeState.classId);
    if (cls && Array.isArray(cls.startNeighbors)) {
      for (const nb of cls.startNeighbors) seeds.add(nb);
    }
    for (const nid of allocatedSet) {
      const n = nodeById.get(nid);
      if (n && typeof n.grantStart === "number" && n.grantStart >= 0) {
        const extraCls = classById.get(n.grantStart);
        if (extraCls && Array.isArray(extraCls.startNeighbors)) {
          for (const nb of extraCls.startNeighbors) seeds.add(nb);
        }
      }
    }
    return seeds;
  }

  function isMasteryUnlocked(masteryNode) {
    if (!treeState.classSelected || !masteryNode || masteryNode.k !== KIND_MASTERY) return false;
    const notables = masteryGroups[String(masteryNode.g)];
    if (!Array.isArray(notables)) return false;
    for (const nid of notables) {
      if (allocatedSet.has(nid)) return true;
    }
    return false;
  }

  function computeShortestPath(targetId) {
    if (!treeState.classSelected) return [];
    const target = nodeById.get(targetId);
    if (!target || target.k === KIND_CLASS_START || target.k === KIND_ASCEND_START ||
        target.k === KIND_MASTERY || target.mc || allocatedSet.has(targetId)) {
      return [];
    }

    const parent = new Map();
    const dist = new Map();
    const deque = [];

    if (!target.asc) {
      const seeds = buildActiveNormalSeeds();
      for (const nid of allocatedSet) {
        const n = nodeById.get(nid);
        if (n && isNormalSpendable(n)) {
          dist.set(nid, 0);
          parent.set(nid, 0);
          deque.unshift(nid);
        }
      }
      for (const s of seeds) {
        if (!dist.has(s)) {
          dist.set(s, 1);
          parent.set(s, 0);
          deque.push(s);
        }
      }

      while (deque.length > 0) {
        const cur = deque.shift();
        if (cur === targetId) break;
        const curDist = dist.get(cur);
        const node = nodeById.get(cur);
        if (!node || !Array.isArray(node.adj)) continue;
        for (const nb of node.adj) {
          const nbNode = nodeById.get(nb);
          if (!nbNode || !isNormalSpendable(nbNode)) continue;
          const stepCost = allocatedSet.has(nb) ? 0 : 1;
          const nextDist = curDist + stepCost;
          const prevDist = dist.get(nb);
          if (prevDist !== undefined && prevDist <= nextDist) continue;
          dist.set(nb, nextDist);
          parent.set(nb, cur);
          if (stepCost === 0) deque.unshift(nb);
          else deque.push(nb);
        }
      }
      if (!dist.has(targetId)) return [];
      const path = [];
      for (let cur = targetId; cur !== 0 && cur !== undefined; cur = parent.get(cur)) {
        if (!allocatedSet.has(cur)) path.push(cur);
      }
      path.reverse();
      return path;
    }

    // 전직 트리 0-1 BFS
    if (!treeState.ascendancyId || target.asc !== treeState.ascendancyId) return [];
    const ascDef = ascById.get(treeState.ascendancyId);
    if (!ascDef || !ascDef.startNodeId) return [];

    dist.set(ascDef.startNodeId, 0);
    parent.set(ascDef.startNodeId, 0);
    deque.unshift(ascDef.startNodeId);
    for (const nid of allocatedSet) {
      const n = nodeById.get(nid);
      if (n && isAscSpendable(n) && n.asc === treeState.ascendancyId && !dist.has(nid)) {
        dist.set(nid, 0);
        parent.set(nid, 0);
        deque.unshift(nid);
      }
    }

    while (deque.length > 0) {
      const cur = deque.shift();
      if (cur === targetId) break;
      const curDist = dist.get(cur);
      const node = nodeById.get(cur);
      if (!node || !Array.isArray(node.adj)) continue;
      for (const nb of node.adj) {
        const nbNode = nodeById.get(nb);
        if (!nbNode || nbNode.asc !== treeState.ascendancyId) continue;
        if (node.mc) {
          const unlocked = isHubUnlocked(cur);
          if (unlocked && nbNode.mco) continue;
          if (!unlocked && !nbNode.mco) continue;
        }
        if (node.mco && nbNode.mc) continue;

        const stepCost = (nbNode.mc || nb === ascDef.startNodeId || allocatedSet.has(nb)) ? 0 : 1;
        const nextDist = curDist + stepCost;
        const prevDist = dist.get(nb);
        if (prevDist !== undefined && prevDist <= nextDist) continue;
        dist.set(nb, nextDist);
        parent.set(nb, cur);
        if (stepCost === 0) deque.unshift(nb);
        else deque.push(nb);
      }
    }
    if (!dist.has(targetId)) return [];
    const path = [];
    for (let cur = targetId; cur !== 0 && cur !== undefined; cur = parent.get(cur)) {
      const n = nodeById.get(cur);
      if (n && isAscSpendable(n) && !allocatedSet.has(cur)) path.push(cur);
    }
    path.reverse();
    return path;
  }

  // ---- 벡터 아이콘 그리기 (결정 154: 자체 SVG/Canvas 벡터 심볼) ----
  function catAccentColor(cat) {
    switch (cat) {
      case "str": case "life": case "fire": case "attack": case "melee": return "#e06254";
      case "dex": case "evasion": case "cold": case "speed": return "#62c77c";
      case "int": case "mana": case "es": case "lightning": case "spell": return "#67a8f2";
      case "chaos": return "#be75e8";
      case "armour": case "crit": return "#e8c468";
      case "keystone": return "#f5cc6c";
      case "mastery": return "#72dcff";
      case "ascendancy": return "#f0b85c";
      case "socket": return "#88c9d9";
      default: return "#c9b897";
    }
  }

  function drawEdgePath(c, edge) {
    c.beginPath();
    if (edge.isArc && edge.rad > 0) {
      c.arc(edge.gx, edge.gy, edge.rad, edge.a1, edge.a2, false);
    } else {
      c.moveTo(edge.a.x, edge.a.y);
      c.lineTo(edge.b.x, edge.b.y);
    }
    c.stroke();
  }

  function drawCategoryGlyph(c, x, y, r, cat, color) {
    const s = r * 0.48;
    c.save();
    c.translate(x, y);
    c.strokeStyle = color;
    c.fillStyle = color;
    c.lineWidth = Math.max(2, r * 0.11);
    c.lineCap = "round";
    c.lineJoin = "round";

    switch (cat) {
      case "life":
        c.beginPath();
        c.moveTo(0, s * 0.85);
        c.bezierCurveTo(-s * 1.1, s * 0.1, -s * 0.9, -s * 0.9, 0, -s * 0.35);
        c.bezierCurveTo(s * 0.9, -s * 0.9, s * 1.1, s * 0.1, 0, s * 0.85);
        c.fill();
        break;
      case "mana":
        c.beginPath();
        c.moveTo(0, -s * 0.95);
        c.quadraticCurveTo(s * 0.9, s * 0.15, 0, s * 0.9);
        c.quadraticCurveTo(-s * 0.9, s * 0.15, 0, -s * 0.95);
        c.fill();
        break;
      case "es":
      case "armour":
        c.beginPath();
        c.moveTo(0, -s * 0.9);
        c.lineTo(s * 0.8, -s * 0.45);
        c.lineTo(s * 0.65, s * 0.4);
        c.lineTo(0, s * 0.95);
        c.lineTo(-s * 0.65, s * 0.4);
        c.lineTo(-s * 0.8, -s * 0.45);
        c.closePath();
        c.stroke();
        break;
      case "evasion":
      case "speed":
        c.beginPath();
        c.moveTo(-s * 0.7, s * 0.45);
        c.lineTo(0, -s * 0.65);
        c.lineTo(s * 0.7, s * 0.45);
        c.moveTo(-s * 0.45, s * 0.85);
        c.lineTo(0, s * 0.05);
        c.lineTo(s * 0.45, s * 0.85);
        c.stroke();
        break;
      case "fire":
        c.beginPath();
        c.moveTo(0, -s);
        c.quadraticCurveTo(s * 0.85, -s * 0.1, s * 0.55, s * 0.75);
        c.quadraticCurveTo(0, s * 1.05, -s * 0.55, s * 0.75);
        c.quadraticCurveTo(-s * 0.85, -s * 0.1, 0, -s);
        c.stroke();
        break;
      case "cold":
        for (let i = 0; i < 3; i++) {
          const a = (i * Math.PI) / 3;
          c.beginPath();
          c.moveTo(-Math.cos(a) * s, -Math.sin(a) * s);
          c.lineTo(Math.cos(a) * s, Math.sin(a) * s);
          c.stroke();
        }
        break;
      case "lightning":
        c.beginPath();
        c.moveTo(s * 0.25, -s * 0.95);
        c.lineTo(-s * 0.45, s * 0.05);
        c.lineTo(s * 0.1, s * 0.05);
        c.lineTo(-s * 0.25, s * 0.95);
        c.stroke();
        break;
      case "crit":
      case "spell":
      case "keystone":
        c.beginPath();
        for (let i = 0; i < 8; i++) {
          const rad = i % 2 === 0 ? s : s * 0.36;
          const a = (i * Math.PI) / 4 - Math.PI / 2;
          const px = Math.cos(a) * rad;
          const py = Math.sin(a) * rad;
          if (i === 0) c.moveTo(px, py);
          else c.lineTo(px, py);
        }
        c.closePath();
        c.stroke();
        break;
      case "attack":
      case "melee":
        c.beginPath();
        c.moveTo(-s * 0.7, s * 0.7);
        c.lineTo(s * 0.7, -s * 0.7);
        c.moveTo(-s * 0.35, -s * 0.15);
        c.lineTo(s * 0.15, s * 0.35);
        c.stroke();
        break;
      case "str":
      case "dex":
      case "int": {
        const ch = cat === "str" ? "S" : cat === "dex" ? "D" : "I";
        c.font = `700 ${Math.round(r * 0.85)}px "Segoe UI", sans-serif`;
        c.textAlign = "center";
        c.textBaseline = "middle";
        c.fillText(ch, 0, 1);
        break;
      }
      case "socket":
        c.beginPath();
        c.moveTo(0, -s * 0.85);
        c.lineTo(s * 0.85, 0);
        c.lineTo(0, s * 0.85);
        c.lineTo(-s * 0.85, 0);
        c.closePath();
        c.stroke();
        break;
      case "mastery":
        c.beginPath();
        c.arc(0, 0, s * 0.62, 0, Math.PI * 2);
        c.stroke();
        c.beginPath();
        c.arc(0, 0, s * 0.22, 0, Math.PI * 2);
        c.fill();
        break;
      default:
        c.beginPath();
        c.arc(0, 0, s * 0.32, 0, Math.PI * 2);
        c.fill();
        break;
    }
    c.restore();
  }

  function drawNodeShape(c, n, r, fill, stroke, lineWidth) {
    c.beginPath();
    if (n.k === KIND_KEYSTONE || n.k === KIND_SOCKET) {
      c.moveTo(n.x, n.y - r);
      c.lineTo(n.x + r, n.y);
      c.lineTo(n.x, n.y + r);
      c.lineTo(n.x - r, n.y);
      c.closePath();
    } else if (n.mc) {
      for (let i = 0; i < 6; i++) {
        const a = (i * Math.PI) / 3 - Math.PI / 6;
        const px = n.x + Math.cos(a) * r;
        const py = n.y + Math.sin(a) * r;
        if (i === 0) c.moveTo(px, py);
        else c.lineTo(px, py);
      }
      c.closePath();
    } else {
      c.arc(n.x, n.y, r, 0, Math.PI * 2);
    }
    c.fillStyle = fill;
    c.fill();
    c.lineWidth = lineWidth;
    c.strokeStyle = stroke;
    c.stroke();

    if (n.k === KIND_NOTABLE || n.k === KIND_KEYSTONE || n.k === KIND_CLASS_START || n.k === KIND_ASCEND_START) {
      c.beginPath();
      c.arc(n.x, n.y, r * 0.82, 0, Math.PI * 2);
      c.lineWidth = Math.max(1.5, lineWidth * 0.5);
      c.strokeStyle = stroke;
      c.stroke();
    }
  }

  // ---- 정적 타일 렌더 (비할당 간선·노드 밑그림) ----
  function renderStaticTile(tx, ty, zBucket) {
    const off = document.createElement("canvas");
    off.width = TILE_SIZE;
    off.height = TILE_SIZE;
    const tc = off.getContext("2d");

    const worldPerTile = TILE_SIZE / zBucket;
    const minX = tx * worldPerTile;
    const minY = ty * worldPerTile;
    const maxX = minX + worldPerTile;
    const maxY = minY + worldPerTile;

    tc.save();
    tc.scale(zBucket, zBucket);
    tc.translate(-minX, -minY);

    const pad = 120;
    const cMinX = Math.floor((minX - pad) / SPATIAL_CELL);
    const cMaxX = Math.floor((maxX + pad) / SPATIAL_CELL);
    const cMinY = Math.floor((minY - pad) / SPATIAL_CELL);
    const cMaxY = Math.floor((maxY + pad) / SPATIAL_CELL);

    // 1. 비할당 시각 간선
    const seenEdges = new Set();
    tc.lineCap = "round";
    tc.lineWidth = Math.max(8, 1.4 / zBucket);
    tc.strokeStyle = "rgba(130, 118, 96, 0.34)";
    for (let cx = cMinX; cx <= cMaxX; cx++) {
      for (let cy = cMinY; cy <= cMaxY; cy++) {
        const arr = edgeSpatialGrid.get(cellKey(cx, cy));
        if (!arr) continue;
        for (const e of arr) {
          if (seenEdges.has(e)) continue;
          seenEdges.add(e);
          if (e.maxX < minX - pad || e.minX > maxX + pad || e.maxY < minY - pad || e.minY > maxY + pad) continue;
          drawEdgePath(tc, e);
        }
      }
    }

    // 2. 비할당 정적 노드
    const drawGlyphs = zBucket >= 0.10;
    for (let cx = cMinX; cx <= cMaxX; cx++) {
      for (let cy = cMinY; cy <= cMaxY; cy++) {
        const bucket = spatialGrid.get(cellKey(cx, cy));
        if (!bucket) continue;
        for (const n of bucket) {
          if (n.x < minX - pad || n.x > maxX + pad || n.y < minY - pad || n.y > maxY + pad) continue;
          const r = n.mc ? 40 : (NODE_RADIUS[n.k] || 27);
          const isMastery = n.k === KIND_MASTERY;
          const fill = isMastery ? "rgba(22, 26, 36, 0.9)" : (n.deferred ? "#1d1a1f" : "#181621");
          const stroke = n.k === KIND_CLASS_START
            ? "#b9944c"
            : (n.k === KIND_KEYSTONE ? "#8f7546" : (isMastery ? "#3a4d62" : "#5e5445"));
          const lw = n.k === KIND_NORMAL ? 5 : 7;
          drawNodeShape(tc, n, r, fill, stroke, lw);
          if (drawGlyphs) {
            drawCategoryGlyph(tc, n.x, n.y, r, n.cat, "rgba(186, 174, 152, 0.58)");
          }
        }
      }
    }

    tc.restore();
    return off;
  }

  function getStaticTile(tx, ty, zBucket) {
    const key = `${zBucket}:${tx},${ty}`;
    let tile = tileCache.get(key);
    if (tile) {
      tileCache.delete(key);
      tileCache.set(key, tile);
      return tile;
    }
    tile = renderStaticTile(tx, ty, zBucket);
    tileCache.set(key, tile);
    evictTileCacheIfNeeded();
    return tile;
  }

  // ---- 동적 프레임 렌더 (타일 합성 + 할당/경로/검색/호버 오버레이) ----
  function renderFrame() {
    syncCanvasSize();
    const w = canvas.width;
    const h = canvas.height;
    ctx.clearRect(0, 0, w, h);

    const effZoom = camZoom * renderScale;
    const viewMinX = camX - (w * 0.5) / effZoom;
    const viewMaxX = camX + (w * 0.5) / effZoom;
    const viewMinY = camY - (h * 0.5) / effZoom;
    const viewMaxY = camY + (h * 0.5) / effZoom;

    // 1. 512px 정적 타일 합성 (검수 Minor #3: 프레임당 6ms 생성 예산, 최소 1장 보장 -> 스파이크 방지)
    const TILE_BUDGET_MS = 6.0;
    const zBucket = getZoomBucket(effZoom);
    const worldPerTile = TILE_SIZE / zBucket;
    const tMinX = Math.floor(viewMinX / worldPerTile);
    const tMaxX = Math.floor(viewMaxX / worldPerTile);
    const tMinY = Math.floor(viewMinY / worldPerTile);
    const tMaxY = Math.floor(viewMaxY / worldPerTile);

    const tileStartMs = performance.now();
    let builtThisFrame = 0;
    let deferredTile = false;

    for (let tx = tMinX; tx <= tMaxX; tx++) {
      for (let ty = tMinY; ty <= tMaxY; ty++) {
        const key = `${zBucket}:${tx},${ty}`;
        let tile = tileCache.get(key);
        if (tile) {
          tileCache.delete(key);
          tileCache.set(key, tile);
        } else {
          if (builtThisFrame > 0 && (performance.now() - tileStartMs) >= TILE_BUDGET_MS) {
            deferredTile = true;
            continue;
          }
          tile = getStaticTile(tx, ty, zBucket);
          builtThisFrame++;
        }
        const sx = (tx * worldPerTile - camX) * effZoom + w * 0.5;
        const sy = (ty * worldPerTile - camY) * effZoom + h * 0.5;
        const sw = worldPerTile * effZoom;
        ctx.drawImage(tile, sx, sy, sw + 0.6, sw + 0.6);
      }
    }
    if (deferredTile) {
      requestRedraw();
    }

    // 2. 월드 좌표 동적 오버레이
    ctx.save();
    ctx.translate(w * 0.5, h * 0.5);
    ctx.scale(effZoom, effZoom);
    ctx.translate(-camX, -camY);

    const activeCls = treeState.classSelected ? classById.get(treeState.classId) : null;
    const activeAsc = treeState.ascendancyId ? ascById.get(treeState.ascendancyId) : null;
    const activeStartIds = new Set();
    if (activeCls) activeStartIds.add(activeCls.startNodeId);
    if (activeAsc) activeStartIds.add(activeAsc.startNodeId);
    for (const nid of allocatedSet) {
      const n = nodeById.get(nid);
      if (n && typeof n.grantStart === "number" && n.grantStart >= 0) {
        const ec = classById.get(n.grantStart);
        if (ec) activeStartIds.add(ec.startNodeId);
      }
    }

    const isVisuallyActive = (id) => {
      if (allocatedSet.has(id) || activeStartIds.has(id)) return true;
      const n = nodeById.get(id);
      return Boolean(n && n.mc && isHubUnlocked(id));
    };

    const pad = 160;
    const cMinX = Math.floor((viewMinX - pad) / SPATIAL_CELL);
    const cMaxX = Math.floor((viewMaxX + pad) / SPATIAL_CELL);
    const cMinY = Math.floor((viewMinY - pad) / SPATIAL_CELL);
    const cMaxY = Math.floor((viewMaxY + pad) / SPATIAL_CELL);

    // 2a. 할당 간선(금색) & 호버 경로 간선(청록)
    const seenDynEdges = new Set();
    ctx.lineCap = "round";
    for (let cx = cMinX; cx <= cMaxX; cx++) {
      for (let cy = cMinY; cy <= cMaxY; cy++) {
        const arr = edgeSpatialGrid.get(cellKey(cx, cy));
        if (!arr) continue;
        for (const e of arr) {
          if (seenDynEdges.has(e)) continue;
          seenDynEdges.add(e);
          const aAct = isVisuallyActive(e.a.id);
          const bAct = isVisuallyActive(e.b.id);
          if (aAct && bAct) {
            ctx.lineWidth = Math.max(12, 2.8 / effZoom);
            ctx.strokeStyle = "#f2c35a";
            drawEdgePath(ctx, e);
          } else if (hoverPathSet.size > 0) {
            const aIn = aAct || hoverPathSet.has(e.a.id) || (e.a.mc && hoverPathSet.has(e.b.id));
            const bIn = bAct || hoverPathSet.has(e.b.id) || (e.b.mc && hoverPathSet.has(e.a.id));
            if (aIn && bIn && (hoverPathSet.has(e.a.id) || hoverPathSet.has(e.b.id))) {
              ctx.lineWidth = Math.max(11, 2.4 / effZoom);
              ctx.strokeStyle = "#5be2ff";
              drawEdgePath(ctx, e);
            }
          }
        }
      }
    }

    // 2b. 동적 노드 상태 (할당됨 / 마스터리 해금 / 호버 경로 / 검색 일치 / 호버 강조)
    const mSel = treeState.masterySelections || {};
    const jSel = treeState.socketedJewels || {};
    for (let cx = cMinX; cx <= cMaxX; cx++) {
      for (let cy = cMinY; cy <= cMaxY; cy++) {
        const bucket = spatialGrid.get(cellKey(cx, cy));
        if (!bucket) continue;
        for (const n of bucket) {
          if (n.x < viewMinX - pad || n.x > viewMaxX + pad || n.y < viewMinY - pad || n.y > viewMaxY + pad) continue;
          const r = n.mc ? 40 : (NODE_RADIUS[n.k] || 27);
          const isAlloc = allocatedSet.has(n.id);
          const isStart = activeStartIds.has(n.id);
          const isMasteryAlloc = n.k === KIND_MASTERY && mSel[String(n.id)] !== undefined;
          const isMasteryOpen = n.k === KIND_MASTERY && !isMasteryAlloc && isMasteryUnlocked(n);
          const socketedJ = n.k === KIND_SOCKET ? jSel[String(n.id)] : undefined;
          const isPreview = hoverPathSet.has(n.id);
          const isMatch = searchMatchSet.has(n.id);
          const isHubOpen = n.mc && isHubUnlocked(n.id);

          if (isAlloc || isStart || isMasteryAlloc || isHubOpen) {
            const fill = isStart ? "#3d2c14" : "#2e2313";
            const stroke = isMasteryAlloc ? "#7ce8ff" : (socketedJ ? "#6ee7b7" : "#ffd369");
            drawNodeShape(ctx, n, r, fill, stroke, n.k === KIND_NORMAL ? 7 : 10);
            if (socketedJ) {
              const jr = r * 0.56;
              const jc = socketedJ.jewelColor;
              const jColor = (jc === 0 || jc === "crimson") ? "#f87171"
                : (jc === 1 || jc === "viridian") ? "#4ade80"
                : (jc === 2 || jc === "cobalt") ? "#60a5fa" : "#facc15";
              ctx.beginPath();
              ctx.moveTo(n.x, n.y - jr);
              ctx.lineTo(n.x + jr, n.y);
              ctx.lineTo(n.x, n.y + jr);
              ctx.lineTo(n.x - jr, n.y);
              ctx.closePath();
              ctx.fillStyle = jColor;
              ctx.fill();
            } else if (effZoom >= 0.08) {
              drawCategoryGlyph(ctx, n.x, n.y, r, n.cat, catAccentColor(n.cat));
            }
          } else if (isPreview) {
            drawNodeShape(ctx, n, r, "#142a38", "#65e6ff", 9);
            if (effZoom >= 0.08) {
              drawCategoryGlyph(ctx, n.x, n.y, r, n.cat, "#a8f2ff");
            }
          } else if (isMasteryOpen) {
            drawNodeShape(ctx, n, r, "#142636", "#56c9ff", 8);
            if (effZoom >= 0.08) {
              drawCategoryGlyph(ctx, n.x, n.y, r, "mastery", "#8ee8ff");
            }
          }

          if (isMatch) {
            ctx.beginPath();
            ctx.arc(n.x, n.y, r + 18, 0, Math.PI * 2);
            ctx.lineWidth = Math.max(8, 2.2 / effZoom);
            ctx.strokeStyle = "#4aff9f";
            ctx.stroke();
          }

          if (hoveredNode && hoveredNode.id === n.id) {
            ctx.beginPath();
            ctx.arc(n.x, n.y, r + 12, 0, Math.PI * 2);
            ctx.lineWidth = Math.max(7, 2.0 / effZoom);
            ctx.strokeStyle = "#ffffff";
            ctx.stroke();
          }

          // 클래스 시작점 이름 라벨
          if (n.k === KIND_CLASS_START && effZoom >= 0.06) {
            ctx.font = "700 44px 'Malgun Gothic', sans-serif";
            ctx.textAlign = "center";
            ctx.textBaseline = "top";
            ctx.fillStyle = isStart ? "#ffe28c" : "#cbb896";
            ctx.fillText(n.nameKo || n.name, n.x, n.y + r + 14);
          } else if (n.k === KIND_ASCEND_START && effZoom >= 0.08) {
            ctx.font = "700 36px 'Malgun Gothic', sans-serif";
            ctx.textAlign = "center";
            ctx.textBaseline = "top";
            ctx.fillStyle = isStart ? "#8ee4ff" : "#a79a84";
            ctx.fillText(n.nameKo || n.name, n.x, n.y + r + 10);
          }
        }
      }
    }

    ctx.restore();
  }

  // ---- 히트 테스트 ----
  function findNodeAtWorld(wx, wy) {
    const cx = Math.floor(wx / SPATIAL_CELL);
    const cy = Math.floor(wy / SPATIAL_CELL);
    let best = null;
    let bestDistSq = Infinity;
    for (let dx = -1; dx <= 1; dx++) {
      for (let dy = -1; dy <= 1; dy++) {
        const bucket = spatialGrid.get(cellKey(cx + dx, cy + dy));
        if (!bucket) continue;
        for (const n of bucket) {
          const r = (n.mc ? 40 : (NODE_RADIUS[n.k] || 27)) + Math.max(14, 10 / camZoom);
          const ddx = wx - n.x;
          const ddy = wy - n.y;
          const d2 = ddx * ddx + ddy * ddy;
          if (d2 <= r * r && d2 < bestDistSq) {
            bestDistSq = d2;
            best = n;
          }
        }
      }
    }
    return best;
  }

  // ---- 툴팁 렌더링 ----
  function updateTooltip() {
    if (!hoveredNode || !isActive || !classModal.classList.contains("hidden") ||
        !masteryModal.classList.contains("hidden") ||
        (jewelModal && !jewelModal.classList.contains("hidden"))) {
      tooltipEl.classList.add("hidden");
      return;
    }
    const n = hoveredNode;
    tooltipEl.textContent = "";

    const head = document.createElement("div");
    head.className = "tt-head";
    const title = document.createElement("span");
    title.className = "tt-title";
    title.textContent = n.nameKo || n.name;
    head.appendChild(title);
    if (n.deferred && n.k !== KIND_SOCKET) {
      const tag = document.createElement("span");
      tag.className = "tt-deferred";
      tag.textContent = "추후 적용";
      head.appendChild(tag);
    }
    tooltipEl.appendChild(head);

    const sub = document.createElement("div");
    sub.className = "tt-sub";
    const kindLabels = ["소형 패시브", "주요 패시브", "핵심(키스톤)", "마스터리", "주얼 소켓", "클래스 시작점", "전직 시작점"];
    const enSub = (n.nameKo && n.nameKo !== n.name) ? ` · ${n.name}` : "";
    sub.textContent = `${kindLabels[n.k] || "패시브"}${n.asc ? ` (${n.asc})` : ""}${enSub}`;
    tooltipEl.appendChild(sub);

    if (n.k === KIND_MASTERY) {
      const selEffId = (treeState.masterySelections || {})[String(n.id)];
      if (selEffId !== undefined) {
        const eff = masteryEffects[String(selEffId)];
        const lines = (eff && (eff.statsKo || eff.stats)) || [];
        for (const s of lines) {
          const lineEl = document.createElement("div");
          lineEl.className = "tt-stat";
          lineEl.textContent = `★ ${s}`;
          tooltipEl.appendChild(lineEl);
        }
      }
      const note = document.createElement("div");
      note.className = "tt-note";
      note.textContent = selEffId !== undefined
        ? "좌클릭: 마스터리 효과 변경 · 우클릭: 마스터리 회수"
        : (isMasteryUnlocked(n) ? "좌클릭: 마스터리 효과 선택 (1 포인트)" : "같은 그룹의 주요 패시브 1개 이상 할당 시 해금");
      tooltipEl.appendChild(note);
    } else {
      const lines = (Array.isArray(n.statsKo) && n.statsKo.length > 0) ? n.statsKo : (n.stats || []);
      for (const s of lines) {
        const lineEl = document.createElement("div");
        lineEl.className = "tt-stat";
        lineEl.textContent = s;
        tooltipEl.appendChild(lineEl);
      }
      if (n.k === KIND_SOCKET) {
        const sockJ = ((treeState && treeState.socketedJewels) || {})[String(n.id)];
        if (sockJ) {
          const jHead = document.createElement("div");
          jHead.className = "tt-stat";
          jHead.textContent = `◆ 장착된 주얼: ${sockJ.title || "주얼"}`;
          tooltipEl.appendChild(jHead);
          if (sockJ.card && Array.isArray(sockJ.card.affixLines)) {
            for (const af of sockJ.card.affixLines) {
              const ln = document.createElement("div");
              ln.className = "tt-stat";
              ln.textContent = `  ${af.textKo || ""}`;
              tooltipEl.appendChild(ln);
            }
          }
        }
      }
      if (n.mc) {
        const note = document.createElement("div");
        note.className = "tt-note";
        note.textContent = "다중 선택 허브 (0 포인트): 연결된 하위 선택지 중 1개만 고를 수 있습니다.";
        tooltipEl.appendChild(note);
      } else if (allocatedSet.has(n.id)) {
        const note = document.createElement("div");
        note.className = "tt-note";
        const freeRefund = Boolean(panelData && panelData.freePassiveRefund);
        const ascCost = Number((treeState && treeState.ascendancyRefundCost) || 5);
        const nodeCost = freeRefund ? 0 : (n.asc ? ascCost : 1);
        const costSuffix = nodeCost > 0 ? ` (후회의 오브 ${nodeCost}개)` : " (무료)";
        note.textContent = n.k === KIND_SOCKET
          ? `할당됨 · 좌클릭: 주얼 장착/회수 · 우클릭: 소켓 회수${costSuffix}`
          : `할당됨 · 우클릭으로 회수${costSuffix}`;
        tooltipEl.appendChild(note);
      } else if (hoverPath.length > 0) {
        const note = document.createElement("div");
        note.className = "tt-note";
        note.textContent = `좌클릭으로 최단 경로 할당 (${hoverPath.length} ${n.asc ? "전직 " : ""}포인트)`;
        tooltipEl.appendChild(note);
      }
    }

    tooltipEl.classList.remove("hidden");
    const wrapRect = canvasWrap.getBoundingClientRect();
    const ttRect = tooltipEl.getBoundingClientRect();
    let left = lastMouseCssX + 16;
    let top = lastMouseCssY + 16;
    if (left + ttRect.width > wrapRect.width - 8) left = Math.max(8, lastMouseCssX - ttRect.width - 16);
    if (top + ttRect.height > wrapRect.height - 8) top = Math.max(8, lastMouseCssY - ttRect.height - 16);
    tooltipEl.style.left = `${left}px`;
    tooltipEl.style.top = `${top}px`;
  }

  // ---- 카메라 이동 도우미 ----
  function centerOnNode(nodeId, targetZoom) {
    const n = nodeById.get(nodeId);
    if (!n) return;
    camX = n.x;
    camY = n.y;
    if (typeof targetZoom === "number") {
      camZoom = Math.max(MIN_ZOOM, Math.min(MAX_ZOOM, targetZoom));
    }
    requestRedraw();
  }

  function centerOnClassStart() {
    if (treeState.classSelected) {
      const cls = classById.get(treeState.classId);
      if (cls) {
        centerOnNode(cls.startNodeId, 0.14);
        return;
      }
    }
    camX = 0;
    camY = 0;
    camZoom = 0.095;
    requestRedraw();
  }

  // ---- 모달 (클래스 선택 / 마스터리 선택) ----
  function openClassModal() {
    classGrid.textContent = "";
    for (const cls of TREE.classes || []) {
      const card = document.createElement("button");
      card.type = "button";
      card.className = "class-card";
      if (treeState.classSelected && treeState.classId === cls.id) {
        card.classList.add("active");
      }

      const t = document.createElement("div");
      t.className = "class-card-title";
      t.textContent = `${cls.nameKo || cls.name} (${cls.name})`;
      card.appendChild(t);

      const a = document.createElement("div");
      a.className = "class-card-attrs";
      a.textContent = `힘 ${cls.baseStr} · 민첩 ${cls.baseDex} · 지능 ${cls.baseInt}`;
      card.appendChild(a);

      const ascs = document.createElement("div");
      ascs.className = "class-card-ascs";
      ascs.textContent = "전직: " + (cls.ascendancies || []).map((x) => x.nameKo || x.name).join(", ");
      card.appendChild(ascs);

      card.onclick = () => {
        window.poeSendCommand({
          op: "treeSelectClass",
          reqId: window.poeNextReqId(),
          classId: cls.id,
        });
        classModal.classList.add("hidden");
        centerOnNode(cls.startNodeId, 0.14);
      };
      classGrid.appendChild(card);
    }
    classModal.classList.remove("hidden");
    tooltipEl.classList.add("hidden");
  }

  function openMasteryModal(node) {
    openMasteryNodeId = node.id;
    masteryTitle.textContent = node.nameKo || node.name || "마스터리 선택";
    masteryList.textContent = "";

    const mSel = treeState.masterySelections || {};
    const currentEff = mSel[String(node.id)];
    const usedElsewhere = new Set();
    for (const [midStr, eid] of Object.entries(mSel)) {
      if (Number(midStr) !== node.id) {
        usedElsewhere.add(Number(eid));
      }
    }

    for (const eid of node.effects || []) {
      const eff = masteryEffects[String(eid)];
      if (!eff) continue;
      const btn = document.createElement("button");
      btn.type = "button";
      btn.className = "mastery-opt-btn";
      const lines = (Array.isArray(eff.statsKo) && eff.statsKo.length > 0) ? eff.statsKo : (eff.stats || []);
      const isSelectedHere = Number(currentEff) === eid;
      const isDuplicate = usedElsewhere.has(eid);
      if (isSelectedHere) btn.classList.add("selected");
      btn.disabled = isDuplicate;
      btn.textContent = lines.join(" / ") + (isDuplicate ? " [다른 마스터리에서 선택됨]" : (isSelectedHere ? " [선택됨]" : ""));
      btn.onclick = () => {
        window.poeSendCommand({
          op: "treeSelectMastery",
          reqId: window.poeNextReqId(),
          masteryNodeId: node.id,
          effectId: eid,
        });
        masteryModal.classList.add("hidden");
      };
      masteryList.appendChild(btn);
    }

    btnMasteryRefund.classList.toggle("hidden", currentEff === undefined);
    const freeRefund = Boolean(panelData && panelData.freePassiveRefund);
    btnMasteryRefund.textContent = freeRefund ? "회수" : "회수 (후회 1)";
    masteryModal.classList.remove("hidden");
    tooltipEl.classList.add("hidden");
  }

  // ---- 검색 (한글/영문 + SimpleIME 연동) ----
  function runSearch(query) {
    searchQuery = (query || "").trim().toLowerCase();
    searchMatches = [];
    searchMatchSet = new Set();
    searchFocusIdx = -1;
    if (searchQuery.length > 0) {
      for (const n of TREE.nodes) {
        if (n._search && n._search.includes(searchQuery)) {
          searchMatches.push(n);
          searchMatchSet.add(n.id);
        }
      }
    }
    searchCountEl.textContent = searchQuery ? `${searchMatches.length}건` : "";
    requestRedraw();
  }

  function focusNextSearchMatch() {
    if (searchMatches.length === 0) return;
    searchFocusIdx = (searchFocusIdx + 1) % searchMatches.length;
    const n = searchMatches[searchFocusIdx];
    centerOnNode(n.id, Math.max(camZoom, 0.22));
    searchCountEl.textContent = `${searchFocusIdx + 1}/${searchMatches.length}`;
  }

  searchInput.addEventListener("focus", () => {
    window.poeSendCommand({ op: "treeSearchFocus", focused: true });
  });
  searchInput.addEventListener("blur", () => {
    window.poeSendCommand({ op: "treeSearchFocus", focused: false });
  });
  searchInput.addEventListener("input", () => {
    runSearch(searchInput.value);
  });
  searchInput.addEventListener("keydown", (e) => {
    if (e.key === "Enter") {
      e.preventDefault();
      focusNextSearchMatch();
    } else if (e.key === "Escape") {
      e.preventDefault();
      searchInput.blur();
    }
  });
  btnSearchNext.onclick = () => focusNextSearchMatch();
  btnSearchClear.onclick = () => {
    searchInput.value = "";
    runSearch("");
  };

  // ---- C-27: 후회의 오브 소모 회수 2단계 확인 상태(3초 유효) ----
  let pendingRefundConfirm = null; // { key: string, expiresAt: number }

  function confirmOrProceedRefund(confirmKey, cost, label, onConfirm) {
    const freeRefund = Boolean(panelData && panelData.freePassiveRefund);
    if (freeRefund || !cost || cost <= 0) {
      pendingRefundConfirm = null;
      onConfirm();
      return;
    }
    const now = Date.now();
    if (pendingRefundConfirm && pendingRefundConfirm.key === confirmKey && now <= pendingRefundConfirm.expiresAt) {
      pendingRefundConfirm = null;
      onConfirm();
      return;
    }
    pendingRefundConfirm = { key: confirmKey, expiresAt: now + 3500 };
    if (window.poeShowToast) {
      window.poeShowToast(false, `[확인] ${label}: 후회의 오브 ${cost}개 소모 — 3초 내 다시 클릭하면 확정합니다.`);
    }
  }

  // ---- 툴바·사이드바 갱신 ----
  function renderToolbarAndSidebar() {
    // C-31: INI 보너스 하향 등으로 포인트 초과(음수) 시 0과 초과 경고를 명확히 표시
    const rawAvailN = Number(treeState.availableNormalPoints ?? 0);
    const rawAvailA = Number(treeState.availableAscendancyPoints ?? 0);
    ptsNormalEl.textContent = rawAvailN < 0 ? `0 (초과 ${-rawAvailN})` : String(rawAvailN);
    ptsAscEl.textContent = rawAvailA < 0
      ? `0 (초과 ${-rawAvailA}) / ${treeState.earnedAscendancyPoints ?? 0}`
      : `${rawAvailA} / ${treeState.earnedAscendancyPoints ?? 0}`;

    const freeRefund = Boolean(panelData && panelData.freePassiveRefund);
    if (regretPillEl && regretCountEl) {
      regretPillEl.classList.toggle("hidden", freeRefund);
      regretCountEl.textContent = String(treeState.regretOrbCount ?? 0);
    }

    // C-27: 회수 버튼에 실제 소모될 후회의 오브 수량 표시
    const refundAscCost = Number(treeState.refundAscCost ?? 0);
    const refundAllCost = Number(treeState.refundAllCost ?? 0);
    btnRefundAsc.textContent = (!freeRefund && refundAscCost > 0)
      ? `전직 회수 (후회 ${refundAscCost})`
      : "전직 회수";
    btnRefundAll.textContent = (!freeRefund && refundAllCost > 0)
      ? `전체 회수 (후회 ${refundAllCost})`
      : "전체 회수";

    // C-31: 서버 측 트리 데이터 미로드 시 오류 배너 표시
    const treeNotLoaded = treeState && treeState.loaded === false;
    let errBanner = canvasWrap ? canvasWrap.querySelector(".tree-error-banner") : null;
    if (treeNotLoaded && canvasWrap) {
      if (!errBanner) {
        errBanner = document.createElement("div");
        errBanner.className = "tree-error-banner";
        errBanner.style.cssText = "position:absolute;inset:0;display:flex;align-items:center;justify-content:center;background:rgba(12,10,14,0.82);color:#f5c06f;font-size:15px;padding:24px;text-align:center;z-index:20;";
        errBanner.textContent = "서버 패시브 트리 데이터(tree_3_30.json)를 불러오지 못했습니다.";
        canvasWrap.appendChild(errBanner);
      }
    } else if (errBanner) {
      errBanner.remove();
    }

    const cls = treeState.classSelected ? classById.get(treeState.classId) : null;
    const asc = treeState.ascendancyId ? ascById.get(treeState.ascendancyId) : null;
    classPillEl.textContent = cls
      ? (asc ? `${cls.nameKo || cls.name} · ${asc.nameKo || asc.name}` : (cls.nameKo || cls.name))
      : "클래스 미선택";

    const spentTotal = (treeState.spentNormalPoints || 0) + (treeState.spentAscendancyPoints || 0);
    btnChangeClass.disabled = treeState.classSelected && spentTotal > 0;

    // 전직 드롭다운 갱신
    ascSelectEl.textContent = "";
    const optEmpty = document.createElement("option");
    optEmpty.value = "";
    optEmpty.textContent = cls ? "전직 선택..." : "클래스 먼저 선택";
    ascSelectEl.appendChild(optEmpty);
    if (cls) {
      for (const a of cls.ascendancies || []) {
        const opt = document.createElement("option");
        opt.value = a.id;
        opt.textContent = `${a.nameKo || a.name} (${a.name})`;
        ascSelectEl.appendChild(opt);
      }
      ascSelectEl.value = treeState.ascendancyId || "";
    }
    ascSelectEl.disabled = !cls;
    btnFocusAsc.disabled = !asc;
    btnClearAsc.disabled = !asc || (treeState.spentAscendancyPoints || 0) > 0;

    // 우측 스탯 요약
    treeStatsGrid.textContent = "";
    const s = panelData && panelData.stats;
    if (s) {
      const rows = [
        ["힘 / 민첩 / 지능", `${s.str} / ${s.dex} / ${s.intel}`],
        ["PoE 생명력", s.poeMaxLife],
        ["PoE 마나", s.poeMaxMana],
        ["에너지 실드", `${s.esCurrent} / ${s.esMax}`],
        ["방어도 / 회피", `${s.armour} / ${s.evasion}`],
        ["화염 저항", `${s.fireResist}% (상한 ${s.maxFireResist ?? 75}%)`],
        ["냉기 저항", `${s.coldResist}% (상한 ${s.maxColdResist ?? 75}%)`],
        ["번개 저항", `${s.lightningResist}% (상한 ${s.maxLightningResist ?? 75}%)`],
        ["카오스 저항", `${s.chaosResist}% (상한 ${s.maxChaosResist ?? 75}%)`],
        ["공격/주문 막기", `${s.attackBlock ?? 0}% / ${s.spellBlock ?? 0}%`],
        ["주문 억제/회피", `${s.spellSuppression ?? 0}% / ${s.spellDodge ?? 0}%`],
        ["MoM 흡수", `${s.momRatio ?? 0}%`],
      ];
      for (const [k, v] of rows) {
        const row = document.createElement("div");
        row.className = "stat-row";
        const sk = document.createElement("span");
        sk.className = "stat-k";
        sk.textContent = k;
        const sv = document.createElement("span");
        sv.className = "stat-v";
        sv.textContent = String(v);
        row.appendChild(sk);
        row.appendChild(sv);
        treeStatsGrid.appendChild(row);
      }
    }

    // 활성 키스톤 칩
    treeKeystonesBox.textContent = "";
    const activeKs = [];
    for (const nid of allocatedSet) {
      const n = nodeById.get(nid);
      if (n && n.k === KIND_KEYSTONE) {
        activeKs.push(n.nameKo || n.name);
      }
    }
    if (activeKs.length === 0) {
      const none = document.createElement("span");
      none.className = "section-sub";
      none.textContent = "활성 키스톤 없음";
      treeKeystonesBox.appendChild(none);
    } else {
      for (const name of activeKs) {
        const chip = document.createElement("span");
        chip.className = "ks-chip";
        chip.textContent = name;
        treeKeystonesBox.appendChild(chip);
      }
    }

    // 1~3차 보스 이정표 진행도
    treeMilestonesBox.textContent = "";
    const mask = treeState.ascendancyMilestonesBitmask || 0;
    const bKills = treeState.bossKillCount || 0;
    const dKills = treeState.dragonKillCount || 0;
    const tiers = [
      { bit: 1, label: "1차 (+2P · +30 정신력): 드래곤 1 / 보스 5", cur: `용 ${dKills}/1 · 보스 ${bKills}/5` },
      { bit: 2, label: "2차 (+2P · +30 정신력): 드래곤 3 / 보스 15", cur: `용 ${dKills}/3 · 보스 ${bKills}/15` },
      { bit: 4, label: "3차 (+2P · +30 정신력): 드래곤 6 / 보스 30", cur: `용 ${dKills}/6 · 보스 ${bKills}/30` },
    ];
    for (const t of tiers) {
      const row = document.createElement("div");
      const done = (mask & t.bit) !== 0;
      row.className = `ms-row${done ? " done" : ""}`;
      const l = document.createElement("span");
      l.textContent = t.label;
      const r = document.createElement("span");
      r.textContent = done ? "달성" : t.cur;
      row.appendChild(l);
      row.appendChild(r);
      treeMilestonesBox.appendChild(row);
    }
  }

  // ---- 버튼 이벤트 ----
  ascSelectEl.onchange = () => {
    const val = ascSelectEl.value;
    if (!val) return;
    window.poeSendCommand({
      op: "treeSelectAscendancy",
      reqId: window.poeNextReqId(),
      ascendancyId: val,
    });
  };
  btnFocusAsc.onclick = () => {
    const asc = ascById.get(treeState.ascendancyId);
    if (asc && asc.startNodeId) {
      centerOnNode(asc.startNodeId, 0.24);
    }
  };
  btnClearAsc.onclick = () => {
    window.poeSendCommand({ op: "treeClearAscendancy", reqId: window.poeNextReqId() });
  };
  btnCenter.onclick = () => centerOnClassStart();
  btnChangeClass.onclick = () => openClassModal();
  btnRefundAsc.onclick = () => {
    const cost = Number(treeState.refundAscCost ?? 0);
    confirmOrProceedRefund("refundAsc", cost, "전직 전체 회수", () => {
      window.poeSendCommand({ op: "treeRefundAll", reqId: window.poeNextReqId(), ascendancyOnly: true });
    });
  };
  btnRefundAll.onclick = () => {
    const cost = Number(treeState.refundAllCost ?? 0);
    confirmOrProceedRefund("refundAll", cost, "패시브 전체 회수", () => {
      window.poeSendCommand({ op: "treeRefundAll", reqId: window.poeNextReqId(), ascendancyOnly: false });
    });
  };
  btnClassModalClose.onclick = () => {
    classModalDismissed = true;
    classModal.classList.add("hidden");
  };
  btnMasteryClose.onclick = () => masteryModal.classList.add("hidden");
  if (btnJewelClose && jewelModal) {
    btnJewelClose.onclick = () => {
      openJewelNodeId = 0;
      jewelModal.classList.add("hidden");
    };
  }

  function openJewelModal(node) {
    if (!jewelModal || !jewelTitle || !jewelCurrent || !jewelBagList) return;
    openJewelNodeId = node.id;
    jewelTitle.textContent = `패시브 주얼 소켓 (#${node.id})`;
    jewelCurrent.textContent = "";

    const sockMap = (treeState && treeState.socketedJewels) || {};
    const curJ = sockMap[String(node.id)];
    if (curJ) {
      const head = document.createElement("div");
      head.className = "slot-top";
      const nm = document.createElement("span");
      nm.className = `gear-item-name rarity-${curJ.rarity || "normal"}`;
      nm.textContent = curJ.title || "장착된 주얼";
      const btnUnsock = document.createElement("button");
      btnUnsock.type = "button";
      btnUnsock.className = "btn btn-danger";
      btnUnsock.textContent = "가방으로 회수";
      btnUnsock.onclick = () => {
        window.poeSendCommand({
          op: "treeUnsocketJewel",
          reqId: window.poeNextReqId(),
          nodeId: node.id,
        });
        openJewelNodeId = 0;
        jewelModal.classList.add("hidden");
      };
      head.appendChild(nm);
      head.appendChild(btnUnsock);
      jewelCurrent.appendChild(head);
      if (curJ.card && Array.isArray(curJ.card.affixLines)) {
        for (const af of curJ.card.affixLines) {
          const ln = document.createElement("div");
          ln.className = "gear-affix-line";
          ln.textContent = `${af.tierBadge || ""} ${af.textKo || ""}`;
          jewelCurrent.appendChild(ln);
        }
      }
    } else {
      jewelCurrent.textContent = "현재 장착된 주얼이 없습니다.";
    }

    jewelBagList.textContent = "";
    const bagItems = (panelData && panelData.gear && panelData.gear.bagItems) || [];
    const jewels = bagItems.filter((b) => Boolean(b.isJewel));
    if (jewels.length === 0) {
      const empty = document.createElement("div");
      empty.className = "section-sub";
      empty.textContent = "가방에 보유 중인 주얼이 없습니다.";
      jewelBagList.appendChild(empty);
    } else {
      for (const jw of jewels) {
        const btn = document.createElement("button");
        btn.type = "button";
        btn.className = "mastery-opt-btn";
        const affSummary = (jw.card && Array.isArray(jw.card.affixLines) && jw.card.affixLines.length > 0)
          ? jw.card.affixLines.map((a) => a.textKo).join(" / ")
          : "어픽스 없음(장비·제작 탭에서 오브 제작 가능)";
        const rarityTag = (jw.card && jw.card.rarityBadge) ? jw.card.rarityBadge : "[일반]";
        btn.textContent = `${rarityTag} ${jw.card ? jw.card.title : jw.displayName} — ${affSummary}`;
        btn.onclick = () => {
          window.poeSendCommand({
            op: "treeSocketJewel",
            reqId: window.poeNextReqId(),
            nodeId: node.id,
            baseFormId: jw.baseFormId,
            uniqueId: jw.uniqueId || 0,
          });
          openJewelNodeId = 0;
          jewelModal.classList.add("hidden");
        };
        jewelBagList.appendChild(btn);
      }
    }

    jewelModal.classList.remove("hidden");
    tooltipEl.classList.add("hidden");
  }
  btnMasteryRefund.onclick = () => {
    if (openMasteryNodeId > 0) {
      const targetId = openMasteryNodeId;
      confirmOrProceedRefund(`mastery:${targetId}`, 1, "마스터리 회수", () => {
        window.poeSendCommand({
          op: "treeRefundNode",
          reqId: window.poeNextReqId(),
          nodeId: targetId,
        });
        masteryModal.classList.add("hidden");
      });
    } else {
      masteryModal.classList.add("hidden");
    }
  };

  // ---- 캔버스 마우스 입력 ----
  canvas.addEventListener("mousedown", (e) => {
    if (e.button !== 0) return;
    dragging = true;
    dragMoved = false;
    dragStartX = e.clientX;
    dragStartY = e.clientY;
    dragCamStartX = camX;
    dragCamStartY = camY;
    canvas.classList.add("dragging");
  });

  window.addEventListener("mousemove", (e) => {
    if (!isActive) return;
    const rect = canvas.getBoundingClientRect();
    lastMouseCssX = e.clientX - rect.left;
    lastMouseCssY = e.clientY - rect.top;

    if (dragging) {
      const dx = e.clientX - dragStartX;
      const dy = e.clientY - dragStartY;
      if (Math.abs(dx) > 4 || Math.abs(dy) > 4) {
        dragMoved = true;
      }
      camX = dragCamStartX - dx / camZoom;
      camY = dragCamStartY - dy / camZoom;
      tooltipEl.classList.add("hidden");
      requestRedraw();
      return;
    }

    // 검수 Minor #4: 모달이 열려 있을 때는 뒤쪽 캔버스 히트 테스트·BFS·리드로우를 건너뛴다.
    if (
      !classModal.classList.contains("hidden") ||
      !masteryModal.classList.contains("hidden") ||
      (jewelModal && !jewelModal.classList.contains("hidden"))
    ) {
      return;
    }

    if (lastMouseCssX < 0 || lastMouseCssY < 0 || lastMouseCssX > rect.width || lastMouseCssY > rect.height) {
      if (hoveredNode) {
        hoveredNode = null;
        hoverPath = [];
        hoverPathSet = new Set();
        updateTooltip();
        requestRedraw();
      }
      return;
    }

    const world = screenCssToWorld(lastMouseCssX, lastMouseCssY);
    const hit = findNodeAtWorld(world.x, world.y);
    if ((hit ? hit.id : 0) !== (hoveredNode ? hoveredNode.id : 0)) {
      hoveredNode = hit;
      hoverPath = hit ? computeShortestPath(hit.id) : [];
      hoverPathSet = new Set(hoverPath);
      requestRedraw();
    }
    updateTooltip();
  });

  window.addEventListener("mouseup", (e) => {
    if (!dragging) return;
    dragging = false;
    canvas.classList.remove("dragging");
    if (!isActive || e.button !== 0 || dragMoved) return;

    const rect = canvas.getBoundingClientRect();
    const world = screenCssToWorld(e.clientX - rect.left, e.clientY - rect.top);
    const hit = findNodeAtWorld(world.x, world.y);
    if (!hit) return;

    if (!treeState.classSelected) {
      openClassModal();
      return;
    }
    if (hit.k === KIND_MASTERY) {
      if (isMasteryUnlocked(hit) || (treeState.masterySelections || {})[String(hit.id)] !== undefined) {
        openMasteryModal(hit);
      } else if (window.poeShowToast) {
        window.poeShowToast(false, "같은 그룹의 주요 패시브를 먼저 1개 이상 할당해야 합니다.");
      }
      return;
    }
    if (hit.k === KIND_SOCKET && allocatedSet.has(hit.id)) {
      openJewelModal(hit);
      return;
    }
    const path = computeShortestPath(hit.id);
    if (path.length > 0) {
      window.poeSendCommand({
        op: "treeAllocatePath",
        reqId: window.poeNextReqId(),
        nodes: path,
      });
    }
  });

  canvas.addEventListener("contextmenu", (e) => {
    e.preventDefault();
    if (!isActive) return;
    const rect = canvas.getBoundingClientRect();
    const world = screenCssToWorld(e.clientX - rect.left, e.clientY - rect.top);
    const hit = findNodeAtWorld(world.x, world.y);
    if (!hit) return;

    const hasMastery = hit.k === KIND_MASTERY && (treeState.masterySelections || {})[String(hit.id)] !== undefined;
    if (allocatedSet.has(hit.id) || hasMastery) {
      const ascCost = Number((treeState && treeState.ascendancyRefundCost) || 5);
      const cost = hit.asc ? ascCost : 1;
      const label = `${hit.nameKo || hit.name || ("#" + hit.id)} 회수`;
      confirmOrProceedRefund(`node:${hit.id}`, cost, label, () => {
        window.poeSendCommand({
          op: "treeRefundNode",
          reqId: window.poeNextReqId(),
          nodeId: hit.id,
        });
      });
    }
  });

  canvas.addEventListener("wheel", (e) => {
    if (!isActive) return;
    e.preventDefault();
    const rect = canvas.getBoundingClientRect();
    const cssX = e.clientX - rect.left;
    const cssY = e.clientY - rect.top;
    const before = screenCssToWorld(cssX, cssY);
    const factor = e.deltaY < 0 ? 1.18 : 1 / 1.18;
    camZoom = Math.max(MIN_ZOOM, Math.min(MAX_ZOOM, camZoom * factor));
    const after = screenCssToWorld(cssX, cssY);
    camX += before.x - after.x;
    camY += before.y - after.y;
    requestRedraw();
  }, { passive: false });

  window.addEventListener("resize", () => {
    if (isActive) requestRedraw();
  });

  function safeSetSelection(pos) {
    try {
      searchInput.setSelectionRange(pos, pos);
    } catch (_) {
    }
  }

  // ---- 외부 모듈 API (app.js에서 호출) ----
  window.poeTreeModule = {
    setActive(active) {
      isActive = Boolean(active);
      if (!isActive) {
        dragging = false;
        hoveredNode = null;
        hoverPath = [];
        hoverPathSet = new Set();
        tooltipEl.classList.add("hidden");
        classModal.classList.add("hidden");
        masteryModal.classList.add("hidden");
        if (jewelModal) {
          openJewelNodeId = 0;
          jewelModal.classList.add("hidden");
        }
        if (document.activeElement === searchInput) {
          searchInput.blur();
        }
        return;
      }
      if (hasReceivedStateSinceOpen) {
        if (!hasAutoCentered) {
          hasAutoCentered = true;
          centerOnClassStart();
        }
        if (!treeState.classSelected && !classModalDismissed) {
          openClassModal();
        }
      }
      requestRedraw();
    },

    // 검수 M2 + Minor #2: 패널 닫힘(및 세이브 전환) 시 타일 캐시·모달·카메라 상태 초기화
    onPanelClose() {
      this.setActive(false);
      tileCache.clear();
      hasAutoCentered = false;
      hasReceivedStateSinceOpen = false;
      classModalDismissed = false;
    },

    updateState(data) {
      panelData = data;
      if (data && data.tree) {
        const wasSelected = treeState.classSelected;
        const prevClassId = treeState.classId;
        treeState = data.tree;
        allocatedSet = new Set(treeState.allocatedNodes || []);
        hasReceivedStateSinceOpen = true;

        if (treeState.classSelected) {
          classModal.classList.add("hidden");
          classModalDismissed = false;
          if (isActive && (!hasAutoCentered || !wasSelected || prevClassId !== treeState.classId)) {
            hasAutoCentered = true;
            centerOnClassStart();
          }
        } else if (isActive) {
          if (!hasAutoCentered || wasSelected) {
            hasAutoCentered = true;
            centerOnClassStart();
          }
          if (!classModalDismissed) {
            openClassModal();
          }
        }
        if (hoveredNode) {
          hoverPath = computeShortestPath(hoveredNode.id);
          hoverPathSet = new Set(hoverPath);
        }
      }
      renderToolbarAndSidebar();
      if (isActive) {
        updateTooltip();
        requestRedraw();
      }
    },

    onImeText(text) {
      if (!text || document.activeElement !== searchInput) return;
      const start = searchInput.selectionStart ?? searchInput.value.length;
      const end = searchInput.selectionEnd ?? start;
      const v = searchInput.value;
      searchInput.value = v.slice(0, start) + text + v.slice(end);
      safeSetSelection(start + text.length);
      runSearch(searchInput.value);
    },

    onImeKey(key) {
      if (document.activeElement !== searchInput) return;
      const v = searchInput.value;
      const start = searchInput.selectionStart ?? v.length;
      const end = searchInput.selectionEnd ?? start;
      if (key === "Escape") {
        searchInput.blur();
        return;
      }
      if (key === "Enter") {
        focusNextSearchMatch();
        return;
      }
      if (key === "Backspace") {
        if (start !== end) {
          searchInput.value = v.slice(0, start) + v.slice(end);
          safeSetSelection(start);
        } else if (start > 0) {
          searchInput.value = v.slice(0, start - 1) + v.slice(end);
          safeSetSelection(start - 1);
        }
        runSearch(searchInput.value);
      } else if (key === "Delete") {
        if (start !== end) {
          searchInput.value = v.slice(0, start) + v.slice(end);
          safeSetSelection(start);
        } else if (end < v.length) {
          searchInput.value = v.slice(0, start) + v.slice(end + 1);
          safeSetSelection(start);
        }
        runSearch(searchInput.value);
      } else if (key === "ArrowLeft") {
        safeSetSelection(start !== end ? start : Math.max(0, start - 1));
      } else if (key === "ArrowRight") {
        safeSetSelection(start !== end ? end : Math.min(v.length, end + 1));
      } else if (key === "Home") {
        safeSetSelection(0);
      } else if (key === "End") {
        safeSetSelection(v.length);
      }
    },

    blurSearch() {
      if (document.activeElement === searchInput) {
        searchInput.blur();
      }
    },
  };
})();
