(() => {
  "use strict";

  const KIND_NORMAL = 0;
  const KIND_NOTABLE = 1;
  const KIND_KEYSTONE = 2;
  const KIND_MASTERY = 3;
  const KIND_SOCKET = 4;
  const KIND_CLASS_START = 5;
  const KIND_ASCEND_START = 6;
  const KIND_ATTRIBUTE = 7;

  // PassiveTree.cpp의 규칙을 트리 화면과 미리보기 브리지가 공유한다.
  function createTreeRules(tree) {
    const generation = tree.meta?.contentGeneration ?? 0;
    const nodeById = new Map(tree.nodes.map((node) => [node.id, node]));
    const classById = new Map((tree.classes || []).map((cls) => [cls.id, cls]));
    const ascById = new Map();
    for (const cls of classById.values()) {
      for (const asc of cls.ascendancies || []) {
        ascById.set(asc.id, { ...asc, classId: cls.id });
        for (const alternate of asc.replacements || []) {
          ascById.set(alternate.id, { ...alternate, classId: cls.id });
        }
      }
    }
    const hubOptions = new Map();
    for (const node of tree.nodes) {
      if (!node.mco || !node.mcParent) continue;
      if (!hubOptions.has(node.mcParent)) hubOptions.set(node.mcParent, []);
      hubOptions.get(node.mcParent).push(node.id);
    }
    const masteryGroups = tree.masteryGroups || {};

    function canUseTree(state) {
      return state.loaded !== false && state.classSelected && (state.generation ?? 0) === generation;
    }
    function variantFor(node, state) {
      return node.variants?.[state.classContentId] || node.variants?.[state.ascendancyId];
    }
    function ascendancyOf(node, state) {
      return variantFor(node, state)?.asc || node.asc || "";
    }
    function resolveNode(node, state) {
      const optionId = state.attributeSelections?.[node.id];
      const option = node.attrOptions?.find((item) => item.id === optionId);
      if (option) return { ...option, asc: node.asc || "" };
      const variant = variantFor(node, state);
      return variant ? { ...variant, asc: variant.asc || node.asc || "" } : node;
    }
    function isAllocatable(node) {
      return node.k !== KIND_CLASS_START && node.k !== KIND_ASCEND_START &&
        node.k !== KIND_MASTERY && !node.mc && !node.alias;
    }
    function requirementsMet(node, allocated) {
      return !node.unlock || node.unlock.every((id) => allocated.has(id));
    }
    function isJewelSocket(node) {
      return !node.alias && (node.k === KIND_SOCKET || Boolean(node.jewelSocket));
    }
    function isAllocatedJewelSocket(state, id) {
      const node = nodeById.get(id);
      return (state.generation ?? 0) === generation && Boolean(node && isJewelSocket(node)) &&
        state.allocatedNodes.includes(id);
    }
    function isPhylactery(id) {
      return generation === 1 && id === 17788;
    }
    function jewelEffectMultiplier(state, id) {
      return isAllocatedJewelSocket(state, id) ? (isPhylactery(id) ? 2 : 1) : 0;
    }
    function isMasteryUnlocked(node, state, allocated) {
      return canUseTree(state) && node?.k === KIND_MASTERY &&
        (masteryGroups[node.g] || []).some((id) => allocated.has(id));
    }
    function isHubUnlocked(id, allocated) {
      return (hubOptions.get(id) || []).some((option) => allocated.has(option));
    }
    function activeNormalSeeds(state, allocated) {
      const seeds = new Set();
      if (!state.classSelected) return seeds;
      for (const id of classById.get(state.classId)?.startNeighbors || []) seeds.add(id);
      for (const id of allocated) {
        const cls = classById.get(nodeById.get(id)?.grantStart);
        for (const neighbor of cls?.startNeighbors || []) seeds.add(neighbor);
      }
      return seeds;
    }

    function computeShortestPath(state, targetId, allocated = new Set(state.allocatedNodes)) {
      const target = nodeById.get(targetId);
      if (!canUseTree(state) || !target || !isAllocatable(target) || allocated.has(targetId) ||
          !requirementsMet(target, allocated)) return [];
      const ascId = ascendancyOf(target, state);
      const asc = ascId ? ascById.get(state.ascendancyId) : null;
      if (ascId && (!asc?.startNodeId || asc.id !== ascId)) return [];

      const parent = new Map();
      const distance = new Map();
      const queue = new Map();
      let front = 0;
      let back = 0;
      function enqueue(id, from, cost) {
        const node = nodeById.get(id);
        if (!node || ascendancyOf(node, state) !== ascId || !requirementsMet(node, allocated)) return;
        const isStart = id === asc?.startNodeId;
        if (!isAllocatable(node) && !isStart && !(asc && node.mc)) return;
        const step = isStart || node.mc || node.freeAlloc || allocated.has(id) ? 0 : 1;
        const next = cost + step;
        if ((distance.get(id) ?? Infinity) <= next) return;
        distance.set(id, next);
        parent.set(id, from);
        if (step === 0) queue.set(--front, id);
        else queue.set(back++, id);
      }

      for (const id of allocated) enqueue(id, 0, 0);
      if (asc) enqueue(asc.startNodeId, 0, 0);
      else for (const id of activeNormalSeeds(state, allocated)) enqueue(id, 0, 0);

      while (front < back) {
        const id = queue.get(front);
        queue.delete(front++);
        if (id === targetId) {
          const path = [];
          for (let current = id; current !== 0; current = parent.get(current)) {
            if (isAllocatable(nodeById.get(current)) && !allocated.has(current)) path.push(current);
          }
          return path.reverse();
        }
        const node = nodeById.get(id);
        for (const neighborId of node.adj || []) {
          const neighbor = nodeById.get(neighborId);
          if (!neighbor || (node.mc && isHubUnlocked(id, allocated) === Boolean(neighbor.mco)) ||
              (node.mco && neighbor.mc)) continue;
          enqueue(neighborId, id, distance.get(id));
        }
      }
      return [];
    }

    function isConnected(state, allocated) {
      const kept = new Set();
      const visited = new Set();
      const queue = [];
      function enqueue(id, ascId) {
        if (visited.has(id)) return;
        const node = nodeById.get(id);
        if (!node || ascendancyOf(node, state) !== ascId || !requirementsMet(node, allocated)) return;
        if (!(node.mc && ascId && isHubUnlocked(id, allocated))) {
          if (!allocated.has(id) || !isAllocatable(node)) return;
          kept.add(id);
        }
        visited.add(id);
        queue.push(id);
      }
      function visit(seeds, ascId) {
        for (const id of seeds) enqueue(id, ascId);
        for (let index = 0; index < queue.length; index++) {
          for (const id of nodeById.get(queue[index]).adj || []) enqueue(id, ascId);
        }
        queue.length = 0;
      }
      const asc = ascById.get(state.ascendancyId);
      if (asc) visit(nodeById.get(asc.startNodeId)?.adj || [], asc.id);
      visit(activeNormalSeeds(state, kept), "");
      return kept.size === allocated.size;
    }

    function canAllocateStep(node, state, allocated) {
      if (!isAllocatable(node) || !requirementsMet(node, allocated)) return false;
      const ascId = ascendancyOf(node, state);
      if (!ascId) {
        return activeNormalSeeds(state, allocated).has(node.id) || (node.adj || []).some((id) =>
          allocated.has(id) && !ascendancyOf(nodeById.get(id), state));
      }
      const asc = ascById.get(state.ascendancyId);
      if (!asc?.startNodeId || asc.id !== ascId) return false;
      if (node.mco) {
        const hub = nodeById.get(node.mcParent);
        return Boolean(hub?.mc && requirementsMet(hub, allocated) && !isHubUnlocked(hub.id, allocated) &&
          (hub.adj || []).some((id) => id === asc.startNodeId || (allocated.has(id) && !nodeById.get(id).mco)));
      }
      return (node.adj || []).some((id) => {
        if (id === asc.startNodeId) return true;
        const neighbor = nodeById.get(id);
        return neighbor && ascendancyOf(neighbor, state) === ascId && (allocated.has(id) ||
          (neighbor.mc && requirementsMet(neighbor, allocated) && isHubUnlocked(id, allocated)));
      });
    }

    function pointUsage(state, allocated = new Set(state.allocatedNodes), masteries = state.masterySelections || {}) {
      let normal = Object.keys(masteries).length;
      let ascendancy = 0;
      let granted = 0;
      for (const id of allocated) {
        const node = nodeById.get(id);
        if (!node) continue;
        granted += node.grantPts || 0;
        if (!isAllocatable(node) || node.freeAlloc) continue;
        if (ascendancyOf(node, state)) ascendancy++;
        else normal++;
      }
      return { normal, ascendancy, granted };
    }

    function pointTotals(state, { exileLevel, atlasCompletedTierMask, bonusPassivePoints, bonusAscendancyPoints }) {
      function countBits(mask) {
        let count = 0;
        while (mask) {
          mask &= mask - 1;
          count++;
        }
        return count;
      }
      const { normal, ascendancy, granted } = pointUsage(state);
      const milestones = 2 * countBits(state.ascendancyMilestonesBitmask & 0xf);
      const earned = Math.min(8, milestones + Math.max(0, bonusAscendancyPoints));
      return {
        availableNormalPoints: Math.max(1, Math.min(100, exileLevel)) - 1 +
          countBits(atlasCompletedTierMask & 0xffff) + milestones + Math.max(0, bonusPassivePoints) + granted - normal,
        availableAscendancyPoints: earned - ascendancy,
        spentNormalPoints: normal,
        spentAscendancyPoints: ascendancy,
        grantedPassivePoints: granted,
        earnedAscendancyPoints: earned,
      };
    }

    function planAllocation(state, path, choices) {
      if (!canUseTree(state) || !path.length) return null;
      const allocated = new Set(state.allocatedNodes);
      const attributes = { ...state.attributeSelections };
      let normal = state.availableNormalPoints;
      let ascendancy = state.availableAscendancyPoints;
      for (const id of path) {
        if (allocated.has(id)) continue;
        const node = nodeById.get(id);
        if (!node) return null;
        if (node.mc) continue;
        if (!canAllocateStep(node, state, allocated)) return null;
        if (node.k === KIND_ATTRIBUTE) {
          const optionId = choices.get(id);
          if (!node.attrOptions.some((option) => option.id === optionId)) return null;
          attributes[id] = optionId;
        }
        if (!node.freeAlloc) {
          if (ascendancyOf(node, state)) {
            if (ascendancy < 1) return null;
            ascendancy--;
          } else {
            if (normal < 1) return null;
            normal--;
          }
        }
        normal += node.grantPts || 0;
        allocated.add(id);
      }
      if (allocated.size === state.allocatedNodes.length || normal < 0 || ascendancy < 0 ||
          !isConnected(state, allocated)) return null;
      return { allocatedNodes: [...allocated].sort((a, b) => a - b), attributeSelections: attributes };
    }

    function attributeChangeCost(state, nodeId, optionId, freeRefund) {
      return freeRefund || state.attributeSelections?.[nodeId] === optionId ? 0 : 1;
    }
    function planAttributeChange(state, nodeId, optionId, { freeRefund, regretOrbCount }) {
      const node = nodeById.get(nodeId);
      if (!canUseTree(state) || node?.k !== KIND_ATTRIBUTE || !state.allocatedNodes.includes(nodeId) ||
          state.attributeSelections?.[nodeId] === undefined ||
          !node.attrOptions.some((option) => option.id === optionId)) return null;
      const cost = attributeChangeCost(state, nodeId, optionId, freeRefund);
      if (regretOrbCount < cost) return null;
      return { attributeSelections: { ...state.attributeSelections, [nodeId]: optionId }, cost };
    }

    function pruneMasteries(state, allocated, masteries) {
      let orphaned = 0;
      for (const id of Object.keys(masteries)) {
        if (isMasteryUnlocked(nodeById.get(Number(id)), state, allocated)) continue;
        delete masteries[id];
        orphaned++;
      }
      return orphaned;
    }
    function refundNodeCost(state, nodeId, { freeRefund, ascendancyRefundCost }) {
      if (freeRefund || !canUseTree(state)) return 0;
      if (state.masterySelections?.[nodeId] !== undefined) return 1;
      const node = nodeById.get(nodeId);
      if (!node || node.freeAlloc || !state.allocatedNodes.includes(nodeId)) return 0;
      if (ascendancyOf(node, state)) return Math.max(1, ascendancyRefundCost);
      const remaining = new Set(state.allocatedNodes);
      remaining.delete(nodeId);
      return 1 + pruneMasteries(state, remaining, { ...state.masterySelections });
    }
    function refundDependents(state, nodeId) {
      if ((state.generation ?? 0) !== generation) return [];
      return state.allocatedNodes.filter((id) => nodeById.get(id)?.unlock?.includes(nodeId));
    }

    function planRefund(state, nodeId, { freeRefund, ascendancyRefundCost, regretOrbCount }) {
      if (!canUseTree(state)) return null;
      const allocated = new Set(state.allocatedNodes);
      const masteries = { ...state.masterySelections };
      const attributes = { ...state.attributeSelections };
      let cost = 0;
      if (masteries[nodeId] !== undefined) {
        delete masteries[nodeId];
        cost = freeRefund ? 0 : 1;
      } else {
        const node = nodeById.get(nodeId);
        if (!node || refundDependents(state, nodeId).length || !allocated.delete(nodeId) ||
            !isConnected(state, allocated)) return null;
        delete attributes[nodeId];
        const orphaned = pruneMasteries(state, allocated, masteries);
        const before = pointUsage(state);
        const after = pointUsage(state, allocated, masteries);
        const delta = after.granted - before.granted + before.normal - after.normal;
        if (delta < 0 && state.availableNormalPoints + delta < 0) return null;
        if (!freeRefund && !node.freeAlloc) {
          cost = ascendancyOf(node, state) ? Math.max(1, ascendancyRefundCost) : 1 + orphaned;
        }
      }
      if (regretOrbCount < cost) return null;
      return {
        allocatedNodes: [...allocated], masterySelections: masteries,
        attributeSelections: attributes, cost,
      };
    }

    function refundAllCost(state, ascendancyOnly, { freeRefund, ascendancyRefundCost }) {
      if (freeRefund || !canUseTree(state)) return 0;
      const usage = pointUsage(state);
      return usage.ascendancy * Math.max(1, ascendancyRefundCost) + (ascendancyOnly ? 0 : usage.normal);
    }

    function planRefundAll(state, ascendancyOnly, settings) {
      if (!canUseTree(state)) return null;
      const cost = refundAllCost(state, ascendancyOnly, settings);
      if (settings.regretOrbCount < cost) return null;
      if (!ascendancyOnly) {
        return { allocatedNodes: [], attributeSelections: {}, masterySelections: {}, ascendancyId: "", cost };
      }
      const remaining = state.allocatedNodes.filter((id) => !ascendancyOf(nodeById.get(id), state));
      const next = { ...state, ascendancyId: "", allocatedNodes: remaining };
      const before = pointUsage(state);
      const after = pointUsage(next);
      const delta = after.granted - before.granted + before.normal - after.normal;
      if (!isConnected(next, new Set(remaining)) || (delta < 0 && state.availableNormalPoints + delta < 0)) return null;
      const attributes = Object.fromEntries(remaining.filter((id) => state.attributeSelections?.[id] !== undefined)
        .map((id) => [id, state.attributeSelections[id]]));
      return { allocatedNodes: remaining, attributeSelections: attributes, ascendancyId: "", cost };
    }

    return {
      generation, nodeById, classById, ascById, canUseTree, resolveNode, ascendancyOf, isAllocatable,
      requirementsMet, isJewelSocket, isAllocatedJewelSocket, isPhylactery, jewelEffectMultiplier,
      isMasteryUnlocked, isHubUnlocked, computeShortestPath, refundDependents,
      pointUsage, pointTotals, planAllocation, attributeChangeCost, planAttributeChange, refundNodeCost, planRefund,
      refundAllCost, planRefundAll,
    };
  }

  if (typeof module !== "undefined" && module.exports && typeof window === "undefined") {
    module.exports = { createTreeRules };
    return;
  }

  const TREE = window.POE_TREE || null;
  if (!TREE || !Array.isArray(TREE.nodes)) {
    const showMissingTreeError = () => {
      const wrap = document.getElementById("tree-canvas-wrap");
      if (wrap && !wrap.querySelector(".tree-error-banner")) {
        const msg = document.createElement("div");
        msg.className = "tree-error-banner";
        msg.style.cssText = "position:absolute;inset:0;display:flex;align-items:center;justify-content:center;color:#f5c06f;font-size:15px;padding:24px;text-align:center;";
        msg.textContent = "패시브 트리 데이터를 불러오지 못했습니다. 모드 설치 파일을 확인해 주세요.";
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

  const attributeModal = document.getElementById("tree-attribute-modal");
  const attributeTitle = document.getElementById("tree-attribute-title");
  const attributeList = document.getElementById("tree-attribute-list");
  const btnAttributeClose = document.getElementById("btn-attribute-close");
  const btnAttributeApply = document.getElementById("btn-attribute-apply");
  let attributeRequest = null;

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
  const bounds = TREE.bounds;
  const wideTree = bounds && Math.max(bounds.maxX - bounds.minX, bounds.maxY - bounds.minY) > 25000;
  const TILE_SIZE = 512;
  const MAX_TILES = wideTree ? 112 : 96;
  const SPATIAL_CELL = wideTree ? 1100 : 800;
  const MIN_ZOOM = wideTree ? 0.020 : 0.045;
  const MAX_ZOOM = wideTree ? 0.84 : 0.72;
  const ZOOM_BUCKETS = wideTree ? [0.025, 0.045, 0.08, 0.14, 0.25, 0.45, 0.72]
    : [0.06, 0.11, 0.20, 0.36, 0.60];

  const NODE_RADIUS = {
    [KIND_NORMAL]: 27,
    [KIND_NOTABLE]: 42,
    [KIND_KEYSTONE]: 58,
    [KIND_MASTERY]: 44,
    [KIND_SOCKET]: 40,
    [KIND_CLASS_START]: 86,
    [KIND_ASCEND_START]: 52,
    [KIND_ATTRIBUTE]: 30,
  };

  // ---- 정적 트리 인덱스 구축 ----
  const rules = createTreeRules(TREE);
  const { nodeById, classById, ascById } = rules;
  const classStartLabels = new Map();
  const spatialGrid = new Map();
  const edgeSpatialGrid = new Map();

  for (const cls of classById.values()) {
    if (!classStartLabels.has(cls.startNodeId)) classStartLabels.set(cls.startNodeId, []);
    classStartLabels.get(cls.startNodeId).push(cls);
  }

  const masteryEffects = TREE.masteryEffects || {};
  const hasMasteries = Object.keys(masteryEffects).length > 0;
  const conditionalNodes = TREE.nodes.filter((node) => node.unlock?.length);
  const searchText = new Map();
  const lockedNodes = new Set();

  function localizedName(node) {
    return node.nameKo || node.name;
  }

  function localizedStats(node) {
    return node.statsKo?.length ? node.statsKo : (node.stats || []);
  }

  function cellKey(cx, cy) {
    return `${cx},${cy}`;
  }

  for (const n of TREE.nodes) {
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
    const entries = [n, ...(n.attrOptions || []), ...(classStartLabels.get(n.id) || [])];
    for (const eid of n.effects || []) {
      if (masteryEffects[eid]) entries.push(masteryEffects[eid]);
    }
    searchText.set(n.id, entries.flatMap((entry) =>
      [entry.name, entry.nameKo, ...(entry.stats || []), ...(entry.statsKo || [])]).join(" ").toLowerCase());
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
    generation: rules.generation,
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

  // ---- 512px 정적 타일 캐시 (LRU) ----
  const tileCache = new Map();

  function getZoomBucket(z) {
    for (const lv of ZOOM_BUCKETS) {
      if (z <= lv * 1.18) return lv;
    }
    return ZOOM_BUCKETS[ZOOM_BUCKETS.length - 1];
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

  function computeShortestPath(targetId) {
    return rules.computeShortestPath(treeState, targetId, allocatedSet);
  }

  function isMasteryUnlocked(node) {
    return hasMasteries && rules.isMasteryUnlocked(node, treeState, allocatedSet);
  }
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
    } else if (n.k === KIND_ATTRIBUTE) {
      const half = r / Math.SQRT2;
      c.rect(n.x - half, n.y - half, half * 2, half * 2);
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
      return Boolean(n && n.mc && rules.isHubUnlocked(id, allocatedSet));
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
          const isMasteryAlloc = hasMasteries && n.k === KIND_MASTERY && mSel[n.id] !== undefined;
          const isMasteryOpen = n.k === KIND_MASTERY && !isMasteryAlloc && isMasteryUnlocked(n);
          const socketedJ = rules.isJewelSocket(n) ? jSel[n.id] : undefined;
          const isPreview = hoverPathSet.has(n.id);
          const isMatch = searchMatchSet.has(n.id);
          const isHubOpen = n.mc && rules.isHubUnlocked(n.id, allocatedSet);

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
          } else if (lockedNodes.has(n.id)) {
            drawNodeShape(ctx, n, r, "#251a25", "#946177", 8);
            if (effZoom >= 0.06) {
              ctx.strokeStyle = "#d4a2b5";
              ctx.lineWidth = 5;
              ctx.strokeRect(n.x - 11, n.y - 1, 22, 17);
              ctx.beginPath();
              ctx.arc(n.x, n.y - 1, 8, Math.PI, 2 * Math.PI);
              ctx.stroke();
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
            const labelSize = wideTree ? Math.max(44, 10 / effZoom) : 44;
            ctx.font = `700 ${labelSize}px 'Malgun Gothic', sans-serif`;
            ctx.textAlign = "center";
            ctx.textBaseline = "top";
            ctx.fillStyle = isStart ? "#ffe28c" : "#cbb896";
            const classes = classStartLabels.get(n.id);
            ctx.fillText(classes ? classes.map(localizedName).join(" / ") : localizedName(n), n.x, n.y + r + 14);
          } else if (n.k === KIND_ASCEND_START && effZoom >= 0.08) {
            ctx.font = "700 36px 'Malgun Gothic', sans-serif";
            ctx.textAlign = "center";
            ctx.textBaseline = "top";
            ctx.fillStyle = isStart ? "#8ee4ff" : "#a79a84";
            ctx.fillText(localizedName(rules.resolveNode(n, treeState)), n.x, n.y + r + 10);
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
        !attributeModal.classList.contains("hidden") ||
        (jewelModal && !jewelModal.classList.contains("hidden"))) {
      tooltipEl.classList.add("hidden");
      return;
    }
    const n = hoveredNode;
    const view = rules.resolveNode(n, treeState);
    const startClasses = classStartLabels.get(n.id);
    const isSocket = rules.isJewelSocket(n);
    tooltipEl.textContent = "";

    const head = document.createElement("div");
    head.className = "tt-head";
    const title = document.createElement("span");
    title.className = "tt-title";
    title.textContent = startClasses ? startClasses.map(localizedName).join(" / ") : localizedName(view);
    head.appendChild(title);
    if (view.deferred) {
      const tag = document.createElement("span");
      tag.className = "tt-deferred";
      tag.textContent = "[추후 적용]";
      head.appendChild(tag);
    }
    tooltipEl.appendChild(head);

    const sub = document.createElement("div");
    sub.className = "tt-sub";
    const kindLabels = ["소형 패시브", "주요 패시브", "핵심(키스톤)", "마스터리", "주얼 소켓", "클래스 시작점", "전직 시작점", "능력치 선택"];
    const asc = ascById.get(view.asc);
    const displayNames = startClasses || [view];
    const enSub = displayNames.some((entry) => entry.nameKo && entry.nameKo !== entry.name)
      ? ` · ${displayNames.map((entry) => entry.name).join(" / ")}` : "";
    sub.textContent = `${kindLabels[n.k] || "패시브"}${asc ? ` (${localizedName(asc)})` : ""}${enSub}`;
    tooltipEl.appendChild(sub);

    if (n.k === KIND_MASTERY && hasMasteries) {
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
      for (const [index, s] of localizedStats(view).entries()) {
        const lineEl = document.createElement("div");
        lineEl.className = "tt-stat";
        lineEl.textContent = s;
        const support = view.statSupport?.[index];
        if (support && support.status !== "full") {
          lineEl.classList.add("tt-stat-inactive");
          const reason = document.createElement("div");
          reason.className = "tt-stat-reason";
          const label = { partial: "일부 적용", mappingOnly: "매핑만", unsupported: "미적용" }[support.status];
          reason.textContent = support.reason ? `${label} · ${support.reason}` : label;
          lineEl.appendChild(reason);
        }
        tooltipEl.appendChild(lineEl);
      }
      if (isSocket) {
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
      if (n.freeAlloc || (isSocket && n.k !== KIND_SOCKET)) {
        const note = document.createElement("div");
        note.className = "tt-note";
        note.textContent = [n.freeAlloc ? "소비 포인트 0" : "",
          isSocket && n.k !== KIND_SOCKET ? "주얼 소켓" : ""].filter(Boolean).join(" · ");
        tooltipEl.appendChild(note);
      }
      if (lockedNodes.has(n.id) || n.alias) {
        const note = document.createElement("div");
        note.className = "tt-note tt-locked";
        const required = (n.unlock || []).filter((id) => !allocatedSet.has(id)).map((id) => {
          const node = nodeById.get(id);
          return node ? localizedName(rules.resolveNode(node, treeState)) : `#${id}`;
        });
        note.textContent = n.alias ? "별칭 소켓 · 직접 할당할 수 없습니다." : `잠김 · 필요 노드 ${required.join(", ")}`;
        tooltipEl.appendChild(note);
      } else if (n.mc) {
        const note = document.createElement("div");
        note.className = "tt-note";
        note.textContent = "다중 선택 허브 (0 포인트): 연결된 하위 선택지 중 1개만 고를 수 있습니다.";
        tooltipEl.appendChild(note);
      } else if (allocatedSet.has(n.id)) {
        const note = document.createElement("div");
        note.className = "tt-note";
        const nodeCost = refundCostFor(n.id);
        const costSuffix = nodeCost === null ? "" : (nodeCost > 0 ? ` (후회의 오브 ${nodeCost}개)` : " (무료)");
        const action = isSocket ? " · 좌클릭: 주얼 장착/회수" : n.k === KIND_ATTRIBUTE ? " · 좌클릭: 능력치 변경" : "";
        const blocked = refundBlockMessage(n.id);
        note.textContent = `할당됨${action} · ${blocked || `우클릭: 노드 회수${costSuffix}`}`;
        note.classList.toggle("tt-locked", Boolean(blocked));
        tooltipEl.appendChild(note);
      } else if (hoverPath.length > 0) {
        const note = document.createElement("div");
        note.className = "tt-note";
        const cost = hoverPath.reduce((sum, id) => sum + (nodeById.get(id).freeAlloc ? 0 : 1), 0);
        note.textContent = `좌클릭으로 최단 경로 할당 (${cost} ${rules.ascendancyOf(n, treeState) ? "전직 " : ""}포인트)`;
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
    camX = wideTree ? (bounds.minX + bounds.maxX) / 2 : 0;
    camY = wideTree ? (bounds.minY + bounds.maxY) / 2 : 0;
    camZoom = wideTree ? 0.05 : 0.095;
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
      ascs.textContent = "전직 " + (cls.ascendancies || []).map((asc) => {
        const alternateNames = (asc.replacements || []).map(localizedName);
        return [localizedName(asc), ...alternateNames].join(" / ");
      }).join(", ");
      card.appendChild(ascs);

      card.onclick = () => {
        window.poeSendCommand({
          op: "treeSelectClass",
          reqId: window.poeNextReqId(),
          classContentId: cls.contentId || `cls.${cls.name}`,
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
        const send = () => {
          window.poeSendCommand({
            op: "treeSelectMastery",
            reqId: window.poeNextReqId(),
            masteryNodeId: node.id,
            effectId: eid,
          });
          masteryModal.classList.add("hidden");
        };
        // 재검수 R-7: 이미 고른 효과를 다른 효과로 바꾸면 후회의 오브를 쓰므로 회수와 같은 두 번 클릭 확인을 거친다.
        if (currentEff !== undefined && !isSelectedHere) {
          confirmOrProceedRefund(`masterySwap:${node.id}:${eid}`, masterySwapCostFor(node.id), "마스터리 효과 교체", send);
        } else {
          send();
        }
      };
      masteryList.appendChild(btn);
    }

    btnMasteryRefund.classList.toggle("hidden", currentEff === undefined);
    const freeRefund = Boolean(panelData && panelData.freePassiveRefund);
    const masteryRefundCost = freeRefund ? 0 : refundCostFor(node.id);
    btnMasteryRefund.textContent = (masteryRefundCost && masteryRefundCost > 0) ? `회수 (후회 ${masteryRefundCost})` : "회수";
    masteryModal.classList.remove("hidden");
    tooltipEl.classList.add("hidden");
  }

  function closeAttributeModal() {
    attributeRequest = null;
    attributeModal.classList.add("hidden");
  }

  function openAttributeModal(path, changingNode = null) {
    const nodes = changingNode ? [changingNode] : path.map((id) => nodeById.get(id))
      .filter((node) => node.k === KIND_ATTRIBUTE);
    const choices = new Map();
    const current = changingNode ? treeState.attributeSelections?.[changingNode.id] : undefined;
    if (current !== undefined) choices.set(changingNode.id, current);
    const request = { path, changingNode, choices, current, expectedRegretCost: 0 };
    attributeRequest = request;
    attributeTitle.textContent = changingNode ? "능력치 변경" : "경로에 할당할 능력치 선택";
    attributeList.textContent = "";

    function updateApply() {
      const ready = choices.size === nodes.length;
      btnAttributeApply.disabled = !ready;
      if (changingNode && ready) {
        const cost = rules.attributeChangeCost(treeState, changingNode.id, choices.get(changingNode.id),
          Boolean(panelData?.freePassiveRefund));
        request.expectedRegretCost = cost;
        btnAttributeApply.textContent = cost ? `변경 (후회의 오브 ${cost}개)` : "변경 (무료)";
      } else {
        btnAttributeApply.textContent = "할당";
      }
    }

    for (const [index, node] of nodes.entries()) {
      const group = document.createElement("fieldset");
      group.className = "tree-attribute-choice";
      const title = document.createElement("legend");
      title.textContent = nodes.length === 1 ? localizedName(node)
        : `${index + 1}. ${localizedName(node)} (#${node.id})`;
      group.appendChild(title);
      for (const option of node.attrOptions) {
        const label = document.createElement("label");
        const input = document.createElement("input");
        input.type = "radio";
        input.name = `tree-attribute-${node.id}`;
        input.checked = option.id === choices.get(node.id);
        input.onchange = () => {
          choices.set(node.id, option.id);
          updateApply();
        };
        label.appendChild(input);
        label.append(localizedStats(option).join(" / ") || localizedName(option));
        group.appendChild(label);
      }
      attributeList.appendChild(group);
    }
    updateApply();
    attributeModal.classList.remove("hidden");
    tooltipEl.classList.add("hidden");
  }

  btnAttributeClose.onclick = closeAttributeModal;
  btnAttributeApply.onclick = () => {
    if (!attributeRequest || btnAttributeApply.disabled) return;
    const { path, changingNode, choices, current, expectedRegretCost } = attributeRequest;
    closeAttributeModal();
    if (changingNode) {
      if (treeState.attributeSelections?.[changingNode.id] !== current) return;
      window.poeSendCommand({
        op: "treeSetAttribute", reqId: window.poeNextReqId(),
        nodeId: changingNode.id, optionId: choices.get(changingNode.id), expectedRegretCost,
      });
    } else {
      window.poeSendCommand({
        op: "treeAllocatePath", reqId: window.poeNextReqId(), nodes: path,
        attributeChoices: [...choices].map(([nodeId, optionId]) => ({ nodeId, optionId })),
      });
    }
  };

  // ---- 검색 (한글/영문 + SimpleIME 연동) ----
  function runSearch(query) {
    searchQuery = (query || "").trim().toLowerCase();
    searchMatches = [];
    searchMatchSet = new Set();
    searchFocusIdx = -1;
    if (searchQuery.length > 0) {
      const nodeId = /^#\d+$/.test(searchQuery) ? Number(searchQuery.slice(1)) : null;
      for (const n of TREE.nodes) {
        const view = n.variants ? rules.resolveNode(n, treeState) : n;
        const text = view === n ? searchText.get(n.id)
          : [view.name, view.nameKo, ...(view.stats || []), ...(view.statsKo || [])].join(" ").toLowerCase();
        if (nodeId === null ? text.includes(searchQuery) : n.id === nodeId) {
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

  // 재검수 R-7: 회수·교체 비용은 C++(PassiveTree::GetRefundNodeCost / GetSelectMasterySwapCost)가 계산해 보낸 값을 쓴다.
  function validRefundCost(value) {
    return Number.isSafeInteger(value) && value >= 0 ? value : null;
  }
  function refundCostFor(nodeId) {
    return validRefundCost(treeState?.refundNodeCosts?.[nodeId]);
  }
  function refundBlockMessage(nodeId) {
    const blockedBy = treeState?.refundNodeCosts?.[nodeId]?.blockedBy;
    if (!blockedBy?.length) return "";
    const names = blockedBy.map((id) => {
      const node = nodeById.get(id);
      return node ? `${localizedName(rules.resolveNode(node, treeState))} (#${id})` : `#${id}`;
    });
    return `먼저 회수할 노드 · ${names.join(", ")}`;
  }
  function requestRefund(command, cost, label) {
    if (cost === null) {
      window.poeShowToast?.(false, "회수 비용을 확인할 수 없습니다. 트리를 새로 고쳐 주세요.");
      return;
    }
    const key = `${command.op}:${command.nodeId ?? command.ascendancyOnly}`;
    confirmOrProceedRefund(key, cost, label, () => {
      window.poeSendCommand({ ...command, reqId: window.poeNextReqId(), expectedRegretCost: cost });
      if (command.nodeId === openMasteryNodeId) masteryModal.classList.add("hidden");
    });
  }
  function masterySwapCostFor(nodeId) {
    const costs = (treeState && treeState.masterySwapCosts) || null;
    if (!costs) return null;
    const v = costs[String(nodeId)];
    return (v === undefined || v === null) ? null : Number(v);
  }

  function confirmOrProceedRefund(confirmKey, cost, label, onConfirm) {
    const freeRefund = Boolean(panelData && panelData.freePassiveRefund);
    if (freeRefund || cost === 0) {
      pendingRefundConfirm = null;
      onConfirm();
      return;
    }
    const now = Date.now();
    // Codex 지적 21: 승인은 비용까지 묶는다. 그사이 상태가 바뀌어 비용이 달라졌으면 다시 확인을 받는다.
    confirmKey = `${confirmKey}|cost=${cost === null || cost === undefined ? "?" : cost}`;
    if (pendingRefundConfirm && pendingRefundConfirm.key === confirmKey && now <= pendingRefundConfirm.expiresAt) {
      pendingRefundConfirm = null;
      onConfirm();
      return;
    }
    pendingRefundConfirm = { key: confirmKey, expiresAt: now + 3500 };
    if (window.poeShowToast) {
      const costText = (cost === null || cost === undefined) ? "후회의 오브 소모" : `후회의 오브 ${cost}개 소모`;
      window.poeShowToast(false, `[확인] ${label}: ${costText} — 3.5초 안에 다시 클릭하면 확정합니다.`);
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
    const treeNotLoaded = treeState.loaded === false || (treeState.classSelected && !rules.canUseTree(treeState));
    let errBanner = canvasWrap ? canvasWrap.querySelector(".tree-error-banner") : null;
    if (treeNotLoaded && canvasWrap) {
      if (!errBanner) {
        errBanner = document.createElement("div");
        errBanner.className = "tree-error-banner";
        errBanner.style.cssText = "position:absolute;inset:0;display:flex;align-items:center;justify-content:center;background:rgba(12,10,14,0.82);color:#f5c06f;font-size:15px;padding:24px;text-align:center;z-index:20;";
        errBanner.textContent = "게임의 패시브 트리 데이터와 화면을 동기화하지 못했습니다.";
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

    btnChangeClass.disabled = treeState.classSelected &&
      (allocatedSet.size > 0 || Object.keys(treeState.masterySelections || {}).length > 0);

    // 전직 드롭다운 갱신
    ascSelectEl.textContent = "";
    const optEmpty = document.createElement("option");
    optEmpty.value = "";
    optEmpty.textContent = cls ? "전직 선택..." : "클래스 먼저 선택";
    ascSelectEl.appendChild(optEmpty);
    if (cls) {
      for (const a of cls.ascendancies || []) {
        for (const entry of [a, ...(a.replacements || [])]) {
          const opt = document.createElement("option");
          opt.value = entry.id;
          opt.textContent = `${localizedName(entry)} (${entry.name})${entry === a ? "" : ` · ${localizedName(a)} 대체`}`;
          ascSelectEl.appendChild(opt);
        }
      }
      ascSelectEl.value = treeState.ascendancyId || "";
    }
    const hasAscNodes = (treeState.allocatedNodes || []).some((id) => {
      const node = nodeById.get(id);
      return node && rules.ascendancyOf(node, treeState);
    });
    ascSelectEl.disabled = !cls || (treeState.earnedAscendancyPoints || 0) < 2 || hasAscNodes;
    btnFocusAsc.disabled = !asc;
    btnClearAsc.disabled = !asc || hasAscNodes;

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
        ["주문 회피", `${s.spellDodge ?? 0}%`],
        ["편향", s.deflectionRating ?? 0],
        ["화염/냉기/번개 관통", `${s.penetrationPct?.fire ?? 0}% / ${s.penetrationPct?.cold ?? 0}% / ${s.penetrationPct?.lightning ?? 0}%`],
        ["카오스 관통", `${s.penetrationPct?.chaos ?? 0}%`],
        ["공격 원소 관통", `${s.attackElementalPenetrationPct ?? 0}%`],
        ["이동 속도 증가", s.movementSpeedApplied ? `${s.incMovementSpeedPct ?? 0}%` : "세대 1부터"],
        ["마나 우선 피해 흡수", `${s.momRatio ?? 0}%`],
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
        activeKs.push(localizedName(rules.resolveNode(n, treeState)));
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

    // 1~4차 보스·결투장 이정표 및 아틀라스 진행도
    treeMilestonesBox.textContent = "";
    const mask = treeState.ascendancyMilestonesBitmask || 0;
    const bKills = treeState.bossKillCount || 0;
    const dKills = treeState.dragonKillCount || 0;
    const atlasPts = treeState.atlasTierPoints || 0;
    const rewards = Array.isArray(treeState.spiritMilestoneRewards) ? treeState.spiritMilestoneRewards : [];
    const tiers = [
      { goal: "드래곤 1 / 보스 5", cur: `용 ${dKills}/1 · 보스 ${bKills}/5` },
      { goal: "드래곤 3 / 보스 15", cur: `용 ${dKills}/3 · 보스 ${bKills}/15` },
      { goal: "드래곤 6 / 보스 30", cur: `용 ${dKills}/6 · 보스 ${bKills}/30` },
      { goal: "4차 결투장 정복", cur: "지도 장치 [U]" },
    ];
    for (const [index, t] of tiers.entries()) {
      const row = document.createElement("div");
      const done = (mask & (1 << index)) !== 0;
      row.className = `ms-row${done ? " done" : ""}`;
      const l = document.createElement("span");
      const spirit = Number.isInteger(rewards[index]) ? ` · +${rewards[index]} 정신력` : "";
      l.textContent = `${index + 1}차 (+2전직 · +2P${spirit}): ${t.goal}`;
      const r = document.createElement("span");
      r.textContent = done ? "달성" : t.cur;
      row.appendChild(l);
      row.appendChild(r);
      treeMilestonesBox.appendChild(row);
    }
    const atlasRow = document.createElement("div");
    atlasRow.className = `ms-row${atlasPts >= 16 ? " done" : ""}`;
    const al = document.createElement("span");
    al.textContent = "아틀라스 첫 클리어 (T1~T16)";
    const ar = document.createElement("span");
    ar.textContent = `+${atlasPts}/16P`;
    atlasRow.appendChild(al);
    atlasRow.appendChild(ar);
    treeMilestonesBox.appendChild(atlasRow);
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
    requestRefund({ op: "treeRefundAll", ascendancyOnly: true },
      validRefundCost(treeState.refundAscCost), "전직 전체 회수");
  };
  btnRefundAll.onclick = () => {
    requestRefund({ op: "treeRefundAll", ascendancyOnly: false },
      validRefundCost(treeState.refundAllCost), "패시브 전체 회수");
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
    const jewels = bagItems.filter((b) => b.isJewel && (!rules.isPhylactery(node.id) || b.rarity !== "unique"));
    if (jewels.length === 0) {
      const empty = document.createElement("div");
      empty.className = "section-sub";
      empty.textContent = "가방에 장착할 수 있는 주얼이 없습니다.";
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
      requestRefund({ op: "treeRefundNode", nodeId: targetId }, refundCostFor(targetId), "마스터리 회수");
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
      !attributeModal.classList.contains("hidden") ||
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
    if (!rules.canUseTree(treeState)) return;
    if (hasMasteries && hit.k === KIND_MASTERY) {
      if (isMasteryUnlocked(hit) || (treeState.masterySelections || {})[String(hit.id)] !== undefined) {
        openMasteryModal(hit);
      } else if (window.poeShowToast) {
        window.poeShowToast(false, "같은 그룹의 주요 패시브를 먼저 1개 이상 할당해야 합니다.");
      }
      return;
    }
    if (rules.isJewelSocket(hit) && allocatedSet.has(hit.id)) {
      openJewelModal(hit);
      return;
    }
    if (hit.k === KIND_ATTRIBUTE && allocatedSet.has(hit.id)) {
      openAttributeModal([], hit);
      return;
    }
    const path = computeShortestPath(hit.id);
    if (path.length > 0) {
      if (path.some((id) => nodeById.get(id).k === KIND_ATTRIBUTE)) {
        openAttributeModal(path);
        return;
      }
      window.poeSendCommand({
        op: "treeAllocatePath",
        reqId: window.poeNextReqId(),
        nodes: path,
      });
    }
  });

  canvas.addEventListener("contextmenu", (e) => {
    e.preventDefault();
    if (!isActive || !rules.canUseTree(treeState)) return;
    const rect = canvas.getBoundingClientRect();
    const world = screenCssToWorld(e.clientX - rect.left, e.clientY - rect.top);
    const hit = findNodeAtWorld(world.x, world.y);
    if (!hit) return;

    const hasMastery = hasMasteries && hit.k === KIND_MASTERY && treeState.masterySelections?.[hit.id] !== undefined;
    if (allocatedSet.has(hit.id) || hasMastery) {
      const blocked = refundBlockMessage(hit.id);
      if (blocked) {
        window.poeShowToast?.(false, blocked);
        return;
      }
      const label = `${localizedName(rules.resolveNode(hit, treeState)) || ("#" + hit.id)} 회수`;
      requestRefund({ op: "treeRefundNode", nodeId: hit.id }, refundCostFor(hit.id), label);
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
    rules,
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
        closeAttributeModal();
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
        const prevAscendancyId = treeState.ascendancyId;
        treeState = data.tree;
        allocatedSet = new Set(treeState.allocatedNodes || []);
        lockedNodes.clear();
        for (const node of conditionalNodes) {
          if (!rules.requirementsMet(node, allocatedSet)) lockedNodes.add(node.id);
        }
        if (searchQuery && (prevClassId !== treeState.classId || prevAscendancyId !== treeState.ascendancyId ||
            wasSelected !== treeState.classSelected)) runSearch(searchQuery);
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
