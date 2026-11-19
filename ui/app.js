(() => {
  "use strict";

  const hudEsRow = document.getElementById("hud-es-row");
  const hudEsText = document.getElementById("hud-es-text");
  const hudEsFill = document.getElementById("hud-es-fill");
  const hudLevel = document.getElementById("hud-level");
  const hudXpText = document.getElementById("hud-xp-text");
  const hudXpFill = document.getElementById("hud-xp-fill");
  const hudPtsBadge = document.getElementById("hud-pts-badge");

  const panelBackdrop = document.getElementById("panel-backdrop");
  const panelHeaderMeta = document.getElementById("panel-header-meta");
  const panelToast = document.getElementById("panel-toast");
  const targetIndicator = document.getElementById("target-indicator");
  const slotsList = document.getElementById("slots-list");
  const inventoryList = document.getElementById("inventory-list");
  const statsGrid = document.getElementById("stats-grid");
  const btnClose = document.getElementById("btn-close");

  let nextReqId = 1;
  let lastPanelData = null;
  let selectedTarget = null; // { mode: "equip"|"socket", slot: number, spellIdx: number, label: string }

  function sendCommand(payload) {
    if (typeof window.poeCommand === "function") {
      window.poeCommand(JSON.stringify(payload));
    }
  }

  function kindBadge(kind) {
    switch (kind) {
      case "attack": return { text: "공격", cls: "badge-attack" };
      case "trigger": return { text: "발동", cls: "badge-trigger" };
      case "spell": return { text: "주문", cls: "badge-spell" };
      case "support": return { text: "보조", cls: "badge-support" };
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
      // 빈 칸에 이미 스킬이 장착됐으면 선택을 해제해 실수로 덮어쓰지 않게 한다.
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
      top.appendChild(el("span", "section-sub", `소켓 ${(gem.socketedGems || []).length}/${gem.socketCount}`));

      const actions = el("div", "slot-actions");
      if ((gem.socketedGems || []).length < gem.socketCount) {
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
        sendCommand({ op: "unequip", reqId: nextReqId++, slot: slot.slotIndex });
      };
      actions.appendChild(btnUnequip);
      top.appendChild(actions);
      card.appendChild(top);

      if (slot.computed) {
        const c = slot.computed;
        card.appendChild(el("div", "slot-stats-line",
          `평균 PoE 피해 ${c.avgDamage} · 치명타 ${c.critPct}% · 마나 ${c.manaCost} · 반경 ×${c.areaMult}`));
      }

      // 발동 젬(CoC): 연결 공격 칸 선택기
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
          sendCommand({ op: "link", reqId: nextReqId++, slot: slot.slotIndex, attackSlot: parseInt(sel.value, 10) });
        };
        linkRow.appendChild(sel);
        card.appendChild(linkRow);
      }

      // 소켓 목록
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
              sendCommand({ op: "unsocket", reqId: nextReqId++, slot: slot.slotIndex, spellIdx: -1, socketIdx: idx });
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
                  sendCommand({ op: "unsocket", reqId: nextReqId++, slot: slot.slotIndex, spellIdx: idx, socketIdx: sIdx });
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
              sendCommand({ op: "unsocket", reqId: nextReqId++, slot: slot.slotIndex, spellIdx: -1, socketIdx: idx });
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
            reqId: nextReqId++,
            slot: selectedTarget.slot,
            gemId: item.gemId,
            ownerFormId: item.ownerFormId || 0,
            uniqueId: item.uniqueId || 0
          });
        } else {
          sendCommand({
            op: "socket",
            reqId: nextReqId++,
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
      ["에너지 실드", `${s.esCurrent} / ${s.esMax}`],
      ["방어도 / 회피", `${s.armour} / ${s.evasion}`],
      ["물리 / 원소 피해 감소", `${s.physDR}% / ${s.elemDR}%`],
      ["화염 / 냉기 저항", `${s.fireResist}% / ${s.coldResist}%`],
      ["번개 / 카오스 저항", `${s.lightningResist}% / ${s.chaosResist}%`]
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

  btnClose.onclick = () => sendCommand({ op: "close" });

  // ---- C++ -> JS Interop 핸들러 ----
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
    } catch (_) {
    }
  };

  window.poeUpdatePanel = (jsonStr) => {
    try {
      const data = JSON.parse(jsonStr);
      lastPanelData = data;
      revalidateSelectedTarget(data);
      panelHeaderMeta.textContent =
        `유배자 Lv.${data.exileLevel} · ${data.className} · XP ${data.exileXp}/${data.exileReqXp} · 미배분 포인트 ${data.unallocatedPoints}`;
      updateTargetIndicator();
      renderSlots(data);
      renderInventory(data);
      renderStats(data);
    } catch (_) {
    }
  };

  window.poeOnPanelOpen = () => {
    panelToast.classList.add("hidden");
    panelBackdrop.classList.remove("hidden");
    if (lastPanelData) {
      renderSlots(lastPanelData);
    }
  };

  window.poeOnPanelClose = () => {
    selectedTarget = null;
    lastPanelData = null;
    updateTargetIndicator();
    panelBackdrop.classList.add("hidden");
  };

  window.poeToast = (jsonStr) => {
    try {
      const t = JSON.parse(jsonStr);
      showToast(Boolean(t.ok), t.message || "");
    } catch (_) {
    }
  };
})();
