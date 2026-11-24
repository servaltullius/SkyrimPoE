(() => {
  "use strict";

  const hudSpiritChip = document.getElementById("hud-spirit-chip");
  const hudGuardChip = document.getElementById("hud-guard-chip");
  const hudHhChip = document.getElementById("hud-hh-chip");
  const hudMapChip = document.getElementById("hud-map-chip");
  const hudCharmsRow = document.getElementById("hud-charms-row");
  const hudEsRow = document.getElementById("hud-es-row");
  const hudEsText = document.getElementById("hud-es-text");
  const hudEsFill = document.getElementById("hud-es-fill");
  const hudLevel = document.getElementById("hud-level");
  const hudXpText = document.getElementById("hud-xp-text");
  const hudXpFill = document.getElementById("hud-xp-fill");
  const hudPtsBadge = document.getElementById("hud-pts-badge");
  const skillsSpiritBadge = document.getElementById("skills-spirit-badge");

  const panelBackdrop = document.getElementById("panel-backdrop");
  const panelHeaderMeta = document.getElementById("panel-header-meta");
  const panelToast = document.getElementById("panel-toast");
  const tabBtnSkills = document.getElementById("tab-btn-skills");
  const tabBtnTree = document.getElementById("tab-btn-tree");
  const tabBtnGear = document.getElementById("tab-btn-gear");
  const tabBtnMaps = document.getElementById("tab-btn-maps");
  const tabPanelSkills = document.getElementById("tab-panel-skills");
  const tabPanelTree = document.getElementById("tab-panel-tree");
  const tabPanelGear = document.getElementById("tab-panel-gear");
  const tabPanelMaps = document.getElementById("tab-panel-maps");
  const targetIndicator = document.getElementById("target-indicator");
  const slotsList = document.getElementById("slots-list");
  const inventoryList = document.getElementById("inventory-list");
  const statsGrid = document.getElementById("stats-grid");
  const btnClose = document.getElementById("btn-close");

  // 검수 M4: app.js와 tree.js가 단일 공용 reqId 카운터를 공유해 탭 간 요청 번호 충돌을 방지한다.
  let sharedReqId = 1;
  window.poeNextReqId = () => sharedReqId++;
  window.poeSendCommand = (payload) => {
    if (typeof window.poeCommand === "function") {
      window.poeCommand(JSON.stringify(payload));
    }
  };

  let activeTab = "skills";
  let lastPanelData = null;
  let lastManualTabSwitchAt = 0;
  let selectedTarget = null; // { mode: "equip"|"socket", slot: number, spellIdx: number, label: string }

  function sendCommand(payload) {
    window.poeSendCommand(payload);
  }

  function applyActiveTab(tab) {
    activeTab = tab === "tree" ? "tree" : (tab === "gear" ? "gear" : (tab === "maps" ? "maps" : "skills"));
    const isSkills = activeTab === "skills";
    const isTree = activeTab === "tree";
    const isGear = activeTab === "gear";
    const isMaps = activeTab === "maps";
    const visible = !panelBackdrop.classList.contains("hidden");

    tabBtnSkills.classList.toggle("active", isSkills);
    tabBtnTree.classList.toggle("active", isTree);
    if (tabBtnGear) tabBtnGear.classList.toggle("active", isGear);
    if (tabBtnMaps) tabBtnMaps.classList.toggle("active", isMaps);
    tabPanelSkills.classList.toggle("hidden", !isSkills);
    tabPanelTree.classList.toggle("hidden", !isTree);
    if (tabPanelGear) tabPanelGear.classList.toggle("hidden", !isGear);
    if (tabPanelMaps) tabPanelMaps.classList.toggle("hidden", !isMaps);

    if (window.poeTreeModule && typeof window.poeTreeModule.setActive === "function") {
      window.poeTreeModule.setActive(isTree && visible);
    }
    if (window.poeGearModule && typeof window.poeGearModule.setActive === "function") {
      window.poeGearModule.setActive(isGear && visible);
    }
    if (window.poeMapsModule && typeof window.poeMapsModule.setActive === "function") {
      window.poeMapsModule.setActive(isMaps && visible);
    }
  }

  tabBtnSkills.onclick = () => {
    lastManualTabSwitchAt = Date.now();
    applyActiveTab("skills");
    sendCommand({ op: "switchTab", tab: "skills" });
  };

  tabBtnTree.onclick = () => {
    lastManualTabSwitchAt = Date.now();
    applyActiveTab("tree");
    sendCommand({ op: "switchTab", tab: "tree" });
  };

  if (tabBtnGear) {
    tabBtnGear.onclick = () => {
      lastManualTabSwitchAt = Date.now();
      applyActiveTab("gear");
      sendCommand({ op: "switchTab", tab: "gear" });
    };
  }

  if (tabBtnMaps) {
    tabBtnMaps.onclick = () => {
      lastManualTabSwitchAt = Date.now();
      applyActiveTab("maps");
      sendCommand({ op: "switchTab", tab: "maps" });
    };
  }

  function kindBadge(kind) {
    switch (kind) {
      case "attack": return { text: "공격", cls: "badge-attack" };
      case "trigger": return { text: "발동", cls: "badge-trigger" };
      case "spell": return { text: "주문", cls: "badge-spell" };
      case "support": return { text: "보조", cls: "badge-support" };
      case "aura": return { text: "오라", cls: "badge-aura" };
      case "herald": return { text: "전령", cls: "badge-herald" };
      case "guard": return { text: "가드", cls: "badge-guard" };
      default: return { text: "젬", cls: "" };
    }
  }

  function el(tag, className, text) {
    const node = document.createElement(tag);
    if (className) node.className = className;
    if (text !== undefined && text !== null) node.textContent = String(text);
    return node;
  }

  function revalidateSelectedTarget(data) {
    if (!selectedTarget) return;
    const slots = (data && data.slots) || [];
    const slot = slots[selectedTarget.slot];
    if (!slot) {
      selectedTarget = null;
      return;
    }
    if (selectedTarget.mode === "equip") {
      if (slot.occupied) {
        selectedTarget = null;
      }
      return;
    }
    if (!slot.occupied || !slot.gem) {
      selectedTarget = null;
      return;
    }
    if (selectedTarget.spellIdx < 0) {
      const used = (slot.gem.socketedGems || []).length;
      if (used >= slot.gem.socketCount) {
        selectedTarget = null;
      }
      return;
    }
    const spells = slot.gem.socketedGems || [];
    if (selectedTarget.spellIdx >= spells.length) {
      selectedTarget = null;
      return;
    }
    const sp = spells[selectedTarget.spellIdx];
    if ((sp.socketedGems || []).length >= sp.socketCount) {
      selectedTarget = null;
    }
  }

  function updateTargetIndicator() {
    if (!selectedTarget) {
      targetIndicator.textContent = "장착 대상을 먼저 선택하세요";
    } else {
      targetIndicator.textContent = `선택됨: ${selectedTarget.label} (오른쪽 인벤토리 젬 클릭)`;
    }
  }

  function renderSlots(data) {
    slotsList.textContent = "";
    if (skillsSpiritBadge && data && data.stats) {
      skillsSpiritBadge.textContent = `정신력 ${data.stats.spiritReserved ?? 0} / ${data.stats.spiritMax ?? 100}`;
    }
    const slots = data.slots || [];
    const attackSlots = slots.filter(s => s.occupied && s.gem && s.gem.kind === "attack");

    for (const slot of slots) {
      const card = el("div", "slot-card");
      if (selectedTarget && selectedTarget.slot === slot.slotIndex && selectedTarget.spellIdx < 0) {
        card.classList.add("selected");
      }

      const top = el("div", "slot-top");
      top.appendChild(el("span", "slot-num", `칸 ${slot.slotIndex + 1}`));

      if (!slot.occupied || !slot.gem) {
        top.appendChild(el("span", "section-sub", "(비어 있음)"));
        const actions = el("div", "slot-actions");
        const btnSelect = el("button", "btn btn-primary", "+ 스킬 장착");
        btnSelect.type = "button";
        btnSelect.onclick = () => {
          selectedTarget = { mode: "equip", slot: slot.slotIndex, spellIdx: -1, label: `칸 ${slot.slotIndex + 1} 스킬 장착` };
          updateTargetIndicator();
          renderSlots(data);
        };
        actions.appendChild(btnSelect);
        top.appendChild(actions);
        card.appendChild(top);
        slotsList.appendChild(card);
        continue;
      }

      const gem = slot.gem;
      const kb = kindBadge(gem.kind);
      top.appendChild(el("span", `badge ${kb.cls}`, kb.text));
      top.appendChild(el("span", "gem-name", `${gem.name} Lv.${gem.level}`));
      if (gem.kind !== "aura" && gem.kind !== "herald") {
        top.appendChild(el("span", "section-sub", `소켓 ${(gem.socketedGems || []).length}/${gem.socketCount}`));
      }

      const actions = el("div", "slot-actions");
      if (gem.kind === "aura" || gem.kind === "herald") {
        const isOn = Boolean(slot.auraActive);
        const isRequested = Boolean(slot.auraRequested);
        const sCost = (slot.computed && slot.computed.spiritCost) ? slot.computed.spiritCost : 0;
        let auraCls = "btn btn-primary";
        let auraLabel = `활성 OFF (-${sCost})`;
        if (isOn) {
          auraCls = "btn btn-aura-on";
          auraLabel = `활성 ON (-${sCost})`;
        } else if (isRequested) {
          auraCls = "btn btn-danger";
          auraLabel = `정신력 부족 · 대기 해제 (-${sCost})`;
        }
        const btnAura = el("button", auraCls, auraLabel);
        btnAura.type = "button";
        btnAura.onclick = () => {
          sendCommand({ op: "toggleAura", reqId: window.poeNextReqId(), slot: slot.slotIndex });
        };
        actions.appendChild(btnAura);
      } else if (gem.kind === "spell") {
        actions.appendChild(el("span", "badge badge-spell", `주문 칸 ${slot.slotIndex + 1}`));
      } else if (gem.kind === "guard") {
        actions.appendChild(el("span", "badge badge-guard", `파워 칸 ${slot.slotIndex + 1}`));
      }

      if ((gem.socketedGems || []).length < gem.socketCount && gem.kind !== "aura" && gem.kind !== "herald") {
        const subLabel = gem.kind === "trigger" ? "+ 주문 담기" : "+ 보조 장착";
        const btnAddSub = el("button", "btn btn-primary", subLabel);
        btnAddSub.type = "button";
        btnAddSub.onclick = () => {
          selectedTarget = {
            mode: "socket",
            slot: slot.slotIndex,
            spellIdx: -1,
            label: `칸 ${slot.slotIndex + 1} ${gem.name} ${gem.kind === "trigger" ? "주문 소켓" : "보조 소켓"}`
          };
          updateTargetIndicator();
          renderSlots(data);
        };
        actions.appendChild(btnAddSub);
      }

      const btnUnequip = el("button", "btn btn-danger", "해제");
      btnUnequip.type = "button";
      btnUnequip.onclick = () => {
        selectedTarget = null;
        updateTargetIndicator();
        sendCommand({ op: "unequip", reqId: window.poeNextReqId(), slot: slot.slotIndex });
      };
      actions.appendChild(btnUnequip);
      top.appendChild(actions);
      card.appendChild(top);

      if (slot.computed) {
        const c = slot.computed;
        let line = "";
        if (gem.kind === "aura" || gem.kind === "herald") {
          line = `정신력 예약 ${c.spiritCost ?? 0} · 상태 ${slot.auraActive ? "활성 중" : "꺼짐"}`;
        } else {
          line = `평균 PoE 피해 ${c.avgDamage} · 치명타 ${c.critPct}% · 마나 ${c.manaCost} · 반경 ×${c.areaMult}`;
          const extras = [];
          if ((c.projectileCount ?? 1) > 1) extras.push(`투사체 ${c.projectileCount}`);
          if ((c.pierceCount ?? 0) > 0) extras.push(`관통 +${c.pierceCount}`);
          if (c.forkEnabled) extras.push("분기");
          if ((c.chainCount ?? 0) > 0) extras.push(`연쇄 +${c.chainCount}`);
          if ((c.spellRepeatCount ?? 0) > 0) extras.push(`반복 +${c.spellRepeatCount}`);
          if (extras.length > 0) line += ` · ${extras.join("/")}`;
        }
        card.appendChild(el("div", "slot-stats-line", line));
      }

      if (gem.kind === "trigger") {
        const linkRow = el("div", "link-row");
        linkRow.appendChild(el("span", "", "연결 공격 칸:"));
        const sel = document.createElement("select");
        sel.className = "link-select";
        const optNone = document.createElement("option");
        optNone.value = "-1";
        optNone.textContent = "(연결 안 함)";
        sel.appendChild(optNone);
        for (const atk of attackSlots) {
          const opt = document.createElement("option");
          opt.value = String(atk.slotIndex);
          opt.textContent = `칸 ${atk.slotIndex + 1} — ${atk.gem.name} Lv.${atk.gem.level}`;
          sel.appendChild(opt);
        }
        sel.value = String(slot.linkedAttackSlot);
        sel.onchange = () => {
          sendCommand({ op: "link", reqId: window.poeNextReqId(), slot: slot.slotIndex, attackSlot: parseInt(sel.value, 10) });
        };
        linkRow.appendChild(sel);
        card.appendChild(linkRow);
      }

      const children = gem.socketedGems || [];
      if (children.length > 0) {
        const box = el("div", "sockets-box");
        children.forEach((child, idx) => {
          if (gem.kind === "trigger") {
            const spCard = el("div", "nested-spell-card");
            if (selectedTarget && selectedTarget.slot === slot.slotIndex && selectedTarget.spellIdx === idx) {
              spCard.classList.add("selected");
            }
            const spTop = el("div", "slot-top");
            const ckb = kindBadge(child.kind);
            spTop.appendChild(el("span", `badge ${ckb.cls}`, ckb.text));
            spTop.appendChild(el("span", "gem-name", `${child.name} Lv.${child.level}`));
            spTop.appendChild(el("span", "section-sub", `보조 ${(child.socketedGems || []).length}/${child.socketCount}`));

            const spAct = el("div", "slot-actions");
            if ((child.socketedGems || []).length < child.socketCount) {
              const btnSpSup = el("button", "btn btn-primary", "+ 보조");
              btnSpSup.type = "button";
              btnSpSup.onclick = () => {
                selectedTarget = {
                  mode: "socket",
                  slot: slot.slotIndex,
                  spellIdx: idx,
                  label: `칸 ${slot.slotIndex + 1} > ${child.name} 보조 소켓`
                };
                updateTargetIndicator();
                renderSlots(data);
              };
              spAct.appendChild(btnSpSup);
            }
            const btnRemSp = el("button", "btn btn-danger", "빼기");
            btnRemSp.type = "button";
            btnRemSp.onclick = () => {
              if (selectedTarget && selectedTarget.slot === slot.slotIndex) {
                selectedTarget = null;
                updateTargetIndicator();
              }
              sendCommand({ op: "unsocket", reqId: window.poeNextReqId(), slot: slot.slotIndex, spellIdx: -1, socketIdx: idx });
            };
            spAct.appendChild(btnRemSp);
            spTop.appendChild(spAct);
            spCard.appendChild(spTop);

            if (child.computed) {
              const cc = child.computed;
              spCard.appendChild(el("div", "slot-stats-line",
                `평균 PoE 피해 ${cc.avgDamage} · 치명타 ${cc.critPct}% · 마나 ${cc.manaCost} · 반경 ×${cc.areaMult} · 쿨 ${cc.cooldownSec}s`));
            }

            const nestedSups = child.socketedGems || [];
            if (nestedSups.length > 0) {
              const nbox = el("div", "sockets-box");
              nestedSups.forEach((sup, sIdx) => {
                const sRow = el("div", "socket-row");
                const skb = kindBadge(sup.kind);
                sRow.appendChild(el("span", `badge ${skb.cls}`, skb.text));
                sRow.appendChild(el("span", "", `${sup.name} Lv.${sup.level}`));
                const sAct = el("div", "slot-actions");
                const btnRemSup = el("button", "btn btn-danger", "빼기");
                btnRemSup.type = "button";
                btnRemSup.onclick = () => {
                  if (selectedTarget && selectedTarget.slot === slot.slotIndex) {
                    selectedTarget = null;
                    updateTargetIndicator();
                  }
                  sendCommand({ op: "unsocket", reqId: window.poeNextReqId(), slot: slot.slotIndex, spellIdx: idx, socketIdx: sIdx });
                };
                sAct.appendChild(btnRemSup);
                sRow.appendChild(sAct);
                nbox.appendChild(sRow);
              });
              spCard.appendChild(nbox);
            }
            box.appendChild(spCard);
          } else {
            const row = el("div", "socket-row");
            const ckb = kindBadge(child.kind);
            row.appendChild(el("span", `badge ${ckb.cls}`, ckb.text));
            row.appendChild(el("span", "", `${child.name} Lv.${child.level}`));
            const rAct = el("div", "slot-actions");
            const btnRem = el("button", "btn btn-danger", "빼기");
            btnRem.type = "button";
            btnRem.onclick = () => {
              if (selectedTarget && selectedTarget.slot === slot.slotIndex) {
                selectedTarget = null;
                updateTargetIndicator();
              }
              sendCommand({ op: "unsocket", reqId: window.poeNextReqId(), slot: slot.slotIndex, spellIdx: -1, socketIdx: idx });
            };
            rAct.appendChild(btnRem);
            row.appendChild(rAct);
            box.appendChild(row);
          }
        });
        card.appendChild(box);
      }

      slotsList.appendChild(card);
    }
  }

  function renderInventory(data) {
    inventoryList.textContent = "";
    const items = data.inventory || [];
    if (items.length === 0) {
      inventoryList.appendChild(el("div", "section-sub", "인벤토리에 보관 중인 젬이 없습니다."));
      return;
    }

    for (const item of items) {
      const btn = document.createElement("button");
      btn.type = "button";
      btn.className = "inv-item";

      const kb = kindBadge(item.kind);
      btn.appendChild(el("span", `badge ${kb.cls}`, kb.text));
      if (item.tracked) {
        btn.appendChild(el("span", "badge badge-tracked", "보관"));
      }
      btn.appendChild(el("span", "gem-name", item.name));
      const detail = item.tracked
        ? (item.socketCount > 0
            ? `Lv.${item.level} · 소켓 ${item.socketedCount}/${item.socketCount}`
            : `Lv.${item.level}`)
        : `순정 ×${item.count}`;
      btn.appendChild(el("span", "inv-right", detail));

      btn.onclick = () => {
        if (!selectedTarget) {
          showToast(false, "먼저 왼쪽 스킬 칸에서 장착할 대상을 선택하세요.");
          return;
        }
        if (selectedTarget.mode === "equip") {
          sendCommand({
            op: "equip",
            reqId: window.poeNextReqId(),
            slot: selectedTarget.slot,
            gemId: item.gemId,
            ownerFormId: item.ownerFormId || 0,
            uniqueId: item.uniqueId || 0
          });
        } else {
          sendCommand({
            op: "socket",
            reqId: window.poeNextReqId(),
            slot: selectedTarget.slot,
            spellIdx: selectedTarget.spellIdx,
            gemId: item.gemId,
            ownerFormId: item.ownerFormId || 0,
            uniqueId: item.uniqueId || 0
          });
        }
      };

      inventoryList.appendChild(btn);
    }
  }

  function renderStats(data) {
    statsGrid.textContent = "";
    const s = data.stats;
    if (!s) return;
    const rows = [
      ["힘 / 민첩 / 지능", `${s.str} / ${s.dex} / ${s.intel}`],
      ["PoE 생명력", s.poeMaxLife],
      ["PoE 마나", s.poeMaxMana],
      ["정신력 (예약/최대)", `${s.spiritReserved ?? 0} / ${s.spiritMax ?? 100}`],
      ["에너지 실드", `${s.esCurrent} / ${s.esMax}`],
      ["용암 방패 보호막", `${s.guardShield ?? 0} / ${s.guardMax ?? 0}`],
      ["방어도 / 회피", `${s.armour} / ${s.evasion}`],
      ["물리 / 원소 피해 감소", `${s.physDR}% / ${s.elemDR}%`],
      ["화염 / 냉기 저항", `${s.fireResist}% (${s.maxFireResist ?? 75}%) / ${s.coldResist}% (${s.maxColdResist ?? 75}%)`],
      ["번개 / 카오스 저항", `${s.lightningResist}% (${s.maxLightningResist ?? 75}%) / ${s.chaosResist}% (${s.maxChaosResist ?? 75}%)`]
    ];
    for (const [k, v] of rows) {
      const row = el("div", "stat-row");
      row.appendChild(el("span", "stat-k", k));
      row.appendChild(el("span", "stat-v", v));
      statsGrid.appendChild(row);
    }
  }

  function showToast(ok, message) {
    panelToast.textContent = message || "";
    panelToast.className = `panel-toast ${ok ? "ok" : "err"}`;
  }
  window.poeShowToast = showToast;

  btnClose.onclick = () => sendCommand({ op: "close" });

  // ---- C++ -> JS Interop 핸들러 ----
  // 결정 185: 적 ES 표시. visible=false면 숨긴다. rarity: 0 일반, 1 매직, 2 레어
  const enemyEsRoot = document.getElementById("enemy-es-root");
  const enemyEsName = document.getElementById("enemy-es-name");
  const enemyEsText = document.getElementById("enemy-es-text");
  const enemyEsFill = document.getElementById("enemy-es-fill");
  window.poeUpdateEnemyEs = (jsonStr) => {
    if (!enemyEsRoot) {
      return;
    }
    try {
      const e = JSON.parse(jsonStr);
      const max = Number(e.max || 0);
      if (!e.visible || !(max > 0)) {
        enemyEsRoot.classList.add("hidden");
        enemyEsRoot.setAttribute("aria-hidden", "true");
        return;
      }
      const cur = Math.min(max, Math.max(0, Number(e.current || 0)));
      const rarity = Number(e.rarity || 0);
      // 막대 폭은 반올림 전 실제 비율(천분율). 없으면 표시 수치로 계산한다.
      const permille = Number.isFinite(Number(e.fillPermille))
        ? Math.min(1000, Math.max(0, Number(e.fillPermille)))
        : (cur / max) * 1000;
      enemyEsName.textContent = String(e.name || "");
      enemyEsName.className = `enemy-es-name${rarity === 2 ? " rarity-rare" : rarity === 1 ? " rarity-magic" : ""}`;
      enemyEsText.textContent = `ES ${cur} / ${max}`;
      enemyEsFill.style.width = `${(permille / 10).toFixed(1)}%`;
      enemyEsRoot.classList.remove("hidden");
      enemyEsRoot.setAttribute("aria-hidden", "false");
    } catch (err) {
      enemyEsRoot.classList.add("hidden");
      enemyEsRoot.setAttribute("aria-hidden", "true");
      console.error("poeUpdateEnemyEs failed", err);
    }
  };

  window.poeUpdateHud = (jsonStr) => {
    try {
      const h = JSON.parse(jsonStr);
      hudLevel.textContent = String(h.exileLevel || 1);
      const xp = Number(h.exileXp || 0);
      const req = Math.max(1, Number(h.exileReqXp || 1000));
      hudXpText.textContent = `${xp} / ${req}`;
      hudXpFill.style.width = `${Math.min(100, Math.max(0, (xp / req) * 100)).toFixed(1)}%`;

      const pts = Number(h.unallocatedPoints || 0);
      if (pts > 0) {
        hudPtsBadge.textContent = `+${pts}P`;
        hudPtsBadge.classList.remove("hidden");
      } else {
        hudPtsBadge.classList.add("hidden");
      }

      const esMax = Number(h.esMax || 0);
      const esCur = Number(h.esCurrent || 0);
      if (esMax > 0) {
        hudEsRow.classList.remove("hidden");
        hudEsText.textContent = `${esCur} / ${esMax}`;
        hudEsFill.style.width = `${Math.min(100, Math.max(0, (esCur / esMax) * 100)).toFixed(1)}%`;
      } else {
        hudEsRow.classList.add("hidden");
      }

      if (hudSpiritChip) {
        hudSpiritChip.textContent = `정신력 ${Number(h.spiritReserved || 0)} / ${Number(h.spiritMax || 100)}`;
      }
      if (hudGuardChip) {
        const gCur = Number(h.guardShield || 0);
        const gMax = Number(h.guardMax || 0);
        if (gMax > 0 && gCur > 0) {
          hudGuardChip.textContent = `용암 방패 ${gCur} / ${gMax}`;
          hudGuardChip.classList.remove("hidden");
        } else {
          hudGuardChip.classList.add("hidden");
        }
      }
      if (hudHhChip) {
        const hh = Number(h.hhStacks || 0);
        if (hh > 0) {
          hudHhChip.textContent = `헤드헌터 ×${hh}`;
          hudHhChip.classList.remove("hidden");
        } else {
          hudHhChip.classList.add("hidden");
        }
      }
      if (hudMapChip) {
        if (h.mapActive) {
          if (h.mapIsArena4) {
            const st = h.mapBossKilled
              ? (h.mapArena4Unlocked ? "결투장 정복" : "결투장 보스 처치 · 4차 미획득")
              : `4차 결투장 P${Number(h.mapArenaPhase || 1)}`;
            hudMapChip.textContent = `${st} · Lv.${Number(h.mapAreaLevel || 75)}`;
          } else {
            const st = h.mapBossKilled ? "보스 처치" : `포털 ${Number(h.mapPortalsRemaining || 0)}/${Number(h.mapPortalsTotal || 6)}`;
            hudMapChip.textContent = `지도 T${Number(h.mapTier || 1)} Lv.${Number(h.mapAreaLevel || 68)} · ${st}`;
          }
          hudMapChip.classList.remove("hidden");
        } else {
          hudMapChip.classList.add("hidden");
        }
      }
      if (hudCharmsRow) {
        const charms = Array.isArray(h.charms) ? h.charms : [];
        hudCharmsRow.textContent = "";
        if (charms.length > 0) {
          hudCharmsRow.classList.remove("hidden");
          for (const ch of charms) {
            const chip = el(
              "span",
              `hud-charm-chip${ch.active ? " active" : ""}`,
              ch.occupied ? `${ch.koName} ${ch.charges}/${ch.maxCharges}` : "참 비어있음"
            );
            hudCharmsRow.appendChild(chip);
          }
        } else {
          hudCharmsRow.classList.add("hidden");
        }
      }
    } catch (_) {
    }
  };

  window.poeUpdatePanel = (jsonStr) => {
    try {
      const data = JSON.parse(jsonStr);
      lastPanelData = data;
      // C-31: 탭 전환 직후 앞서 보낸 명령의 응답이 방금 선택한 탭을 되돌리지 못하게 600ms 보호
      if (data.activeTab && (Date.now() - lastManualTabSwitchAt > 600 || data.activeTab === activeTab)) {
        applyActiveTab(data.activeTab);
      }
      revalidateSelectedTarget(data);
      const ptsDisp = Number(data.unallocatedPoints ?? 0);
      panelHeaderMeta.textContent =
        `유배자 Lv.${data.exileLevel} · ${data.className} · XP ${data.exileXp}/${data.exileReqXp} · 미배분 포인트 ${ptsDisp}`;
      updateTargetIndicator();
      renderSlots(data);
      renderInventory(data);
      renderStats(data);
      if (window.poeTreeModule && typeof window.poeTreeModule.updateState === "function") {
        window.poeTreeModule.updateState(data);
      }
      if (window.poeGearModule && typeof window.poeGearModule.updateState === "function") {
        window.poeGearModule.updateState(data);
      }
      if (window.poeMapsModule && typeof window.poeMapsModule.updateState === "function") {
        window.poeMapsModule.updateState(data);
      }
    } catch (err) {
      // C-31: 렌더/파싱 예외를 삼키지 않고 사용자에게 오류 토스트 노출
      showToast(false, `패널 갱신 오류: ${err && err.message ? err.message : "알 수 없는 오류"}`);
    }
  };

  window.poeOnPanelOpen = (jsonStr) => {
    panelToast.classList.add("hidden");
    panelBackdrop.classList.remove("hidden");
    let tab = activeTab;
    if (jsonStr) {
      try {
        const parsed = JSON.parse(jsonStr);
        if (parsed && parsed.tab) {
          tab = parsed.tab;
        }
      } catch (_) {
      }
    }
    applyActiveTab(tab);
    if (lastPanelData) {
      renderSlots(lastPanelData);
    }
  };

  window.poeOnPanelClose = () => {
    selectedTarget = null;
    lastPanelData = null;
    updateTargetIndicator();
    panelBackdrop.classList.add("hidden");
    if (window.poeTreeModule) {
      if (typeof window.poeTreeModule.onPanelClose === "function") {
        window.poeTreeModule.onPanelClose();
      } else if (typeof window.poeTreeModule.setActive === "function") {
        window.poeTreeModule.setActive(false);
      }
    }
    if (window.poeGearModule && typeof window.poeGearModule.onPanelClose === "function") {
      window.poeGearModule.onPanelClose();
    }
    if (window.poeMapsModule && typeof window.poeMapsModule.onPanelClose === "function") {
      window.poeMapsModule.onPanelClose();
    }
  };

  window.poeToast = (jsonStr) => {
    try {
      const t = JSON.parse(jsonStr);
      showToast(Boolean(t.ok), t.message || "");
    } catch (_) {
    }
  };

  window.poeImeText = (jsonStr) => {
    try {
      const msg = JSON.parse(jsonStr);
      if (window.poeTreeModule && typeof window.poeTreeModule.onImeText === "function") {
        window.poeTreeModule.onImeText(msg.text || "");
      }
    } catch (_) {
    }
  };

  window.poeImeKey = (jsonStr) => {
    try {
      const msg = JSON.parse(jsonStr);
      if (window.poeTreeModule && typeof window.poeTreeModule.onImeKey === "function") {
        window.poeTreeModule.onImeKey(msg.key || "");
      }
    } catch (_) {
    }
  };

  window.poeBlurSearch = () => {
    if (window.poeTreeModule && typeof window.poeTreeModule.blurSearch === "function") {
      window.poeTreeModule.blurSearch();
    }
  };
})();
