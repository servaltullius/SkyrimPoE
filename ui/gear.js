(() => {
  "use strict";

  const equippedList = document.getElementById("gear-equipped-list");
  const bagList = document.getElementById("gear-bag-list");
  const bagCountEl = document.getElementById("gear-bag-count");
  const selectedTagEl = document.getElementById("gear-selected-tag");
  const itemCardEl = document.getElementById("gear-item-card");
  const wisdomCountEl = document.getElementById("gear-wisdom-count");
  const tokenListEl = document.getElementById("gear-token-list");
  const jewellerCountEl = document.getElementById("gear-jeweller-count");
  const jewellerListEl = document.getElementById("gear-jeweller-list");
  const currencyGridEl = document.getElementById("gear-currency-grid");
  const gearStatsGrid = document.getElementById("gear-stats-grid");

  if (!equippedList || !bagList || !itemCardEl) {
    window.poeGearModule = {
      setActive() {},
      updateState() {},
      onPanelClose() {},
    };
    return;
  }

  // 화폐 종류 인덱스(Core::CurrencyKind와 1:1 일치)
  const KIND_WISDOM = 0;
  const KIND_REGRET = 10;
  const KIND_JEWELLER = 11;

  let isActive = false;
  let lastData = null;
  let selectedRef = null; // { source: "equipped"|"bag", slotIndex, baseFormId, uniqueId, equipped }
  let pendingStampBaseFormId = 0; // 검수 Major #5: 미추적(uniqueId===0) 스택 제작 직후 새로 스탬핑된 UID 우선 추적
  let prevTrackedBagKeySet = new Set();

  function el(tag, className, text) {
    const node = document.createElement(tag);
    if (className) node.className = className;
    if (text !== undefined && text !== null) node.textContent = String(text);
    return node;
  }

  function rarityBadgeInfo(rarity) {
    if (rarity === "rare") return { text: "[희귀]", badgeCls: "badge-rarity-rare", textCls: "rarity-rare" };
    if (rarity === "magic") return { text: "[매직]", badgeCls: "badge-rarity-magic", textCls: "rarity-magic" };
    return { text: "[일반]", badgeCls: "badge-rarity-normal", textCls: "rarity-normal" };
  }

  function bagTrackKey(item) {
    return `${item.baseFormId}:${item.uniqueId}`;
  }

  function resolveSelection(gear) {
    const eq = (gear && gear.equippedSlots) || [];
    const bag = (gear && gear.bagItems) || [];

    if (selectedRef) {
      if (selectedRef.source === "equipped") {
        const slot = eq[selectedRef.slotIndex];
        if (slot && slot.occupied) {
          selectedRef = {
            source: "equipped",
            slotIndex: slot.slotIndex,
            baseFormId: slot.baseFormId,
            uniqueId: slot.uniqueId,
          };
          return { item: slot, preferEquipped: true };
        }
      } else if (selectedRef.source === "bag") {
        // 검수 Major #5: 미추적(uniqueId===0) 스택(×2 이상)에 오브를 바른 직후에는 새로 생성된 추적 UID를 최우선 선택한다.
        if (selectedRef.uniqueId === 0 && pendingStampBaseFormId === selectedRef.baseFormId) {
          const newlyStamped = bag.find(
            (b) =>
              b.baseFormId === selectedRef.baseFormId &&
              b.uniqueId > 0 &&
              !prevTrackedBagKeySet.has(bagTrackKey(b))
          );
          pendingStampBaseFormId = 0;
          if (newlyStamped) {
            selectedRef = {
              source: "bag",
              slotIndex: newlyStamped.slotIndex,
              baseFormId: newlyStamped.baseFormId,
              uniqueId: newlyStamped.uniqueId,
              equipped: Boolean(newlyStamped.equipped),
            };
            return { item: newlyStamped, preferEquipped: Boolean(newlyStamped.equipped) };
          }
        }
        // 1) 정확한 (baseFormId, uniqueId, equipped) 일치 우선
        const exact = bag.find(
          (b) =>
            b.baseFormId === selectedRef.baseFormId &&
            b.uniqueId === selectedRef.uniqueId &&
            Boolean(b.equipped) === Boolean(selectedRef.equipped)
        );
        if (exact) {
          return { item: exact, preferEquipped: Boolean(exact.equipped) };
        }
        // 2) 미추적 일반(uniqueId===0) 아이템이 사라지고 새 uniqueId가 부여된 경우 폴백 추적
        if (selectedRef.uniqueId === 0) {
          const newlyStamped = bag.find(
            (b) =>
              b.baseFormId === selectedRef.baseFormId &&
              b.uniqueId > 0 &&
              !prevTrackedBagKeySet.has(bagTrackKey(b))
          );
          if (newlyStamped) {
            selectedRef = {
              source: "bag",
              slotIndex: newlyStamped.slotIndex,
              baseFormId: newlyStamped.baseFormId,
              uniqueId: newlyStamped.uniqueId,
              equipped: Boolean(newlyStamped.equipped),
            };
            return { item: newlyStamped, preferEquipped: Boolean(newlyStamped.equipped) };
          }
        }
      }
    }

    // 기본 선택: 첫 번째 착용 장비 -> 첫 번째 가방 장비
    const firstEq = eq.find((s) => s.occupied);
    if (firstEq) {
      selectedRef = {
        source: "equipped",
        slotIndex: firstEq.slotIndex,
        baseFormId: firstEq.baseFormId,
        uniqueId: firstEq.uniqueId,
        equipped: true,
      };
      return { item: firstEq, preferEquipped: true };
    }
    if (bag.length > 0) {
      const b0 = bag[0];
      selectedRef = {
        source: "bag",
        slotIndex: b0.slotIndex,
        baseFormId: b0.baseFormId,
        uniqueId: b0.uniqueId,
        equipped: Boolean(b0.equipped),
      };
      return { item: b0, preferEquipped: Boolean(b0.equipped) };
    }

    selectedRef = null;
    return null;
  }

  function renderEquippedList(gear) {
    equippedList.textContent = "";
    const slots = (gear && gear.equippedSlots) || [];
    for (const slot of slots) {
      if (!slot.occupied) {
        const row = el("div", "gear-row empty");
        row.appendChild(el("span", "gear-slot-tag", slot.slotLabel));
        row.appendChild(el("span", "section-sub", "(비어 있음)"));
        equippedList.appendChild(row);
        continue;
      }

      const btn = document.createElement("button");
      btn.type = "button";
      btn.className = "gear-row";
      if (selectedRef && selectedRef.source === "equipped" && selectedRef.slotIndex === slot.slotIndex) {
        btn.classList.add("selected");
      }

      const rb = rarityBadgeInfo(slot.rarity);
      btn.appendChild(el("span", "gear-slot-tag", slot.slotLabel));
      btn.appendChild(el("span", `badge ${rb.badgeCls}`, rb.text));
      btn.appendChild(el("span", `gear-item-name ${rb.textCls}`, slot.displayName));
      if (slot.requirementMet === false) {
        btn.appendChild(el("span", "badge badge-req-fail", "요구 미달"));
      }

      btn.onclick = () => {
        pendingStampBaseFormId = 0;
        selectedRef = {
          source: "equipped",
          slotIndex: slot.slotIndex,
          baseFormId: slot.baseFormId,
          uniqueId: slot.uniqueId,
          equipped: true,
        };
        renderAll();
      };
      equippedList.appendChild(btn);
    }
  }

  function renderBagList(gear) {
    bagList.textContent = "";
    const bag = (gear && gear.bagItems) || [];
    bagCountEl.textContent = `${bag.length}종`;
    if (bag.length === 0) {
      bagList.appendChild(el("div", "section-sub", "가방에 제작 가능한 무기·방어구·장신구가 없습니다."));
      return;
    }

    for (const item of bag) {
      const btn = document.createElement("button");
      btn.type = "button";
      btn.className = "gear-row";
      if (
        selectedRef &&
        selectedRef.source === "bag" &&
        selectedRef.baseFormId === item.baseFormId &&
        selectedRef.uniqueId === item.uniqueId &&
        Boolean(selectedRef.equipped) === Boolean(item.equipped)
      ) {
        btn.classList.add("selected");
      }

      const rb = rarityBadgeInfo(item.rarity);
      btn.appendChild(el("span", "gear-slot-tag", item.slotLabel));
      btn.appendChild(el("span", `badge ${rb.badgeCls}`, rb.text));
      btn.appendChild(el("span", `gear-item-name ${rb.textCls}`, item.displayName));
      if (item.equipped) {
        btn.appendChild(el("span", "badge badge-tracked", "장착"));
      }
      if (item.count > 1) {
        btn.appendChild(el("span", "inv-right", `×${item.count}`));
      }

      btn.onclick = () => {
        pendingStampBaseFormId = 0;
        selectedRef = {
          source: "bag",
          slotIndex: item.slotIndex,
          baseFormId: item.baseFormId,
          uniqueId: item.uniqueId,
          equipped: Boolean(item.equipped),
        };
        renderAll();
      };
      bagList.appendChild(btn);
    }
  }

  function renderItemCard(resolved) {
    itemCardEl.textContent = "";
    itemCardEl.className = "gear-item-card";
    if (!resolved || !resolved.item || !resolved.item.card) {
      selectedTagEl.textContent = "장비를 선택하세요";
      itemCardEl.appendChild(
        el("div", "section-sub", "왼쪽에서 착용 장비 또는 가방 장비를 선택하면 상세 어픽스와 오브 제작이 열립니다.")
      );
      return;
    }

    const item = resolved.item;
    const card = item.card;
    const rb = rarityBadgeInfo(card.rarity);
    if (card.rarity === "magic") itemCardEl.classList.add("card-magic");
    else if (card.rarity === "rare") itemCardEl.classList.add("card-rare");

    selectedTagEl.textContent = resolved.preferEquipped
      ? `장착 · ${card.slotLabel}`
      : `가방 · ${card.slotLabel}`;

    const head = el("div", "gear-card-head");
    head.appendChild(el("span", `badge ${rb.badgeCls}`, card.rarityBadge || rb.text));
    head.appendChild(el("span", `gear-card-title ${rb.textCls}`, card.title || item.displayName));
    itemCardEl.appendChild(head);

    const meta = el("div", "gear-card-meta");
    meta.appendChild(el("span", "", `부위: ${card.slotLabel}`));
    meta.appendChild(el("span", "", `아이템 레벨(iLvl): ${card.itemLevel}`));
    if (item.uniqueId > 0) {
      meta.appendChild(el("span", "", `추적 ID #${item.uniqueId}`));
    } else {
      meta.appendChild(el("span", "", "순정 일반(제작 시 1개 자동 분리)"));
    }
    itemCardEl.appendChild(meta);

    if (card.requirementText) {
      // 검수 Minor #7: ItemFormatter가 이미 '(미달 — PoE 효과 비활성)' 문구를 포함하므로 중복 접미사를 붙이지 않는다.
      const reqLine = el(
        "div",
        card.requirementMet ? "gear-card-meta" : "gear-card-meta gear-card-req-fail",
        card.requirementText
      );
      itemCardEl.appendChild(reqLine);
    }

    if (Array.isArray(card.baseStatLines) && card.baseStatLines.length > 0) {
      const baseSec = el("div", "gear-card-section");
      for (const line of card.baseStatLines) {
        baseSec.appendChild(el("div", "gear-base-line", line));
      }
      itemCardEl.appendChild(baseSec);
    }

    const affSec = el("div", "gear-card-section");
    const affixes = card.affixLines || [];
    if (affixes.length === 0) {
      affSec.appendChild(
        el("div", "section-sub", "부여된 PoE 어픽스 없음 — 진화/연금술의 오브로 어픽스를 부여할 수 있습니다.")
      );
    } else {
      for (const a of affixes) {
        const row = el("div", "gear-affix-line");
        const bCls = a.type === "prefix" ? "gear-affix-prefix" : "gear-affix-suffix";
        row.appendChild(el("span", `gear-affix-badge ${bCls}`, a.tierBadge || (a.type === "prefix" ? "[접두]" : "[접미]")));
        row.appendChild(el("span", "", a.textKo));
        affSec.appendChild(row);
      }
    }
    itemCardEl.appendChild(affSec);
  }

  function renderTokens(gear) {
    tokenListEl.textContent = "";
    const currencies = (gear && gear.currencies) || [];
    const wisdomCount = currencies[KIND_WISDOM] ? currencies[KIND_WISDOM].count : 0;
    wisdomCountEl.textContent = `지혜의 두루마리 ${wisdomCount}개`;

    const tokens = (gear && gear.tokens) || [];
    if (tokens.length === 0) {
      tokenListEl.appendChild(el("div", "section-sub", "보유 중인 봉인 전리품 토큰이 없습니다."));
      return;
    }

    for (const tk of tokens) {
      const row = el("div", "gear-row gear-row-static");
      const rb = rarityBadgeInfo(tk.rarity);
      row.appendChild(el("span", `badge ${rb.badgeCls}`, rb.text));
      row.appendChild(el("span", "gear-slot-tag", `T${tk.tier} ${tk.slotLabel}`));
      row.appendChild(el("span", `gear-item-name ${rb.textCls}`, `${tk.displayName} ×${tk.count}`));

      const btn = el("button", "btn btn-primary", "감정");
      btn.type = "button";
      btn.disabled = wisdomCount <= 0;
      btn.onclick = () => {
        window.poeSendCommand({
          op: "gearIdentifyToken",
          reqId: window.poeNextReqId(),
          tier: tk.tier,
          rarityCode: tk.rarityCode,
          slotGroup: tk.slotGroup,
        });
      };
      row.appendChild(btn);
      tokenListEl.appendChild(row);
    }
  }

  function renderJewellerTargets(gear) {
    jewellerListEl.textContent = "";
    const currencies = (gear && gear.currencies) || [];
    const jewellerCount = currencies[KIND_JEWELLER] ? currencies[KIND_JEWELLER].count : 0;
    jewellerCountEl.textContent = `쥬얼러의 오브 ${jewellerCount}개`;

    const targets = (gear && gear.jewellerTargets) || [];
    if (targets.length === 0) {
      jewellerListEl.appendChild(el("div", "section-sub", "장착 중인 스킬·발동·주문 젬이 없습니다."));
      return;
    }

    for (const t of targets) {
      const row = el("div", "gear-row gear-row-static");
      row.appendChild(el("span", "gear-item-name", t.label));
      row.appendChild(el("span", "inv-right", `소켓 ${t.socketCount}/${t.maxSockets}`));

      const btn = el("button", "btn btn-primary", t.canExpand ? "+1 소켓" : "최대");
      btn.type = "button";
      btn.disabled = !t.canExpand || jewellerCount <= 0;
      btn.onclick = () => {
        window.poeSendCommand({
          op: "gearApplyJeweller",
          reqId: window.poeNextReqId(),
          slotIndex: t.slotIndex,
          triggeredSpellInstanceId: t.triggeredSpellInstanceId || 0,
        });
      };
      row.appendChild(btn);
      jewellerListEl.appendChild(row);
    }
  }

  function renderCurrencies(gear, resolved) {
    currencyGridEl.textContent = "";
    const currencies = (gear && gear.currencies) || [];
    const targetItem = resolved ? resolved.item : null;
    const canFlags = targetItem && Array.isArray(targetItem.canApplyOrb) ? targetItem.canApplyOrb : null;
    const canReasons = targetItem && Array.isArray(targetItem.canApplyOrbReason) ? targetItem.canApplyOrbReason : null;

    for (const cur of currencies) {
      const isSpecial =
        cur.kind === KIND_WISDOM || cur.kind === KIND_REGRET || cur.kind === KIND_JEWELLER;

      if (isSpecial) {
        const box = el("div", "orb-btn orb-btn-special");
        const top = el("div", "orb-top");
        top.appendChild(el("span", "orb-name", cur.koName));
        top.appendChild(el("span", "orb-count", `×${cur.count}`));
        box.appendChild(top);
        const roleNote =
          cur.kind === KIND_WISDOM
            ? "봉인 토큰 감정에서 사용"
            : cur.kind === KIND_REGRET
              ? "패시브 트리 회수에서 사용"
              : "젬 소켓 확장에서 사용";
        box.appendChild(el("div", "orb-desc", roleNote));
        currencyGridEl.appendChild(box);
        continue;
      }

      const btn = document.createElement("button");
      btn.type = "button";
      btn.className = "orb-btn";
      const canApply = Boolean(targetItem && canFlags && canFlags[cur.kind]);
      btn.disabled = cur.count <= 0 || !canApply;
      if (cur.count <= 0) {
        btn.title = `${cur.koDesc || ""} (보유 수량 부족)`;
      } else if (!canApply && canReasons && canReasons[cur.kind]) {
        btn.title = canReasons[cur.kind];
      } else {
        btn.title = cur.koDesc || "";
      }

      const top = el("div", "orb-top");
      top.appendChild(el("span", "orb-name", cur.koName));
      top.appendChild(el("span", "orb-count", `×${cur.count}`));
      btn.appendChild(top);
      btn.appendChild(el("div", "orb-desc", cur.koDesc || ""));

      btn.onclick = () => {
        if (!targetItem) return;
        if ((targetItem.uniqueId || 0) === 0 && selectedRef && selectedRef.source === "bag") {
          pendingStampBaseFormId = targetItem.baseFormId;
        }
        window.poeSendCommand({
          op: "gearApplyOrb",
          reqId: window.poeNextReqId(),
          orbKind: cur.kind,
          baseFormId: targetItem.baseFormId,
          uniqueId: targetItem.uniqueId || 0,
          preferEquipped: Boolean(resolved && resolved.preferEquipped),
        });
      };
      currencyGridEl.appendChild(btn);
    }
  }

  function renderGearStats(data) {
    gearStatsGrid.textContent = "";
    const s = data && data.stats;
    if (!s) return;
    const rows = [
      ["힘 / 민첩 / 지능", `${s.str} / ${s.dex} / ${s.intel}`],
      ["PoE 생명력 / 마나", `${s.poeMaxLife} / ${s.poeMaxMana}`],
      ["에너지 실드", `${s.esCurrent} / ${s.esMax}`],
      ["방어도 / 회피", `${s.armour} / ${s.evasion}`],
      ["화염 / 냉기 저항", `${s.fireResist}% / ${s.coldResist}%`],
      ["번개 / 카오스 저항", `${s.lightningResist}% / ${s.chaosResist}%`],
      ["공격/주문 막기", `${s.attackBlock ?? 0}% / ${s.spellBlock ?? 0}%`],
      ["주문 억제", `${s.spellSuppression ?? 0}%`],
      ["생명력 재생 / 이속", `${s.lifeRegenPerSec ?? 0}/초 · +${s.incMovementSpeedPct ?? 0}%`],
    ];
    for (const [k, v] of rows) {
      const row = el("div", "stat-row");
      row.appendChild(el("span", "stat-k", k));
      row.appendChild(el("span", "stat-v", v));
      gearStatsGrid.appendChild(row);
    }
  }

  function renderAll() {
    if (!lastData) return;
    const gear = lastData.gear;
    if (!gear || !gear.loaded) {
      equippedList.textContent = "";
      bagList.textContent = "";
      tokenListEl.textContent = "";
      jewellerListEl.textContent = "";
      currencyGridEl.textContent = "";
      gearStatsGrid.textContent = "";
      itemCardEl.textContent = "";
      itemCardEl.appendChild(el("div", "section-sub", "아이템 데이터(items_3_30.json)가 로드되지 않았습니다."));
      return;
    }

    const resolved = resolveSelection(gear);
    renderEquippedList(gear);
    renderBagList(gear);
    renderItemCard(resolved);
    renderTokens(gear);
    renderJewellerTargets(gear);
    renderCurrencies(gear, resolved);
    renderGearStats(lastData);

    // 다음 갱신에서 새로 스탬핑된 아이템을 식별할 수 있도록 현재 추적 UID 집합 기록
    const nextSet = new Set();
    for (const b of gear.bagItems || []) {
      if (b.uniqueId > 0) {
        nextSet.add(bagTrackKey(b));
      }
    }
    prevTrackedBagKeySet = nextSet;
  }

  window.poeGearModule = {
    setActive(active) {
      isActive = Boolean(active);
      if (isActive && lastData) {
        renderAll();
      }
    },
    updateState(data) {
      lastData = data;
      if (isActive) {
        renderAll();
      } else {
        // 비활성 탭에서도 직전 추적 UID 집합만 최신 유지
        const nextSet = new Set();
        const bag = (data && data.gear && data.gear.bagItems) || [];
        for (const b of bag) {
          if (b.uniqueId > 0) nextSet.add(bagTrackKey(b));
        }
        prevTrackedBagKeySet = nextSet;
      }
    },
    onPanelClose() {
      isActive = false;
      selectedRef = null;
      pendingStampBaseFormId = 0;
      prevTrackedBagKeySet.clear();
    },
  };
})();
