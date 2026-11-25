(() => {
  "use strict";

  document.addEventListener("DOMContentLoaded", () => {
    const rules = window.poeTreeModule.rules;
    const options = new URLSearchParams(location.search);
    const jewel = {
      isJewel: true, baseFormId: 1, uniqueId: 1, jewelColor: "crimson", rarity: "magic",
      title: "진홍색 주얼", card: { title: "진홍색 주얼", rarityBadge: "[마법]", affixLines: [{ textKo: "힘 +5" }] },
    };
    const settings = {
      freeRefund: options.get("freeRefund") === "1", ascendancyRefundCost: 5, regretOrbCount: 100,
    };
    const tree = {
      loaded: true, generation: rules.generation, classSelected: false,
      classId: 0, classContentId: "", ascendancyId: "", allocatedNodes: [],
      attributeSelections: {}, masterySelections: {}, socketedJewels: {},
      earnedAscendancyPoints: 8, ascendancyMilestonesBitmask: 15,
      bossKillCount: 30, dragonKillCount: 6, atlasTierPoints: 16,
    };
    const exileLevel = 100;
    const pointSettings = {
      exileLevel, atlasCompletedTierMask: 0xffff, bonusPassivePoints: 0, bonusAscendancyPoints: 0,
    };
    let activeTab = "tree";

    function updatePanel() {
      Object.assign(tree, rules.pointTotals(tree, pointSettings));
      tree.regretOrbCount = settings.regretOrbCount;
      tree.refundAscCost = rules.refundAllCost(tree, true, settings);
      tree.refundAllCost = rules.refundAllCost(tree, false, settings);
      const refundable = [...tree.allocatedNodes, ...Object.keys(tree.masterySelections).map(Number)];
      tree.refundNodeCosts = Object.fromEntries(refundable.map((id) => {
        const blockedBy = rules.refundDependents(tree, id);
        return [id, blockedBy.length ? { blockedBy } : rules.refundNodeCost(tree, id, settings)];
      }));
      tree.masterySwapCosts = Object.fromEntries(Object.keys(tree.masterySelections)
        .map((id) => [id, settings.freeRefund ? 0 : 1]));
      const cls = tree.classSelected ? rules.classById.get(tree.classId) : null;
      window.poeUpdatePanel(JSON.stringify({
        activeTab, exileLevel, exileXp: 0, exileReqXp: 0,
        className: cls ? cls.nameKo || cls.name : "미선택",
        unallocatedPoints: tree.availableNormalPoints, freePassiveRefund: settings.freeRefund, tree,
        gear: { loaded: true, bagItems: Object.keys(tree.socketedJewels).length ? [] : [jewel] },
        stats: {
          str: (cls?.baseStr ?? 0) + Object.keys(tree.socketedJewels).reduce(
            (sum, id) => sum + 5 * rules.jewelEffectMultiplier(tree, Number(id)), 0),
          dex: cls?.baseDex ?? 0, intel: cls?.baseInt ?? 0, movementSpeedApplied: rules.generation === 1,
          poeMaxLife: 1000, poeMaxMana: 300, esCurrent: 0, esMax: 0, armour: 0, evasion: 0,
          fireResist: 0, coldResist: 0, lightningResist: 0, chaosResist: 0, physDR: 0, elemDR: 0,
        },
      }));
    }

    function selectClass(cmd) {
      if (tree.allocatedNodes.length || Object.keys(tree.masterySelections).length) return null;
      const cls = typeof cmd.classContentId === "string"
        ? [...rules.classById.values()].find((entry) => (entry.contentId || `cls.${entry.name}`) === cmd.classContentId)
        : rules.classById.get(cmd.classId);
      return cls ? {
        classSelected: true, classId: cls.id, classContentId: cls.contentId || `cls.${cls.name}`,
        ascendancyId: "", attributeSelections: {},
      } : null;
    }

    function selectAscendancy(id) {
      if (!rules.canUseTree(tree)) return null;
      if (id) {
        const asc = rules.ascById.get(id);
        if (!asc || asc.classId !== tree.classId || tree.earnedAscendancyPoints < 2) return null;
      }
      if (tree.ascendancyId !== id && tree.allocatedNodes.some((nodeId) =>
        rules.ascendancyOf(rules.nodeById.get(nodeId), tree))) return null;
      return { ascendancyId: id };
    }

    function selectMastery(nodeId, effectId) {
      const node = rules.nodeById.get(nodeId);
      if (!rules.isMasteryUnlocked(node, tree, new Set(tree.allocatedNodes)) || !node.effects?.includes(effectId)) return null;
      const current = tree.masterySelections[nodeId];
      if (current === effectId) return {};
      if (Object.values(tree.masterySelections).includes(effectId) ||
          (current === undefined && tree.availableNormalPoints < 1)) return null;
      const cost = current !== undefined && !settings.freeRefund ? 1 : 0;
      if (settings.regretOrbCount < cost) return null;
      return { masterySelections: { ...tree.masterySelections, [nodeId]: effectId }, cost };
    }

    function command(cmd) {
      switch (cmd.op) {
        case "treeSelectClass": return selectClass(cmd);
        case "treeSelectAscendancy": return selectAscendancy(cmd.ascendancyId);
        case "treeClearAscendancy": return selectAscendancy("");
        case "treeAllocatePath":
          return rules.planAllocation(tree, cmd.nodes, new Map((cmd.attributeChoices || []).map(
            ({ nodeId, optionId }) => [nodeId, optionId])));
        case "treeSetAttribute": {
          const result = rules.planAttributeChange(tree, cmd.nodeId, cmd.optionId, settings);
          return result && cmd.expectedRegretCost === result.cost ? result : null;
        }
        case "treeRefundNode": {
          const result = rules.planRefund(tree, cmd.nodeId, settings);
          return result && cmd.expectedRegretCost === result.cost ? result : null;
        }
        case "treeRefundAll": {
          if (typeof cmd.ascendancyOnly !== "boolean") return null;
          const result = rules.planRefundAll(tree, cmd.ascendancyOnly, settings);
          return result && cmd.expectedRegretCost === result.cost ? result : null;
        }
        case "treeSelectMastery": return selectMastery(cmd.masteryNodeId, cmd.effectId);
        case "treeSocketJewel":
          if (!rules.isAllocatedJewelSocket(tree, cmd.nodeId) || Object.keys(tree.socketedJewels).length ||
              cmd.baseFormId !== jewel.baseFormId || cmd.uniqueId !== jewel.uniqueId) return null;
          return { socketedJewels: { [cmd.nodeId]: jewel } };
        case "treeUnsocketJewel":
          return tree.socketedJewels[cmd.nodeId] ? { socketedJewels: {} } : null;
        default: return null;
      }
    }

    window.poeCommand = (message) => {
      const cmd = JSON.parse(message);
      if (cmd.op === "close") {
        window.poeOnPanelClose();
        return;
      }
      if (cmd.op === "treeSearchFocus") return;
      if (cmd.op === "refresh" || cmd.op === "switchTab") {
        if (cmd.op === "switchTab") activeTab = cmd.tab;
        updatePanel();
        return;
      }
      const result = command(cmd);
      if (result) {
        const { cost = 0, ...changes } = result;
        Object.assign(tree, changes);
        settings.regretOrbCount -= cost;
        for (const id of Object.keys(tree.socketedJewels)) {
          if (!rules.isAllocatedJewelSocket(tree, Number(id))) delete tree.socketedJewels[id];
        }
      }
      window.poeToast(JSON.stringify({
        ok: Boolean(result),
        message: result ? "패시브 트리를 갱신했습니다." : "할당 조건·연결·가용 포인트 또는 후회의 오브를 확인하세요.",
      }));
      updatePanel();
    };

    const canvas = document.getElementById("tree-canvas");
    const raf = window.requestAnimationFrame.bind(window);
    const nextFrame = () => new Promise((resolve) => raf(resolve));
    let measuring = false;

    function summary(samples) {
      const slowest = [...samples].sort((a, b) => b - a).slice(0, Math.ceil(samples.length * 0.01));
      const mean = (xs) => xs.reduce((sum, x) => sum + x, 0) / xs.length;
      return { frames: samples.length, meanMs: mean(samples), slowest1PctMs: mean(slowest) };
    }

    async function measure() {
      if (measuring) throw new Error("프레임 측정 중입니다.");
      if (activeTab !== "tree" || !tree.classSelected || !canvas.clientWidth) {
        throw new Error("클래스를 선택한 뒤 패시브 트리에서 측정하세요.");
      }
      measuring = true;
      const originalRaf = window.requestAnimationFrame;
      const drawTimes = [];
      let phase = "warmup";
      let idleRedraws = 0;
      window.requestAnimationFrame = (callback) => raf((time) => {
        const start = performance.now();
        callback(time);
        if (phase === "moving") drawTimes.push(performance.now() - start);
        if (phase === "idle") idleRedraws++;
      });
      const rect = canvas.getBoundingClientRect();
      const x = rect.left + rect.width / 2;
      const y = rect.top + rect.height / 2;
      const mouse = (target, type, dx = 0, dy = 0) => target.dispatchEvent(new MouseEvent(type, {
        bubbles: true, clientX: x + dx, clientY: y + dy, button: 0,
      }));
      async function sample(count, step = () => {}) {
        const intervals = [];
        let previous = await nextFrame();
        for (let frame = 0; frame < count; frame++) {
          step(frame);
          const now = await nextFrame();
          intervals.push(now - previous);
          previous = now;
        }
        return summary(intervals);
      }

      try {
        document.getElementById("btn-tree-center").click();
        await sample(30);
        phase = "idle";
        const idle = await sample(240);
        phase = "moving";
        mouse(canvas, "mousedown");
        const pan = await sample(120, (i) => {
          const angle = i / 120 * 2 * Math.PI;
          mouse(window, "mousemove", rect.width * 0.32 * Math.sin(angle), rect.height * 0.33 * (1 - Math.cos(angle)));
        });
        mouse(window, "mouseup");
        const zoom = await sample(120, (i) => {
          if (i % 2 === 0) canvas.dispatchEvent(new WheelEvent("wheel", {
            bubbles: true, cancelable: true, clientX: x, clientY: y,
            deltaY: i < 30 || i >= 90 ? 100 : -100,
          }));
        });
        const extra = (row) => ({
          meanMs: row.meanMs - idle.meanMs, slowest1PctMs: row.slowest1PctMs - idle.slowest1PctMs,
        });
        const result = {
          canvas: { width: canvas.width, height: canvas.height }, idle, idleRedraws, pan, zoom,
          panExtra: extra(pan), zoomExtra: extra(zoom), drawCallbacks: summary(drawTimes),
        };
        console.log("SkyrimPoE frame timings (ms)", result);
        return result;
      } finally {
        window.requestAnimationFrame = originalRaf;
        measuring = false;
      }
    }

    window.poePreview = { measure };
    window.poeOnPanelOpen(JSON.stringify({ tab: activeTab }));
    if (options.get("benchmark") === "1") {
      window.poeSendCommand({ op: "treeSelectClass", classContentId: "cls.Ranger" });
      measure().catch(console.error);
    }
  });
})();
