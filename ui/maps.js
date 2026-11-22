(() => {
  "use strict";

  const activeBadge = document.getElementById("maps-active-badge");
  const activeBody = document.getElementById("maps-active-body");
  const packStatus = document.getElementById("maps-pack-status");
  const travelStatus = document.getElementById("maps-travel-status");
  const openReason = document.getElementById("maps-open-reason");
  const btnReenter = document.getElementById("btn-map-reenter");
  const btnReturn = document.getElementById("btn-map-return");
  const btnAbandon = document.getElementById("btn-map-abandon");
  const btnSafeEscape = document.getElementById("btn-map-safe-escape");

  const tributeBadge = document.getElementById("maps-tribute-badge");
  const arena4Status = document.getElementById("maps-arena4-status");
  const arena4Name = document.getElementById("maps-arena4-name");
  const btnOpenArena4 = document.getElementById("btn-map-open-arena4");

  const uninstallBadge = document.getElementById("maps-uninstall-badge");
  const btnSafeUninstall = document.getElementById("btn-map-safe-uninstall");
  const uninstallRemaining = el("p", "maps-desc");
  uninstallRemaining.hidden = true;
  if (btnSafeUninstall) btnSafeUninstall.before(uninstallRemaining);

  const atlasPtsBadge = document.getElementById("maps-atlas-pts");
  const atlasSummary = document.getElementById("maps-atlas-summary");
  const atlasGrid = document.getElementById("maps-atlas-grid");

  const bagCount = document.getElementById("maps-bag-count");
  const bagList = document.getElementById("maps-bag-list");

  let isActiveTab = false;
  let lastMapsData = null;
  let uninstallConfirmPending = false;
  let uninstallConfirmTimer = null;
  let abandonConfirmUntil = 0;
  let abandonConfirmTimer = null;
  const activeRunReason = "진행 중인 회차가 있습니다. 보스를 처치하고 귀환하거나 포기한 뒤 여세요.";

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
      const incomplete = Boolean(lastMapsData && lastMapsData.uninstallCleanupRemaining);
      btnSafeUninstall.textContent = armed ? "정리 완료됨 (저장 후 종료)"
        : incomplete ? "안전 제거 정리 다시 시도" : "안전 제거 정리 실행";
      btnSafeUninstall.disabled = armed || Boolean(lastMapsData &&
        (lastMapsData.travelInProgress || lastMapsData.inPackCell));
    }
  }

  function resetAbandonConfirm() {
    abandonConfirmUntil = 0;
    if (abandonConfirmTimer !== null) {
      clearTimeout(abandonConfirmTimer);
      abandonConfirmTimer = null;
    }
    if (btnAbandon) btnAbandon.textContent = "회차 포기";
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
      resetAbandonConfirm();
      return;
    }

    const portalsRem = Number(act.portalsRemaining || 0);
    const portalsTotal = Number(act.portalsTotal || (act.isArena4 ? 1 : 6));
    const areaLv = Number(act.areaLevel || 68);
    const bossDone = Boolean(act.bossKilled);
    const arenaStatus = bossDone
      ? (maps.ascendancy4Unlocked ? "결투장 정복" : "결투장 보스 처치 · 4차 미획득")
      : `4차 결투장 P${Number(act.arenaPhase || 1)}`;
    const inCell = Boolean(act.inMapCell);

    // 실패하거나 포기를 접수한 회차는 귀환이 끝나면 닫힌다. 그때까지 진행 중으로 보이지 않게 한다.
    const abandoning = Boolean(act.endRequested) && !act.failed;
    activeBadge.textContent = act.failed
      ? (act.isArena4 ? "결투장 실패 · 귀환 대기" : `T${Number(act.tier || 1)} 실패 · 귀환 대기`)
      : abandoning
        ? (act.isArena4 ? "결투장 포기 · 귀환 대기" : `T${Number(act.tier || 1)} 포기 · 귀환 대기`)
        : act.isArena4
          ? arenaStatus
          : (bossDone ? `T${Number(act.tier || 1)} 보스 처치` : `T${Number(act.tier || 1)} 진행 중`);
    activeBadge.className = (act.failed || abandoning) ? "badge badge-guard" : "badge badge-aura";

    activeBody.appendChild(el("div", "", `${act.dungeonName || "지도"} (몬스터 Lv.${areaLv})`));
    const location = inCell ? "지도 내부" : (maps.unauthorizedInPackCell ? "허가 없는 지도 팩 셀" : "지도 팩 밖");
    activeBody.appendChild(el("div", "", `위치: ${location} · 남은 포털: ${portalsRem} / ${portalsTotal}`));
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
      const canReenter = !maps.inPackCell && !act.isArena4 && !act.failed && !act.endRequested && portalsRem > 0;
      btnReenter.classList.toggle("hidden", !canReenter);
      btnReenter.disabled = !canReenter || !maps.packAvailable || maps.travelInProgress;
    }
    if (btnReturn) {
      const canReturn = inCell || (maps.canRetryReturn && !maps.inPackCell);
      btnReturn.classList.toggle("hidden", !canReturn);
      btnReturn.disabled = !canReturn || maps.travelInProgress;
    }
    if (btnAbandon) {
      btnAbandon.classList.remove("hidden");
      btnAbandon.disabled = Boolean(maps.travelInProgress);
      if (btnAbandon.disabled) resetAbandonConfirm();
    }
  }

  function renderArena4(maps) {
    const trib = Number((maps && maps.tributeCount) || 0);
    const unlocked = Boolean(maps && maps.ascendancy4Unlocked);
    const canEnter = Boolean(maps && maps.arena4CanEnter);
    const milestones = Number((maps && maps.arena4MilestonesCompleted) || 0);
    const act = (maps && maps.active) || {};
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
        : canEnter
          ? "미정복 · T6+ 지도 보스에서 공물 획득 가능 (첫 보상 자격 충족 시 확정)"
          : `1~3차 전직 이정표 ${milestones}/3 · 모두 달성하면 입장 가능`;
      if (act.active) arena4Status.appendChild(el("div", "", activeRunReason));
      arena4Status.classList.toggle("done", unlocked);
    }
    if (btnOpenArena4) {
      btnOpenArena4.disabled = !canEnter || !maps.packAvailable || armed || act.active ||
        maps.inPackCell || maps.travelInProgress || trib <= 0;
    }
  }

  function renderSafeUninstall(maps) {
    const armed = Boolean(maps && maps.uninstallCleanupArmed);
    const remaining = maps && maps.uninstallCleanupRemaining;
    if (uninstallBadge) {
      uninstallBadge.textContent = armed ? "정리 완료" : remaining ? "정리 미완료" : "정상 운용";
      uninstallBadge.className = armed || remaining ? "badge badge-guard" : "badge";
    }
    uninstallRemaining.hidden = armed || !remaining;
    uninstallRemaining.textContent = remaining
      ? `남은 장착품 · 젬 ${remaining.gems}개 · 주얼 ${remaining.jewels}개 · 참 ${remaining.charms}개`
      : "";
    if (btnSafeUninstall) {
      const blocked = Boolean(maps.travelInProgress || maps.inPackCell);
      if (armed || blocked || !uninstallConfirmPending) resetUninstallConfirm();
      btnSafeUninstall.title = blocked ? "이동을 마치고 지도 밖에서 실행하세요." : "";
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
    const armed = Boolean(maps && maps.uninstallCleanupArmed);

    let totalMaps = 0;
    for (const m of list) totalMaps += Number(m.count || 1);
    bagCount.textContent = `${totalMaps}장`;
    bagList.textContent = "";
    if (openReason) {
      openReason.hidden = !act.active;
      openReason.textContent = act.active ? activeRunReason : "";
    }

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
      openBtn.disabled = !maps.packAvailable || armed || act.active || maps.inPackCell || maps.travelInProgress;
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
    if (travelStatus) {
      travelStatus.hidden = !lastMapsData.travelInProgress && !lastMapsData.unauthorizedInPackCell;
      travelStatus.textContent = lastMapsData.travelInProgress
        ? `${lastMapsData.travelKind} · 이동 중`
        : "입장 허가가 없습니다. 귀환 문이나 안전 탈출을 이용하세요.";
    }
    if (btnSafeEscape) {
      btnSafeEscape.classList.toggle("hidden", !lastMapsData.unauthorizedInPackCell);
      btnSafeEscape.disabled = !lastMapsData.unauthorizedInPackCell || lastMapsData.travelInProgress;
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
    btnAbandon.onclick = () => {
      if (Date.now() < abandonConfirmUntil) {
        resetAbandonConfirm();
        sendMapCommand({ op: "mapAbandon" });
        return;
      }
      resetAbandonConfirm();
      abandonConfirmUntil = Date.now() + 5000;
      btnAbandon.textContent = "포기할까요? (다시 클릭해 확정)";
      abandonConfirmTimer = setTimeout(resetAbandonConfirm, 5000);
    };
  }
  if (btnSafeEscape) {
    btnSafeEscape.onclick = () => sendMapCommand({ op: "mapSafeEscape" });
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
        resetAbandonConfirm();
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
      resetAbandonConfirm();
    },
  };
})();
