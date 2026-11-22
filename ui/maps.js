(() => {
  "use strict";

  const activeBadge = document.getElementById("maps-active-badge");
  const activeBody = document.getElementById("maps-active-body");
  const packStatus = document.getElementById("maps-pack-status");
  const btnReenter = document.getElementById("btn-map-reenter");
  const btnReturn = document.getElementById("btn-map-return");
  const btnAbandon = document.getElementById("btn-map-abandon");

  const tributeBadge = document.getElementById("maps-tribute-badge");
  const arena4Status = document.getElementById("maps-arena4-status");
  const arena4Name = document.getElementById("maps-arena4-name");
  const btnOpenArena4 = document.getElementById("btn-map-open-arena4");

  const uninstallBadge = document.getElementById("maps-uninstall-badge");
  const btnSafeUninstall = document.getElementById("btn-map-safe-uninstall");

  const atlasPtsBadge = document.getElementById("maps-atlas-pts");
  const atlasSummary = document.getElementById("maps-atlas-summary");
  const atlasGrid = document.getElementById("maps-atlas-grid");

  const bagCount = document.getElementById("maps-bag-count");
  const bagList = document.getElementById("maps-bag-list");

  let isActiveTab = false;
  let lastMapsData = null;
  let uninstallConfirmPending = false;
  let uninstallConfirmTimer = null;

  function el(tag, className, text) {
    const node = document.createElement(tag);
    if (className) node.className = className;
    if (text !== undefined && text !== null) node.textContent = String(text);
    return node;
  }

  function sendMapCommand(payload) {
    if (typeof window.poeSendCommand === "function") {
      window.poeSendCommand(Object.assign({ reqId: window.poeNextReqId ? window.poeNextReqId() : 0 }, payload));
    }
  }

  function resetUninstallConfirm() {
    uninstallConfirmPending = false;
    if (uninstallConfirmTimer) {
      clearTimeout(uninstallConfirmTimer);
      uninstallConfirmTimer = null;
    }
    if (btnSafeUninstall) {
      const armed = Boolean(lastMapsData && lastMapsData.uninstallCleanupArmed);
      btnSafeUninstall.textContent = armed ? "정리 완료됨 (저장 후 종료)" : "안전 제거 정리 실행";
      btnSafeUninstall.disabled = armed;
    }
  }

  function renderActiveMap(maps) {
    if (!activeBody || !activeBadge) return;
    const act = (maps && maps.active) || {};
    activeBody.textContent = "";

    if (!act.active) {
      activeBadge.textContent = "대기 중";
      activeBadge.className = "badge";
      // 팩을 쓸 수 없으면 개방 안내 대신 위의 사용 불가 사유를 보라고 한다(버튼도 막혀 있음).
      const hint = maps && maps.packAvailable === false
        ? "현재 열려 있는 지도가 없습니다. 지도 팩을 쓸 수 없어 지금은 지도를 개방할 수 없습니다."
        : "현재 열려 있는 지도가 없습니다. 오른쪽 목록에서 지도를 선택해 개방하세요.";
      activeBody.appendChild(el("div", "empty-hint", hint));
      if (btnReenter) btnReenter.classList.add("hidden");
      if (btnReturn) btnReturn.classList.add("hidden");
      if (btnAbandon) btnAbandon.classList.add("hidden");
      return;
    }

    const portalsRem = Number(act.portalsRemaining || 0);
    const portalsTotal = Number(act.portalsTotal || (act.isArena4 ? 1 : 6));
    const areaLv = Number(act.areaLevel || 68);
    const bossDone = Boolean(act.bossKilled);
    const inCell = Boolean(act.inMapCell);

    activeBadge.textContent = act.isArena4
      ? (bossDone ? "결투장 정복" : `4차 결투장 P${Number(act.arenaPhase || 1)}`)
      : (bossDone ? `T${Number(act.tier || 1)} 보스 처치` : `T${Number(act.tier || 1)} 진행 중`);
    activeBadge.className = "badge badge-aura";

    activeBody.appendChild(el("div", "", `${act.dungeonName || "지도"} (몬스터 Lv.${areaLv})`));
    activeBody.appendChild(el("div", "", `위치: ${inCell ? "지도 내부" : "바깥(귀환됨)"} · 남은 포털: ${portalsRem} / ${portalsTotal}`));
    if (!act.isArena4) {
      const iiq = Math.round(Number(act.iiqPct || 0));
      const iir = Math.round(Number(act.iirPct || 0));
      const pack = Math.round(Number(act.packSizePct || 0));
      activeBody.appendChild(el("div", "map-bag-bonus", `지도 보너스: 수량 +${iiq}% · 희귀도 +${iir}% · 무리 +${pack}%`));
    }

    const modLines = Array.isArray(act.modLines) ? act.modLines : [];
    if (modLines.length > 0) {
      const box = el("div", "maps-active-mod-list");
      for (const line of modLines) {
        box.appendChild(el("div", "", `• ${line}`));
      }
      activeBody.appendChild(box);
    }

    if (btnReenter) {
      const canReenter = !inCell && !act.isArena4 && portalsRem > 0;
      btnReenter.classList.toggle("hidden", !canReenter);
      btnReenter.disabled = !canReenter || !maps.packAvailable;
    }
    if (btnReturn) {
      btnReturn.classList.toggle("hidden", !inCell);
      btnReturn.disabled = !inCell;
    }
    if (btnAbandon) {
      btnAbandon.classList.remove("hidden");
      btnAbandon.disabled = false;
    }
  }

  function renderArena4(maps) {
    const trib = Number((maps && maps.tributeCount) || 0);
    const unlocked = Boolean(maps && maps.ascendancy4Unlocked);
    const act = (maps && maps.active) || {};
    const inCell = Boolean(act.active && act.inMapCell);
    const armed = Boolean(maps && maps.uninstallCleanupArmed);

    if (tributeBadge) {
      tributeBadge.textContent = `공물 ${trib}개`;
    }
    if (arena4Name) {
      arena4Name.textContent = maps.arenaDungeonName || "결투장";
    }
    if (arena4Status) {
      arena4Status.textContent = unlocked
        ? "정복 완료 · 전직 8/8 · 기본 최대 정신력 220 해금"
        : "미정복 · T6+ 지도 보스 처치 시 4차 전직의 공물 획득";
      arena4Status.classList.toggle("done", unlocked);
    }
    if (btnOpenArena4) {
      btnOpenArena4.disabled = !maps.packAvailable || armed || inCell || trib <= 0;
    }
  }

  function renderSafeUninstall(maps) {
    const armed = Boolean(maps && maps.uninstallCleanupArmed);
    if (uninstallBadge) {
      uninstallBadge.textContent = armed ? "정리 완료" : "정상 운용";
      uninstallBadge.className = armed ? "badge badge-guard" : "badge";
    }
    if (!uninstallConfirmPending && btnSafeUninstall) {
      btnSafeUninstall.textContent = armed ? "정리 완료됨 (저장 후 종료)" : "안전 제거 정리 실행";
      btnSafeUninstall.disabled = armed;
    }
  }

  function renderAtlas(maps) {
    if (!atlasGrid || !atlasSummary || !atlasPtsBadge) return;
    const atlas = (maps && maps.atlas) || {};
    const atlasPts = Number(atlas.atlasTierPoints || 0);
    const msPts = Number(atlas.milestonePassivePoints || 0);
    const totalPts = Number(atlas.totalBonusPoints || (atlasPts + msPts));
    const cleared = Number(atlas.totalMapsCleared || 0);
    const hiTier = Number(atlas.highestTierCleared || 0);
    const iiq = Math.round(Number(atlas.bonusIiqPct || 0));
    const iir = Math.round(Number(atlas.bonusIirPct || 0));

    atlasPtsBadge.textContent = `보너스 패시브 +${totalPts} / 24`;
    atlasSummary.textContent =
      `아틀라스 첫 클리어 ${atlasPts}/16 · 보스/결투장 이정표 ${msPts}/8 · 누적 클리어 ${cleared}회 (최고 T${hiTier}) · 아틀라스 수량 +${iiq}% / 희귀도 +${iir}%`;

    atlasGrid.textContent = "";
    const tiers = Array.isArray(atlas.tiers) ? atlas.tiers : [];
    for (const t of tiers) {
      const tile = el("div", `atlas-tile${t.completed ? " done" : ""}`);
      const head = el("div", "atlas-tile-head");
      head.appendChild(el("span", "", `T${t.tier} · Lv.${t.areaLevel}`));
      head.appendChild(el("span", "", t.completed ? "완료 +1P" : "미완료"));
      tile.appendChild(head);
      tile.appendChild(el("div", "atlas-tile-name", t.dungeonName || ""));
      tile.appendChild(el("div", "atlas-tile-sub", t.theme || ""));
      atlasGrid.appendChild(tile);
    }
  }

  function renderBagMaps(maps) {
    if (!bagList || !bagCount) return;
    const list = Array.isArray(maps && maps.bagMaps) ? maps.bagMaps : [];
    const act = (maps && maps.active) || {};
    const inCell = Boolean(act.active && act.inMapCell);
    const armed = Boolean(maps && maps.uninstallCleanupArmed);

    let totalMaps = 0;
    for (const m of list) totalMaps += Number(m.count || 1);
    bagCount.textContent = `${totalMaps}장`;
    bagList.textContent = "";

    if (list.length === 0) {
      bagList.appendChild(el("div", "empty-hint", "보유 중인 지도가 없습니다. Lv.50+ 보스/레어 처치 또는 지도 보스 처치로 지도를 획득하세요."));
      return;
    }

    for (const m of list) {
      const rar = m.rarity || "normal";
      const card = el("div", `map-bag-card rarity-${rar}`);
      const head = el("div", "map-bag-head");
      const countTxt = Number(m.count || 1) > 1 ? ` ×${m.count}` : "";
      head.appendChild(el("span", "map-bag-title", `${m.displayName || `T${m.tier} 지도`}${countTxt}`));

      const openBtn = el("button", "btn btn-primary", "지도 열기");
      openBtn.type = "button";
      openBtn.disabled = !maps.packAvailable || armed || inCell;
      openBtn.onclick = () => {
        sendMapCommand({
          op: "mapOpen",
          baseFormId: Number(m.baseFormId || 0),
          ownerFormId: Number(m.ownerFormId || 0),
          uniqueId: Number(m.uniqueId || 0),
        });
      };
      head.appendChild(openBtn);
      card.appendChild(head);

      const iiq = Math.round(Number(m.iiqPct || 0));
      const iir = Math.round(Number(m.iirPct || 0));
      const pack = Math.round(Number(m.packSizePct || 0));
      if (iiq > 0 || iir > 0 || pack > 0) {
        card.appendChild(el("div", "map-bag-bonus", `수량 +${iiq}% · 희귀도 +${iir}% · 무리 +${pack}%`));
      }

      const modLines = Array.isArray(m.modLines) ? m.modLines : [];
      if (modLines.length > 0) {
        const mbox = el("div", "map-bag-mods");
        for (const line of modLines) {
          mbox.appendChild(el("div", "", `• ${line}`));
        }
        card.appendChild(mbox);
      }
      bagList.appendChild(card);
    }
  }

  function renderAll() {
    if (!lastMapsData) return;
    if (packStatus) {
      packStatus.hidden = Boolean(lastMapsData.packAvailable);
      packStatus.textContent = lastMapsData.packUnavailableReason || "";
    }
    renderActiveMap(lastMapsData);
    renderArena4(lastMapsData);
    renderSafeUninstall(lastMapsData);
    renderAtlas(lastMapsData);
    renderBagMaps(lastMapsData);
  }

  if (btnReenter) {
    btnReenter.onclick = () => sendMapCommand({ op: "mapReenter" });
  }
  if (btnReturn) {
    btnReturn.onclick = () => sendMapCommand({ op: "mapReturnPortal" });
  }
  if (btnAbandon) {
    btnAbandon.onclick = () => sendMapCommand({ op: "mapAbandon" });
  }
  if (btnOpenArena4) {
    btnOpenArena4.onclick = () => sendMapCommand({ op: "mapOpenArena4" });
  }
  if (btnSafeUninstall) {
    btnSafeUninstall.onclick = () => {
      if (lastMapsData && lastMapsData.uninstallCleanupArmed) return;
      if (!uninstallConfirmPending) {
        uninstallConfirmPending = true;
        btnSafeUninstall.textContent = "정말 실행할까요? (다시 클릭해 확정)";
        if (uninstallConfirmTimer) clearTimeout(uninstallConfirmTimer);
        uninstallConfirmTimer = setTimeout(resetUninstallConfirm, 5000);
        return;
      }
      resetUninstallConfirm();
      sendMapCommand({ op: "mapSafeUninstall" });
    };
  }

  window.poeMapsModule = {
    setActive(active) {
      isActiveTab = Boolean(active);
      if (!isActiveTab) {
        resetUninstallConfirm();
      } else if (lastMapsData) {
        renderAll();
      }
    },
    updateState(data) {
      if (!data || !data.maps) return;
      lastMapsData = data.maps;
      if (isActiveTab) {
        renderAll();
      }
    },
    onPanelClose() {
      isActiveTab = false;
      lastMapsData = null;
      resetUninstallConfirm();
    },
  };
})();
