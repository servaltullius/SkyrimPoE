// PoE Proto 4: 패시브 트리 렌더.
// 확인하려는 것: 노드 수천 개의 트리를 Canvas 2D로, CPU 렌더링 환경(Prisma UI)에서 끊기지 않게 움직일 수 있는가.
// 그리는 방식 두 가지를 같은 화면에서 바꿔 가며 잰다.
//   direct: 매 프레임 보이는 범위의 선과 노드를 전부 다시 그린다.
//   tiles : 변하지 않는 바닥(배경, 선, 찍지 않은 노드)을 타일로 미리 그려 두고, 프레임마다 타일을 붙인 뒤
//           찍은 노드와 강조만 그 위에 그린다(통합 설계서 12.2의 3단 구성).
(function () {
  'use strict';

  var KIND = { NORMAL: 0, NOTABLE: 1, KEYSTONE: 2, MASTERY: 3, SOCKET: 4, CLASS_START: 5, ASCEND_START: 6 };
  var KIND_LABEL = ['일반', '주요 패시브', '키스톤', '마스터리', '주얼 소켓', '클래스 시작', '전직 시작'];
  var FRAME_OF_KIND = ['normal', 'notable', 'keystone', null, 'socket', null, null];

  var BACKGROUND = '#0b0a08';
  var CELL = 800;            // 공간 격자 한 칸의 크기(트리 좌표)
  var TILE = 512;            // 타일 한 변의 픽셀 수
  var TILE_LEVELS = [0.04, 0.08, 0.16, 0.32, 0.64];  // 타일을 만들어 두는 배율(트리 좌표 1단위당 픽셀)
  var MAX_SCALE = 0.9;
  var LOD_PIXELS = 5;        // 노드가 이보다 작게 보이면 그림 대신 점으로 그린다
  var TILE_BUDGET_MS = 6;    // 한 프레임에 타일을 새로 그리는 데 쓰는 시간의 한도

  var params = new URLSearchParams(location.search);
  var settings = {
    mode: params.get('mode') === 'direct' ? 'direct' : 'tiles',
    lod: params.get('lod') !== '0',
    maxTiles: clampInt(params.get('tiles'), 16, 1024, 160),
    dprCap: clampNumber(params.get('dpr'), 0.5, 3, 2),
    // cpu=1이면 캔버스를 GPU 가속 없이 만든다(willReadFrequently). Prisma UI처럼 CPU로 그리는 환경을
    // 브라우저에서 흉내 내려는 것이다. 엔진이 달라 같은 숫자가 나오지는 않는다.
    cpu: params.get('cpu') === '1',
    // w, h를 주면 창 크기와 무관하게 그 크기로 그린다. 창이 가려져 크기가 0으로 잡히는 환경에서 재기 위한 것이다.
    forceWidth: clampInt(params.get('w'), 0, 7680, 0),
    forceHeight: clampInt(params.get('h'), 0, 4320, 0),
    // 캔버스의 내부 해상도를 화면보다 낮춰 그리는 비율(1이면 화면 그대로, 0.5면 가로·세로 절반). 낮춘 그림은 화면 크기로 늘려 보인다.
    // 화면이 큰 환경(3840x2160)에서 CPU로 그리는 비용을 줄일 수 있는지 재기 위한 것이다.
    renderScale: clampNumber(params.get('scale'), 0.25, 1, 1),
    // 글자와 버튼의 배율. 주지 않으면 화면 높이에 맞춘다(1080이 1, 2160이 2).
    uiScale: clampNumber(params.get('ui'), 0.5, 4, 0),
  };
  var CONTEXT_OPTIONS = { alpha: false, willReadFrequently: settings.cpu };

  // 다음 프레임에 부른다. timer=1이면 화면 갱신 콜백 대신 타이머로 돌린다.
  // 가려진 브라우저 창에서는 화면 갱신 콜백이 오지 않아 측정 묶음의 흐름을 확인할 수 없기 때문이다(숫자는 뜻이 없다).
  var useTimerFrames = params.get('timer') === '1';
  function nextFrame(callback) {
    if (useTimerFrames) {
      setTimeout(function () { callback(performance.now()); }, 16);
    } else {
      requestAnimationFrame(callback);
    }
  }

  function clampInt(text, low, high, fallback) {
    var value = parseInt(text, 10);
    return isFinite(value) ? Math.min(high, Math.max(low, value)) : fallback;
  }

  function clampNumber(text, low, high, fallback) {
    var value = parseFloat(text);
    return isFinite(value) ? Math.min(high, Math.max(low, value)) : fallback;
  }

  var canvas = document.getElementById('tree');
  var ctx = canvas.getContext('2d', CONTEXT_OPTIONS);
  var elStats = document.getElementById('stats');
  var elTooltip = document.getElementById('tooltip');
  var elMessage = document.getElementById('message');
  var elBench = document.getElementById('bench');
  var elSearch = document.getElementById('search');
  var elSearchCount = document.getElementById('search-count');
  var elMode = document.getElementById('btn-mode');
  var elBenchButton = document.getElementById('btn-bench');

  function showMessage(text) {
    elMessage.textContent = text;
    elMessage.hidden = false;
  }

  var data = window.POE_TREE;
  if (!data || !data.nodes || !data.nodes.length) {
    showMessage('트리 데이터가 없습니다. tools/build_site.py 를 먼저 실행해 data/tree-data.js 를 만들어 주세요.');
    return;
  }

  // ---------------------------------------------------------------- 데이터 준비
  var nodes = data.nodes;
  var edges = data.edges;
  var groups = data.groups;
  var imageScale = data.imageScale;
  var nodeCount = nodes.length;
  var nodeX = new Float32Array(nodeCount);
  var nodeY = new Float32Array(nodeCount);
  var nodeKind = new Uint8Array(nodeCount);
  var nodeRadius = new Float32Array(nodeCount);  // 가리키기 판정과 강조 원에 쓰는 반지름(트리 좌표)
  var allocated = new Uint8Array(nodeCount);
  var matched = new Uint8Array(nodeCount);
  var adjacency = [];

  (function prepareNodes() {
    for (var i = 0; i < nodeCount; i++) {
      var n = nodes[i];
      nodeX[i] = n.x;
      nodeY[i] = n.y;
      nodeKind[i] = n.k;
      var frame = FRAME_OF_KIND[n.k] ? data.frames[FRAME_OF_KIND[n.k]][0] : null;
      var pixels = frame ? frame[3] : (n.i ? n.i[3] : 40);
      nodeRadius[i] = pixels / imageScale / 2;
      adjacency.push([]);
    }
    for (var e = 0; e < edges.length; e++) {
      adjacency[edges[e][0]].push(edges[e][1]);
      adjacency[edges[e][1]].push(edges[e][0]);
    }
  })();

  // 공간 격자: 화면이나 타일에 걸리는 노드와 선만 골라 그리기 위한 것.
  var grid = new Map();
  var edgeStamp = new Uint32Array(edges.length);
  var stampCounter = 0;

  function cellKey(cx, cy) {
    return (cx + 512) * 4096 + (cy + 512);
  }

  function cellAt(cx, cy, create) {
    var key = cellKey(cx, cy);
    var cell = grid.get(key);
    if (!cell && create) {
      cell = { nodes: [], edges: [] };
      grid.set(key, cell);
    }
    return cell;
  }

  (function buildGrid() {
    for (var i = 0; i < nodeCount; i++) {
      cellAt(Math.floor(nodeX[i] / CELL), Math.floor(nodeY[i] / CELL), true).nodes.push(i);
    }
    for (var e = 0; e < edges.length; e++) {
      var edge = edges[e];
      var x0, y0, x1, y1;
      if (edge.length > 2) {
        // 호는 원 전체의 범위로 넉넉하게 잡는다.
        x0 = edge[2] - edge[4]; x1 = edge[2] + edge[4];
        y0 = edge[3] - edge[4]; y1 = edge[3] + edge[4];
      } else {
        x0 = Math.min(nodeX[edge[0]], nodeX[edge[1]]); x1 = Math.max(nodeX[edge[0]], nodeX[edge[1]]);
        y0 = Math.min(nodeY[edge[0]], nodeY[edge[1]]); y1 = Math.max(nodeY[edge[0]], nodeY[edge[1]]);
      }
      for (var cy = Math.floor(y0 / CELL); cy <= Math.floor(y1 / CELL); cy++) {
        for (var cx = Math.floor(x0 / CELL); cx <= Math.floor(x1 / CELL); cx++) {
          cellAt(cx, cy, true).edges.push(e);
        }
      }
    }
  })();

  // ---------------------------------------------------------------- 그림 불러오기
  var images = [];
  var missingImages = [];

  // 불러오지 못한 그림이 있어도 멈추지 않는다. 그 그림을 쓰는 부분만 빠진 채로 그리고, 무엇이 빠졌는지 알린다.
  // 게임 안에서 그림 하나 때문에 화면 전체가 안 뜨면 측정도 못 하기 때문이다.
  function loadImages() {
    return Promise.all(data.atlases.map(function (atlas, index) {
      return new Promise(function (resolve) {
        var image = new Image();
        image.onload = function () { images[index] = image; resolve(); };
        image.onerror = function () { images[index] = null; missingImages.push(atlas.file); resolve(); };
        image.src = 'data/assets/' + atlas.file;
      });
    }));
  }

  // ---------------------------------------------------------------- 카메라
  var view = { width: 1, height: 1, dpr: 1, ui: 1 };
  var tileLimit = settings.maxTiles;  // 실제로 쓰는 타일 한도. 화면을 다 덮는 데 필요한 수보다 작으면 타일이 서로를 밀어내므로 resize에서 늘린다
  var camera = { x: 0, y: 0, scale: 0.1 };  // 화면 가운데의 트리 좌표와 배율(트리 1단위당 CSS 픽셀)
  var minScale = 0.02;
  var dirty = true;

  function resize() {
    view.dpr = Math.min(window.devicePixelRatio || 1, settings.dprCap) * settings.renderScale;
    if (settings.forceWidth && settings.forceHeight) {
      canvas.style.width = settings.forceWidth + 'px';
      canvas.style.height = settings.forceHeight + 'px';
      view.width = settings.forceWidth;
      view.height = settings.forceHeight;
    } else {
      view.width = Math.max(1, canvas.clientWidth);
      view.height = Math.max(1, canvas.clientHeight);
    }
    canvas.width = Math.round(view.width * view.dpr);
    canvas.height = Math.round(view.height * view.dpr);
    // 타일이 가장 작게(한 변이 TILE의 절반) 보일 때 화면을 덮는 수. 한도가 이보다 작으면 이번 프레임에 쓴 타일끼리 밀어내 끝내 다 채우지 못한다.
    var visibleWorst = (Math.ceil(canvas.width / (TILE / 2)) + 1) * (Math.ceil(canvas.height / (TILE / 2)) + 1);
    tileLimit = Math.max(settings.maxTiles, Math.ceil(visibleWorst * 1.25));
    view.ui = settings.uiScale || Math.min(3, Math.max(1, Math.round(view.height / 1080 * 20) / 20));
    document.documentElement.style.setProperty('--u', view.ui + 'px');
    var b = data.bounds;
    minScale = Math.min(view.width / (b.maxX - b.minX + 2000), view.height / (b.maxY - b.minY + 2000)) * 0.9;
    camera.scale = Math.max(camera.scale, minScale);
    dirty = true;
  }

  function fitMain() {
    var b = data.mainBounds;
    camera.x = (b.minX + b.maxX) / 2;
    camera.y = (b.minY + b.maxY) / 2;
    camera.scale = Math.max(minScale, Math.min(view.width / (b.maxX - b.minX + 800), view.height / (b.maxY - b.minY + 800)));
    dirty = true;
  }

  function clampCamera() {
    var b = data.bounds;
    camera.scale = Math.min(MAX_SCALE, Math.max(minScale, camera.scale));
    camera.x = Math.min(b.maxX, Math.max(b.minX, camera.x));
    camera.y = Math.min(b.maxY, Math.max(b.minY, camera.y));
  }

  function screenToWorld(sx, sy) {
    return {
      x: camera.x + (sx - view.width / 2) / camera.scale,
      y: camera.y + (sy - view.height / 2) / camera.scale,
    };
  }

  // ---------------------------------------------------------------- 바닥 그리기(변하지 않는 것)
  var counters = { nodesDrawn: 0, edgesDrawn: 0, tilesRendered: 0, tilesDrawn: 0, tilesMissing: 0 };

  function drawSprite(target, rect, x, y) {
    var image = images[rect[0]];
    if (!image) { return; }
    var w = rect[3] / imageScale;
    var h = rect[4] / imageScale;
    target.drawImage(image, rect[1], rect[2], rect[3], rect[4], x - w / 2, y - h / 2, w, h);
  }

  // 트리 좌표의 사각형 [x0, y0, x1, y1]에 걸리는 바닥을 target에 그린다.
  // target의 변환은 부르는 쪽이 "트리 좌표 → 픽셀"로 맞춰 둔다. pixelScale은 트리 1단위당 실제 픽셀 수.
  function drawStatic(target, x0, y0, x1, y1, pixelScale) {
    var i, g, cx, cy, cell, list, e;

    // 그룹 배경. 수가 적어 격자 없이 전부 훑는다.
    for (i = 0; i < groups.length; i++) {
      g = groups[i];
      var gw = g[6] / imageScale;
      var gh = g[7] / imageScale;
      var half = g[2] === 1;
      var reach = half ? gh : gh / 2;
      if (g[0] + gw / 2 < x0 || g[0] - gw / 2 > x1 || g[1] + reach < y0 || g[1] - reach > y1) {
        continue;
      }
      if (gw * pixelScale < 6) {
        continue;
      }
      var image = images[g[3]];
      if (!image) {
        continue;
      }
      if (half) {
        // 위쪽 절반만 든 그림: 그대로 한 번, 위아래를 뒤집어 한 번.
        target.drawImage(image, g[4], g[5], g[6], g[7], g[0] - gw / 2, g[1] - gh, gw, gh);
        target.save();
        target.translate(g[0], g[1]);
        target.scale(1, -1);
        target.drawImage(image, g[4], g[5], g[6], g[7], -gw / 2, -gh, gw, gh);
        target.restore();
      } else {
        target.drawImage(image, g[4], g[5], g[6], g[7], g[0] - gw / 2, g[1] - gh / 2, gw, gh);
      }
    }

    var margin = 420;  // 격자 칸 밖으로 삐져나오는 큰 노드 틀을 놓치지 않게
    var cx0 = Math.floor((x0 - margin) / CELL), cx1 = Math.floor((x1 + margin) / CELL);
    var cy0 = Math.floor((y0 - margin) / CELL), cy1 = Math.floor((y1 + margin) / CELL);

    // 선. 한 번의 경로로 모아 한 번에 긋는다.
    stampCounter++;
    target.lineWidth = Math.max(10, 1 / pixelScale);
    target.strokeStyle = '#4d4534';
    target.beginPath();
    for (cy = cy0; cy <= cy1; cy++) {
      for (cx = cx0; cx <= cx1; cx++) {
        cell = grid.get(cellKey(cx, cy));
        if (!cell) { continue; }
        list = cell.edges;
        for (i = 0; i < list.length; i++) {
          e = list[i];
          if (edgeStamp[e] === stampCounter) { continue; }
          edgeStamp[e] = stampCounter;
          traceEdge(target, edges[e]);
          counters.edgesDrawn++;
        }
      }
    }
    target.stroke();

    // 노드
    for (cy = cy0; cy <= cy1; cy++) {
      for (cx = cx0; cx <= cx1; cx++) {
        cell = grid.get(cellKey(cx, cy));
        if (!cell) { continue; }
        list = cell.nodes;
        for (i = 0; i < list.length; i++) {
          drawNode(target, list[i], false, pixelScale);
          counters.nodesDrawn++;
        }
      }
    }
  }

  function traceEdge(target, edge) {
    if (edge.length > 2) {
      target.moveTo(edge[2] + Math.cos(edge[5]) * edge[4], edge[3] + Math.sin(edge[5]) * edge[4]);
      target.arc(edge[2], edge[3], edge[4], edge[5], edge[6]);
    } else {
      target.moveTo(nodeX[edge[0]], nodeY[edge[0]]);
      target.lineTo(nodeX[edge[1]], nodeY[edge[1]]);
    }
  }

  function drawNode(target, index, active, pixelScale) {
    var n = nodes[index];
    var x = nodeX[index], y = nodeY[index];
    var kind = n.k;
    var radius = nodeRadius[index];

    if (settings.lod && radius * 2 * pixelScale < LOD_PIXELS) {
      var size = Math.max(radius * 1.4, 1.5 / pixelScale);
      target.fillStyle = active ? '#f0d890' : (kind === KIND.NORMAL ? '#6a6048' : '#a89868');
      target.fillRect(x - size / 2, y - size / 2, size, size);
      return;
    }
    if (kind === KIND.CLASS_START || kind === KIND.ASCEND_START) {
      target.fillStyle = kind === KIND.CLASS_START ? '#8a7440' : '#5a6a8a';
      target.beginPath();
      target.arc(x, y, radius * (kind === KIND.CLASS_START ? 1.6 : 1.0), 0, Math.PI * 2);
      target.fill();
      return;
    }
    var icon = active ? (n.a || n.i) : n.i;
    if (icon) {
      drawSprite(target, icon, x, y);
    }
    var frameName = FRAME_OF_KIND[kind];
    if (frameName) {
      var frame = data.frames[frameName][active ? 1 : 0];
      if (frame) {
        drawSprite(target, frame, x, y);
      }
    }
  }

  // ---------------------------------------------------------------- 타일
  var tiles = new Map();
  var tileClock = 0;

  function tileKey(level, tx, ty) {
    return level * 67108864 + (tx + 4096) * 8192 + (ty + 4096);
  }

  function renderTile(level, tx, ty) {
    var scale = TILE_LEVELS[level];
    var world = TILE / scale;
    var tileCanvas = document.createElement('canvas');
    tileCanvas.width = TILE;
    tileCanvas.height = TILE;
    var tctx = tileCanvas.getContext('2d', CONTEXT_OPTIONS);
    tctx.fillStyle = BACKGROUND;
    tctx.fillRect(0, 0, TILE, TILE);
    var x0 = tx * world, y0 = ty * world;
    tctx.setTransform(scale, 0, 0, scale, -x0 * scale, -y0 * scale);
    drawStatic(tctx, x0, y0, x0 + world, y0 + world, scale);
    counters.tilesRendered++;
    return tileCanvas;
  }

  function evictTiles() {
    if (tiles.size <= tileLimit) {
      return;
    }
    var entries = Array.from(tiles.entries());
    entries.sort(function (a, b) { return a[1].used - b[1].used; });
    for (var i = 0; i < entries.length - tileLimit; i++) {
      if (entries[i][1].used === tileClock) {
        break;  // 이번 프레임에 쓴 타일은 버리지 않는다
      }
      tiles.delete(entries[i][0]);
    }
  }

  function pickLevel(scale) {
    for (var i = 0; i < TILE_LEVELS.length; i++) {
      if (TILE_LEVELS[i] >= scale * 0.999) {
        return i;
      }
    }
    return TILE_LEVELS.length - 1;
  }

  // 이 타일이 아직 없을 때, 더 거친 단계에 이미 있는 타일의 해당 부분을 늘려서 대신 그린다.
  function drawFallback(level, tx, ty, dx, dy, dw, dh) {
    for (var coarser = level - 1; coarser >= 0; coarser--) {
      var ratio = TILE_LEVELS[level] / TILE_LEVELS[coarser];
      var ptx = Math.floor(tx / ratio), pty = Math.floor(ty / ratio);
      var parent = tiles.get(tileKey(coarser, ptx, pty));
      if (!parent) { continue; }
      parent.used = tileClock;
      var sub = TILE / ratio;
      ctx.drawImage(parent.canvas, (tx - ptx * ratio) * sub, (ty - pty * ratio) * sub, sub, sub, dx, dy, dw, dh);
      return true;
    }
    return false;
  }

  // 없는 타일은 프레임마다 TILE_BUDGET_MS만큼만 새로 만든다. 재는 것은 타일을 만드는 데 쓴 시간이다.
  // (프레임이 시작된 뒤의 시간으로 재면, 있는 타일을 그리는 데만 한도를 다 쓰는 큰 화면에서 뒤쪽 타일이 끝내 만들어지지 않는다.
  //  2026-11-13 게임 안 2차 시험의 3840x2160에서 실제로 그랬다.) 프레임마다 적어도 하나는 만든다.
  function drawTiles(x0, y0, x1, y1, pixelScale) {
    var level = pickLevel(camera.scale * view.dpr);
    var world = TILE / TILE_LEVELS[level];
    var tx0 = Math.floor(x0 / world), tx1 = Math.floor(x1 / world);
    var ty0 = Math.floor(y0 / world), ty1 = Math.floor(y1 / world);
    var missing = 0, created = 0, spent = 0;
    tileClock++;
    ctx.setTransform(1, 0, 0, 1, 0, 0);
    for (var ty = ty0; ty <= ty1; ty++) {
      // 타일 사이에 틈이 생기지 않게 가장자리를 정수 픽셀로 맞추고 폭은 다음 가장자리와의 차로 잡는다.
      var dy = Math.round((ty * world - y0) * pixelScale);
      var dh = Math.round(((ty + 1) * world - y0) * pixelScale) - dy;
      for (var tx = tx0; tx <= tx1; tx++) {
        var dx = Math.round((tx * world - x0) * pixelScale);
        var dw = Math.round(((tx + 1) * world - x0) * pixelScale) - dx;
        var key = tileKey(level, tx, ty);
        var tile = tiles.get(key);
        if (!tile && (created === 0 || spent < TILE_BUDGET_MS)) {
          var createStarted = performance.now();
          tile = { canvas: renderTile(level, tx, ty), used: tileClock };
          tiles.set(key, tile);
          created++;
          spent += performance.now() - createStarted;
        }
        if (tile) {
          tile.used = tileClock;
          ctx.drawImage(tile.canvas, dx, dy, dw, dh);
          counters.tilesDrawn++;
        } else {
          missing++;
          drawFallback(level, tx, ty, dx, dy, dw, dh);
        }
      }
    }
    counters.tilesMissing = missing;
    evictTiles();
    return missing;
  }

  // ---------------------------------------------------------------- 덧그림(찍은 노드, 검색 결과, 가리킨 노드)
  var allocatedList = [];
  var allocatedEdges = [];
  var matchList = [];
  var hovered = -1;

  function rebuildAllocated() {
    allocatedList = [];
    allocatedEdges = [];
    for (var i = 0; i < nodeCount; i++) {
      if (allocated[i]) { allocatedList.push(i); }
    }
    for (var e = 0; e < edges.length; e++) {
      if (allocated[edges[e][0]] && allocated[edges[e][1]]) { allocatedEdges.push(e); }
    }
    dirty = true;
  }

  function inRect(index, x0, y0, x1, y1, margin) {
    return nodeX[index] >= x0 - margin && nodeX[index] <= x1 + margin &&
           nodeY[index] >= y0 - margin && nodeY[index] <= y1 + margin;
  }

  function drawOverlay(x0, y0, x1, y1, pixelScale) {
    var i, index;
    ctx.setTransform(pixelScale, 0, 0, pixelScale, -x0 * pixelScale, -y0 * pixelScale);

    if (allocatedEdges.length) {
      ctx.lineWidth = Math.max(14, 2 / pixelScale);
      ctx.strokeStyle = '#d8b860';
      ctx.beginPath();
      for (i = 0; i < allocatedEdges.length; i++) {
        var edge = edges[allocatedEdges[i]];
        if (!inRect(edge[0], x0, y0, x1, y1, 1800) && !inRect(edge[1], x0, y0, x1, y1, 1800)) { continue; }
        traceEdge(ctx, edge);
      }
      ctx.stroke();
    }
    for (i = 0; i < allocatedList.length; i++) {
      index = allocatedList[i];
      if (inRect(index, x0, y0, x1, y1, 400)) {
        drawNode(ctx, index, true, pixelScale);
      }
    }

    if (matchList.length) {
      ctx.lineWidth = Math.max(8, 2 / pixelScale);
      ctx.strokeStyle = '#60d8f0';
      ctx.beginPath();
      for (i = 0; i < matchList.length; i++) {
        index = matchList[i];
        if (!inRect(index, x0, y0, x1, y1, 400)) { continue; }
        var r = Math.max(nodeRadius[index] * 1.25, 5 / pixelScale);
        ctx.moveTo(nodeX[index] + r, nodeY[index]);
        ctx.arc(nodeX[index], nodeY[index], r, 0, Math.PI * 2);
      }
      ctx.stroke();
    }

    if (hovered >= 0) {
      ctx.lineWidth = Math.max(8, 2 / pixelScale);
      ctx.strokeStyle = '#ffffff';
      ctx.beginPath();
      var hr = Math.max(nodeRadius[hovered] * 1.15, 6 / pixelScale);
      ctx.arc(nodeX[hovered], nodeY[hovered], hr, 0, Math.PI * 2);
      ctx.stroke();
    }
  }

  // ---------------------------------------------------------------- 한 프레임
  var lastRenderMs = 0;

  function render() {
    var started = performance.now();
    counters.nodesDrawn = 0;
    counters.edgesDrawn = 0;
    counters.tilesDrawn = 0;
    counters.tilesMissing = 0;

    var pixelScale = camera.scale * view.dpr;
    var halfW = view.width / 2 / camera.scale;
    var halfH = view.height / 2 / camera.scale;
    var x0 = camera.x - halfW, x1 = camera.x + halfW;
    var y0 = camera.y - halfH, y1 = camera.y + halfH;

    ctx.setTransform(1, 0, 0, 1, 0, 0);
    ctx.fillStyle = BACKGROUND;
    ctx.fillRect(0, 0, canvas.width, canvas.height);

    var pending = 0;
    if (settings.mode === 'tiles') {
      pending = drawTiles(x0, y0, x1, y1, pixelScale);
    } else {
      ctx.setTransform(pixelScale, 0, 0, pixelScale, -x0 * pixelScale, -y0 * pixelScale);
      drawStatic(ctx, x0, y0, x1, y1, pixelScale);
    }
    drawOverlay(x0, y0, x1, y1, pixelScale);

    lastRenderMs = performance.now() - started;
    return pending;
  }

  // 움직임이 없으면 다시 그리지 않는다. CPU로 그리는 환경에서는 가만히 있는 화면이 프레임을 먹지 않아야 한다.
  var frameTimes = [];
  var lastFrameAt = 0;
  var lastStatsAt = 0;
  var bench = null;
  var renderCount = 0;      // 지금까지 그린 프레임 수. 아무것도 안 그리는 기준 측정이 정말 조용했는지 확인하는 데 쓴다
  var loopRunning = false;
  var suspended = false;    // 게임 쪽에서 화면을 닫은 동안. 숨긴 화면도 스크립트는 계속 돌기 때문에 스스로 멈춘다

  function startLoop() {
    if (!loopRunning) {
      loopRunning = true;
      nextFrame(frame);
    }
  }

  function frame(now) {
    if (suspended) {
      loopRunning = false;
      lastFrameAt = 0;
      return;
    }
    nextFrame(frame);
    if (bench) {
      stepBench(now);
    }
    if (!dirty) {
      lastFrameAt = 0;
      return;
    }
    dirty = false;
    var pending = render();
    renderCount++;
    if (pending > 0) {
      dirty = true;  // 아직 못 그린 타일이 있으면 다음 프레임에 이어서 그린다
    }
    if (lastFrameAt) {
      frameTimes.push(now - lastFrameAt);
      if (frameTimes.length > 60) { frameTimes.shift(); }
      if (bench && bench.recording) {
        bench.intervals.push(now - lastFrameAt);
        bench.renders.push(lastRenderMs);
      }
    }
    lastFrameAt = now;
    if (now - lastStatsAt > 250) {
      lastStatsAt = now;
      updateStats();
    }
  }

  function updateStats() {
    var average = 0;
    for (var i = 0; i < frameTimes.length; i++) { average += frameTimes[i]; }
    average = frameTimes.length ? average / frameTimes.length : 0;
    var lines = [
      (average ? (1000 / average).toFixed(0) + ' FPS' : '대기 중') + '  그리기 ' + lastRenderMs.toFixed(1) + 'ms',
      '방식 ' + (settings.mode === 'tiles' ? '타일' : '직접') + '  배율 ' + camera.scale.toFixed(3) + '  그림 ' + canvas.width + 'x' + canvas.height,
      settings.mode === 'tiles'
        ? '타일 ' + counters.tilesDrawn + '장(보관 ' + tiles.size + '/' + tileLimit + ', 대기 ' + counters.tilesMissing + ')'
        : '노드 ' + counters.nodesDrawn + '  선 ' + counters.edgesDrawn,
      '노드 ' + nodeCount + '  찍음 ' + allocatedList.length + '  검색 ' + matchList.length,
    ];
    elStats.textContent = lines.join('\n');
  }

  // ---------------------------------------------------------------- 입력
  var drag = null;
  var pointer = { x: 0, y: 0, inside: false };

  canvas.addEventListener('mousedown', function (event) {
    if (event.button !== 0 || bench) { return; }
    drag = { x: event.clientX, y: event.clientY, moved: 0 };
    canvas.classList.add('dragging');
  });

  window.addEventListener('mousemove', function (event) {
    pointer.x = event.clientX;
    pointer.y = event.clientY;
    pointer.inside = event.target === canvas;
    if (drag) {
      var dx = event.clientX - drag.x, dy = event.clientY - drag.y;
      drag.moved += Math.abs(dx) + Math.abs(dy);
      drag.x = event.clientX;
      drag.y = event.clientY;
      camera.x -= dx / camera.scale;
      camera.y -= dy / camera.scale;
      clampCamera();
      hideTooltip();
      dirty = true;
    } else if (!bench) {
      updateHover();
    }
  });

  window.addEventListener('mouseup', function (event) {
    if (!drag) { return; }
    var moved = drag.moved;
    drag = null;
    canvas.classList.remove('dragging');
    if (moved < 4 && event.target === canvas) {
      updateHover();
      if (hovered >= 0 && nodeKind[hovered] !== KIND.CLASS_START && nodeKind[hovered] !== KIND.ASCEND_START) {
        allocated[hovered] = allocated[hovered] ? 0 : 1;
        rebuildAllocated();
      }
    }
  });

  canvas.addEventListener('mouseleave', function () {
    pointer.inside = false;
    if (hovered >= 0) { hovered = -1; dirty = true; }
    hideTooltip();
  });

  canvas.addEventListener('wheel', function (event) {
    event.preventDefault();
    if (bench) { return; }
    var before = screenToWorld(event.clientX, event.clientY);
    var delta = event.deltaMode === 1 ? event.deltaY * 33 : event.deltaY;
    camera.scale *= Math.exp(-delta * 0.0015);
    clampCamera();
    // 커서 아래의 지점이 그대로 커서 아래에 있게 한다.
    camera.x = before.x - (event.clientX - view.width / 2) / camera.scale;
    camera.y = before.y - (event.clientY - view.height / 2) / camera.scale;
    clampCamera();
    hideTooltip();
    dirty = true;
  }, { passive: false });

  function nodeAt(wx, wy) {
    var best = -1, bestDistance = Infinity;
    var cx = Math.floor(wx / CELL), cy = Math.floor(wy / CELL);
    var slack = 6 / camera.scale;  // 작게 보일 때도 가리킬 수 있게 화면 기준 여유를 준다
    for (var y = cy - 1; y <= cy + 1; y++) {
      for (var x = cx - 1; x <= cx + 1; x++) {
        var cell = grid.get(cellKey(x, y));
        if (!cell) { continue; }
        for (var i = 0; i < cell.nodes.length; i++) {
          var index = cell.nodes[i];
          var dx = nodeX[index] - wx, dy = nodeY[index] - wy;
          var distance = Math.sqrt(dx * dx + dy * dy);
          if (distance <= Math.max(nodeRadius[index], slack) && distance < bestDistance) {
            best = index;
            bestDistance = distance;
          }
        }
      }
    }
    return best;
  }

  function updateHover() {
    var found = -1;
    if (pointer.inside) {
      var world = screenToWorld(pointer.x, pointer.y);
      found = nodeAt(world.x, world.y);
    }
    if (found !== hovered) {
      hovered = found;
      dirty = true;
    }
    if (hovered >= 0) {
      showTooltip(hovered);
    } else {
      hideTooltip();
    }
  }

  var tooltipFor = -1;

  function showTooltip(index) {
    if (tooltipFor !== index) {
      tooltipFor = index;
      var n = nodes[index];
      elTooltip.textContent = '';
      var name = document.createElement('div');
      name.className = 'name';
      name.textContent = n.n || '(이름 없음)';
      elTooltip.appendChild(name);
      var kind = document.createElement('div');
      kind.className = 'kind';
      kind.textContent = KIND_LABEL[n.k] + (n.asc ? ' · ' + n.asc : '') + (allocated[index] ? ' · 찍음' : '');
      elTooltip.appendChild(kind);
      for (var i = 0; i < n.s.length; i++) {
        var stat = document.createElement('div');
        stat.className = 'stat';
        stat.textContent = n.s[i];
        elTooltip.appendChild(stat);
      }
      elTooltip.hidden = false;
    }
    var gap = 18 * view.ui, edge = 8 * view.ui;
    var left = pointer.x + gap, top = pointer.y + gap;
    var box = elTooltip.getBoundingClientRect();
    if (left + box.width > view.width - edge) { left = Math.max(edge, pointer.x - box.width - gap); }
    if (top + box.height > view.height - edge) { top = Math.max(edge, view.height - box.height - edge); }
    elTooltip.style.left = left + 'px';
    elTooltip.style.top = top + 'px';
  }

  function hideTooltip() {
    if (tooltipFor !== -1) {
      tooltipFor = -1;
      elTooltip.hidden = true;
    }
  }

  // ---------------------------------------------------------------- 검색
  var searchText = null;
  var matchCursor = -1;

  // 조합형 입력(한글 등)의 상태. Prisma UI는 표준 조합 이벤트를 보내지 않고 창에 'prismaIME_state' 이벤트로 알려 준다.
  // 조합 중인 글자는 입력창에 들어오지 않고, 확정된 글자만 보통의 글자 입력으로 들어온다.
  var elIme = document.getElementById('ime');
  var ime = { active: false, composition: '', caret: 0, candidates: [], selectedIndex: -1 };

  // 조합 중인 글자나 후보가 실제로 있을 때만 참. Prisma UI는 후보 창이 닫힐 때도 active를 참으로 알려 오므로
  // active만 보면 조합이 끝난 뒤에도 Enter와 Esc가 막힐 수 있다.
  function imeBusy() {
    return ime.composition !== '' || ime.candidates.length > 0;
  }

  // 검색어. 검색창에서 조합 중인 글자가 있으면 커서 자리에 끼워 넣은 것으로 본다(조합 중에도 결과가 따라오게).
  function searchQuery() {
    var value = elSearch.value;
    if (ime.composition && document.activeElement === elSearch) {
      var at = typeof elSearch.selectionStart === 'number' ? elSearch.selectionStart : value.length;
      value = value.slice(0, at) + ime.composition + value.slice(at);
    }
    return value.trim().toLowerCase();
  }

  function renderIme() {
    var show = document.activeElement === elSearch && (ime.composition !== '' || ime.candidates.length > 0);
    if (!show) {
      elIme.hidden = true;
      return;
    }
    elIme.textContent = '';
    var line = document.createElement('div');
    var caretAt = Math.min(Math.max(ime.caret, 0), ime.composition.length);
    line.appendChild(document.createTextNode(ime.composition.slice(0, caretAt)));
    var caret = document.createElement('span');
    caret.className = 'caret';
    line.appendChild(caret);
    line.appendChild(document.createTextNode(ime.composition.slice(caretAt)));
    elIme.appendChild(line);
    if (ime.candidates.length) {
      var list = document.createElement('div');
      list.className = 'candidates';
      for (var i = 0; i < ime.candidates.length; i++) {
        var item = document.createElement('span');
        item.textContent = (i + 1) + ' ' + ime.candidates[i];
        if (i === ime.selectedIndex) { item.className = 'selected'; }
        list.appendChild(item);
      }
      elIme.appendChild(list);
    }
    var box = elSearch.getBoundingClientRect();
    elIme.style.left = box.left + 'px';
    elIme.style.top = (box.bottom + 4 * view.ui) + 'px';
    elIme.hidden = false;
  }

  function clearIme() {
    ime = { active: false, composition: '', caret: 0, candidates: [], selectedIndex: -1 };
    elIme.hidden = true;
  }

  window.addEventListener('prismaIME_state', function (event) {
    var detail = event.detail || {};
    var before = ime.composition;
    ime = {
      active: !!detail.active,
      composition: typeof detail.composition === 'string' ? detail.composition : '',
      caret: typeof detail.caret === 'number' ? detail.caret : 0,
      candidates: Array.isArray(detail.candidates) ? detail.candidates.map(String) : [],
      selectedIndex: typeof detail.selectedIndex === 'number' ? detail.selectedIndex : -1,
    };
    renderIme();
    if (ime.composition !== before) {
      if (document.activeElement === elSearch) { runSearch(); }
      reportInput("조합 '" + ime.composition + "'" + (ime.active ? '' : ' (끝)') + (ime.candidates.length ? ', 후보 ' + ime.candidates.length + '개' : ''));
    }
  });

  function runSearch() {
    var query = searchQuery();
    var started = performance.now();
    matched.fill(0);
    matchList = [];
    matchCursor = -1;
    if (query) {
      if (!searchText) {
        searchText = nodes.map(function (n) {
          return (n.n + '\n' + n.s.join('\n') + '\n' + KIND_LABEL[n.k] + (n.asc ? '\n' + n.asc : '')).toLowerCase();
        });
      }
      for (var i = 0; i < nodeCount; i++) {
        if (searchText[i].indexOf(query) >= 0) {
          matched[i] = 1;
          matchList.push(i);
        }
      }
    }
    var elapsed = performance.now() - started;
    elSearchCount.textContent = query ? matchList.length + '개 (' + elapsed.toFixed(1) + 'ms)' : '';
    dirty = true;
    return elapsed;
  }

  // 브라우저에서는 조합 중에도 input 이벤트가 오므로 그때마다 찾는다. 조합이 끝날 때도 한 번 더 찾는다.
  // Prisma UI에서는 확정된 글자가 input 이벤트로 오고, 조합 중인 글자는 위의 prismaIME_state로 온다.
  elSearch.addEventListener('input', function () {
    runSearch();
    reportInput("입력값 '" + elSearch.value + "', 결과 " + matchList.length + '개');
  });
  elSearch.addEventListener('compositionend', runSearch);
  elSearch.addEventListener('focus', function () { renderIme(); reportInput('포커스를 받음'); });
  elSearch.addEventListener('blur', function () { elIme.hidden = true; reportInput('포커스를 잃음'); });
  elSearch.addEventListener('keydown', function (event) {
    // 조합 중의 Enter, Esc, 방향키는 입력기의 것이다. 여기서 받으면 조합이 끝나기 전에 검색이 넘어가거나 지워진다.
    if (imeBusy() || event.isComposing) { return; }
    if (event.key === 'Enter' && matchList.length) {
      matchCursor = (matchCursor + 1) % matchList.length;
      var index = matchList[matchCursor];
      camera.x = nodeX[index];
      camera.y = nodeY[index];
      camera.scale = Math.max(camera.scale, 0.3);
      clampCamera();
      dirty = true;
    } else if (event.key === 'Escape') {
      elSearch.value = '';
      runSearch();
      elSearch.blur();
    }
  });

  // ---------------------------------------------------------------- 시험용 조작
  // 연결된 노드를 따라 임의로 123개를 찍는다. 실제 규칙(시작점, 전직)은 따르지 않고 찍힌 경로의 모양만 흉내 낸다.
  function allocateDemo(points) {
    allocated.fill(0);
    var candidates = [];
    for (var i = 0; i < nodeCount; i++) {
      if (nodeKind[i] === KIND.NORMAL && !nodes[i].asc && adjacency[i].length >= 2) { candidates.push(i); }
    }
    if (!candidates.length) { rebuildAllocated(); return; }
    var seed = candidates[Math.floor(Math.random() * candidates.length)];
    var frontier = [seed];
    var count = 0;
    while (frontier.length && count < points) {
      var pick = frontier.splice(Math.floor(Math.random() * frontier.length), 1)[0];
      if (allocated[pick] || nodes[pick].asc || nodeKind[pick] === KIND.MASTERY) { continue; }
      allocated[pick] = 1;
      count++;
      for (var j = 0; j < adjacency[pick].length; j++) {
        if (!allocated[adjacency[pick][j]]) { frontier.push(adjacency[pick][j]); }
      }
    }
    rebuildAllocated();
  }

  function setMode(mode) {
    settings.mode = mode === 'direct' ? 'direct' : 'tiles';
    elMode.textContent = '방식: ' + (settings.mode === 'tiles' ? '타일' : '직접');
    frameTimes.length = 0;
    dirty = true;
  }

  var elScale = document.getElementById('btn-scale');

  function setRenderScale(scale) {
    scale = Math.min(1, Math.max(0.25, scale));
    if (scale !== settings.renderScale) {
      settings.renderScale = scale;
      resize();
      frameTimes.length = 0;
    }
    elScale.textContent = '해상도: ' + (scale === 1 ? '전체' : Math.round(scale * 100) + '%');
  }

  elMode.addEventListener('click', function () { setMode(settings.mode === 'tiles' ? 'direct' : 'tiles'); });
  elScale.addEventListener('click', function () {
    // 측정이 도는 중에는 측정이 해상도를 정한다.
    if (!suiteStage) {
      setRenderScale(settings.renderScale === 1 ? 0.5 : 1);
    }
  });
  document.getElementById('btn-alloc').addEventListener('click', function () { allocateDemo(123); });
  document.getElementById('btn-clear').addEventListener('click', function () { allocated.fill(0); rebuildAllocated(); });
  document.getElementById('btn-fit').addEventListener('click', fitMain);
  elBenchButton.addEventListener('click', function () { runBench(); });

  // ---------------------------------------------------------------- 측정
  // 정해진 경로로 화면을 움직이며 프레임 간격과 그리는 데 든 시간을 모은다.
  // 구간: 멀리서 가로지르기 → 가운데로 확대 → 가까이서 가로지르기 → 축소 → 확대·축소 반복.
  var BENCH_SEGMENTS = [
    { name: '멀리서 가로지르기', seconds: 2.5, at: function (t, b) { return { x: lerp(b.minX, b.maxX, t), y: lerp(b.minY, b.maxY, t) * 0.4, scale: 0.07 }; } },
    { name: '확대', seconds: 2.5, at: function (t, b) { return { x: lerp(b.maxX, 0, t), y: 0, scale: expLerp(0.07, 0.45, t) }; } },
    { name: '가까이서 가로지르기', seconds: 4, at: function (t, b) { return { x: lerp(b.minX * 0.7, b.maxX * 0.7, t), y: Math.sin(t * Math.PI * 2) * b.maxY * 0.5, scale: 0.45 }; } },
    { name: '축소', seconds: 2.5, at: function (t, b) { return { x: lerp(b.maxX * 0.7, 0, t), y: 0, scale: expLerp(0.45, 0.06, t) }; } },
    { name: '확대·축소 반복', seconds: 3, at: function (t) { return { x: 0, y: 0, scale: expLerp(0.06, 0.5, 0.5 - 0.5 * Math.cos(t * Math.PI * 4)) }; } },
  ];

  function lerp(a, b, t) { return a + (b - a) * t; }
  function expLerp(a, b, t) { return a * Math.pow(b / a, t); }

  function percentile(sorted, p) {
    if (!sorted.length) { return 0; }
    return sorted[Math.min(sorted.length - 1, Math.floor(sorted.length * p))];
  }

  function summarize(values) {
    var sorted = values.slice().sort(function (a, b) { return a - b; });
    var sum = 0;
    for (var i = 0; i < sorted.length; i++) { sum += sorted[i]; }
    return {
      count: sorted.length,
      avg: sorted.length ? sum / sorted.length : 0,
      p50: percentile(sorted, 0.5),
      p95: percentile(sorted, 0.95),
      p99: percentile(sorted, 0.99),
      max: sorted.length ? sorted[sorted.length - 1] : 0,
    };
  }

  function runBench(options) {
    if (bench) { return bench.promise; }
    options = options || {};
    if (options.mode) { setMode(options.mode); }
    if (options.clearTiles !== false) { tiles.clear(); }
    if (options.allocate !== false && !allocatedList.length) { allocateDemo(123); }
    hideTooltip();
    hovered = -1;
    elBench.hidden = true;
    elBenchButton.disabled = true;
    var saved = { x: camera.x, y: camera.y, scale: camera.scale };
    var resolveDone;
    bench = {
      started: 0,
      segment: 0,
      segmentStarted: 0,
      recording: true,
      intervals: [],
      renders: [],
      perSegment: [],
      mark: 0,
      tilesBefore: counters.tilesRendered,
      saved: saved,
      promise: new Promise(function (resolve) { resolveDone = resolve; }),
      resolve: function (result) { resolveDone(result); },
    };
    dirty = true;
    return bench.promise;
  }

  // 도는 중인 측정을 버린다. 기다리던 쪽에는 null이 간다.
  function cancelBench() {
    if (!bench) { return; }
    var cancelled = bench;
    bench = null;
    camera.x = cancelled.saved.x;
    camera.y = cancelled.saved.y;
    camera.scale = cancelled.saved.scale;
    dirty = true;
    elBenchButton.disabled = false;
    cancelled.resolve(null);
  }

  function stepBench(now) {
    if (!bench.started) {
      bench.started = now;
      bench.segmentStarted = now;
    }
    var segment = BENCH_SEGMENTS[bench.segment];
    var t = (now - bench.segmentStarted) / (segment.seconds * 1000);
    if (t >= 1) {
      bench.perSegment.push({
        name: segment.name,
        interval: summarize(bench.intervals.slice(bench.mark)),
        render: summarize(bench.renders.slice(bench.mark)),
      });
      bench.mark = bench.intervals.length;
      bench.segment++;
      bench.segmentStarted = now;
      if (bench.segment >= BENCH_SEGMENTS.length) {
        finishBench(now);
        return;
      }
      segment = BENCH_SEGMENTS[bench.segment];
      t = 0;
    }
    var next = segment.at(t, data.mainBounds);
    camera.x = next.x;
    camera.y = next.y;
    camera.scale = next.scale;
    clampCamera();
    dirty = true;
  }

  function finishBench(now) {
    var finished = bench;
    bench = null;
    var interval = summarize(finished.intervals);
    var render = summarize(finished.renders);
    var seconds = (now - finished.started) / 1000;
    // 가장 느린 1%의 프레임 간격으로 본 프레임 수("1% low").
    var result = {
      mode: settings.mode,
      lod: settings.lod,
      width: view.width,
      height: view.height,
      dpr: view.dpr,
      canvas: canvas.width + 'x' + canvas.height,
      renderScale: settings.renderScale,
      nodes: nodeCount,
      edges: edges.length,
      allocated: allocatedList.length,
      seconds: +seconds.toFixed(2),
      frames: interval.count,
      fpsAvg: +(interval.avg ? 1000 / interval.avg : 0).toFixed(1),
      fpsLow1: +(interval.p99 ? 1000 / interval.p99 : 0).toFixed(1),
      intervalMs: roundAll(interval),
      renderMs: roundAll(render),
      tilesRendered: counters.tilesRendered - finished.tilesBefore,
      tilesKept: tiles.size,
      segments: finished.perSegment.map(function (s) {
        return {
          name: s.name,
          fpsAvg: +(s.interval.avg ? 1000 / s.interval.avg : 0).toFixed(1),
          intervalP95: +s.interval.p95.toFixed(1),
          renderAvg: +s.render.avg.toFixed(2),
          renderP95: +s.render.p95.toFixed(2),
          renderMax: +s.render.max.toFixed(2),
        };
      }),
      userAgent: navigator.userAgent,
    };
    camera.x = finished.saved.x;
    camera.y = finished.saved.y;
    camera.scale = finished.saved.scale;
    dirty = true;
    window.__benchResult = result;
    showBench(result);
    elBenchButton.disabled = false;
    try { console.log('[PoE Proto 4] bench ' + JSON.stringify(result)); } catch (error) { /* 콘솔이 없는 환경 */ }
    finished.resolve(result);
  }

  function roundAll(summary) {
    return {
      count: summary.count,
      avg: +summary.avg.toFixed(2),
      p50: +summary.p50.toFixed(2),
      p95: +summary.p95.toFixed(2),
      p99: +summary.p99.toFixed(2),
      max: +summary.max.toFixed(2),
    };
  }

  function showBench(result) {
    var lines = [
      '측정 결과 (' + (result.mode === 'tiles' ? '타일' : '직접') + ' 방식, 화면 ' + result.width + 'x' + result.height + ', 그림 ' + result.canvas + ')',
      '평균 ' + result.fpsAvg + ' FPS, 가장 느린 1% ' + result.fpsLow1 + ' FPS, 프레임 ' + result.frames + '개 / ' + result.seconds + '초',
      '프레임 간격(ms): 평균 ' + result.intervalMs.avg + ', 중앙 ' + result.intervalMs.p50 + ', 95% ' + result.intervalMs.p95 + ', 99% ' + result.intervalMs.p99 + ', 최대 ' + result.intervalMs.max,
      '그리는 시간(ms): 평균 ' + result.renderMs.avg + ', 95% ' + result.renderMs.p95 + ', 99% ' + result.renderMs.p99 + ', 최대 ' + result.renderMs.max,
      '새로 그린 타일 ' + result.tilesRendered + '장, 보관 ' + result.tilesKept + '장',
      '',
    ];
    result.segments.forEach(function (s) {
      lines.push(s.name + ': ' + s.fpsAvg + ' FPS, 간격 95% ' + s.intervalP95 + 'ms, 그리기 평균 ' + s.renderAvg + ' / 95% ' + s.renderP95 + ' / 최대 ' + s.renderMax + 'ms');
    });
    elBench.textContent = lines.join('\n');
    elBench.hidden = false;
  }

  // 화면 갱신 주기와 무관하게, 같은 경로를 1/60초 간격으로 밟으며 프레임 하나를 그리는 데 든 시간만 잰다.
  // 창이 가려져 갱신이 느려지는 환경에서도 잴 수 있고, 60 FPS에 필요한 16.7ms 안에 드는지를 바로 볼 수 있다.
  // every는 몇 프레임마다 하나를 그릴지(1이면 전부). cpu=1일 때는 그린 결과를 읽어 실제로 다 그려지게 한다.
  function runSyncBench(options) {
    options = options || {};
    if (bench) { return null; }
    if (options.mode) { setMode(options.mode); }
    if (options.clearTiles !== false) { tiles.clear(); }
    if (options.allocate !== false && !allocatedList.length) { allocateDemo(123); }
    var every = Math.max(1, options.every || 1);
    var saved = { x: camera.x, y: camera.y, scale: camera.scale };
    var tilesBefore = counters.tilesRendered;
    var all = [];
    var segments = [];
    var started = performance.now();
    for (var s = 0; s < BENCH_SEGMENTS.length; s++) {
      var segment = BENCH_SEGMENTS[s];
      var frames = Math.round(segment.seconds * 60);
      var times = [];
      for (var f = 0; f < frames; f += every) {
        var next = segment.at(f / frames, data.mainBounds);
        camera.x = next.x;
        camera.y = next.y;
        camera.scale = next.scale;
        clampCamera();
        var t0 = performance.now();
        render();
        if (settings.cpu) { ctx.getImageData(0, 0, 1, 1); }
        times.push(performance.now() - t0);
      }
      var summary = summarize(times);
      segments.push({
        name: segment.name,
        frames: summary.count,
        avg: +summary.avg.toFixed(2),
        p95: +summary.p95.toFixed(2),
        max: +summary.max.toFixed(2),
        over16: times.filter(function (t) { return t > 16.7; }).length,
      });
      all = all.concat(times);
    }
    var total = summarize(all);
    var result = {
      kind: 'sync',
      mode: settings.mode,
      cpu: settings.cpu,
      lod: settings.lod,
      width: view.width,
      height: view.height,
      dpr: view.dpr,
      nodes: nodeCount,
      allocated: allocatedList.length,
      frames: total.count,
      renderMs: roundAll(total),
      over16: all.filter(function (t) { return t > 16.7; }).length,
      over33: all.filter(function (t) { return t > 33.3; }).length,
      tilesRendered: counters.tilesRendered - tilesBefore,
      tilesKept: tiles.size,
      wallSeconds: +((performance.now() - started) / 1000).toFixed(2),
      segments: segments,
    };
    camera.x = saved.x;
    camera.y = saved.y;
    camera.scale = saved.scale;
    dirty = true;
    window.__benchResult = result;
    return result;
  }

  // ---------------------------------------------------------------- 게임 안(Prisma UI)
  // DLL(plugin/src/main.cpp)이 화면에 전역 함수 poeTreeReport, poeTreeCommand를 만들어 두고,
  // 화면이 열릴 때 poeTreeOnShow(단계 이름)를 부른다. 브라우저에서는 이 함수들이 없으므로 아무 일도 하지 않는다.
  var elGameBar = document.getElementById('game-bar');
  var elGameStatus = document.getElementById('game-status');
  var suiteStage = null;   // 측정 묶음이 도는 중이면 그 단계의 이름
  var suiteToken = 0;      // 묶음을 새로 시작하거나 버릴 때마다 올린다. 옛 묶음의 남은 단계가 스스로 멈추게 하는 표시
  var SUITE_ABORTED = {};
  var measuredStages = {};  // 이번 실행에서 측정을 끝낸 단계. 다시 열면 재지 않고 바로 만져 볼 수 있게 한다
  var shownStage = null;    // 지금 열려 있는 단계의 이름. 닫혀 있으면 null

  function inGame() {
    return typeof window.poeTreeReport === 'function';
  }

  function reportToGame(text) {
    if (inGame()) {
      try { window.poeTreeReport(String(text)); } catch (error) { /* 보낼 곳이 사라진 경우 */ }
    }
  }

  // 검색창에서 일어난 일을 게임 쪽 로그에 남긴다. 한글 입력이 어디까지 되는지를 로그로 가르기 위한 것이다.
  // 친 글자가 로그에 그대로 남으므로 INI의 LogSearchInput을 켰을 때만 한다.
  var logSearchInput = false;
  function reportInput(text) {
    if (logSearchInput) {
      reportToGame('검색창: ' + text);
    }
  }

  // DLL이 부른다. "이름=값" 꼴의 설정 하나.
  window.poeTreeSetOption = function (argument) {
    var text = String(argument || '');
    var at = text.indexOf('=');
    var name = at < 0 ? text : text.slice(0, at);
    var value = at < 0 ? '' : text.slice(at + 1);
    if (name === 'logSearchInput') {
      logSearchInput = value === 'true';
    } else {
      reportToGame("모르는 설정: '" + text + "'");
    }
  };

  // 게임 쪽에 다음 단계로 넘겨 달라고 한다. 지금 단계를 함께 보내, 늦게 도착했을 때(그 사이에 사용자가 넘기거나 닫았을 때)
  // 게임 쪽이 버릴 수 있게 한다.
  function requestNext() {
    if (shownStage) {
      sendCommand('next:' + shownStage);
    }
  }

  function sendCommand(command) {
    if (typeof window.poeTreeCommand === 'function') {
      try { window.poeTreeCommand(command); } catch (error) { /* 보낼 곳이 사라진 경우 */ }
    }
  }

  function setGameStatus(text) {
    elGameBar.hidden = false;
    elGameStatus.textContent = text;
  }

  function environment() {
    return {
      userAgent: navigator.userAgent,
      innerWidth: window.innerWidth,
      innerHeight: window.innerHeight,
      devicePixelRatio: window.devicePixelRatio,
      canvas: canvas.width + 'x' + canvas.height,
      renderScale: settings.renderScale,
      offscreenCanvas: typeof OffscreenCanvas !== 'undefined',
      cores: navigator.hardwareConcurrency || 0,
      imagesLoadMs: window.__loadMs,
      missingImages: missingImages.slice(),
      nodes: nodeCount,
      edges: edges.length,
      maxTiles: tileLimit,
      uiScale: view.ui,
      lod: settings.lod,
    };
  }

  function wait(ms) {
    return new Promise(function (resolve) { setTimeout(resolve, ms); });
  }

  // 사람이 읽는 요약 줄. 로그에서 바로 통과 기준과 견줄 수 있게 한다.
  function describeRaf(name, r) {
    return name + ' ' + r.canvas + ': 평균 ' + r.fpsAvg + ' FPS, 가장 느린 1% ' + r.fpsLow1 + ' FPS, 프레임 ' + r.frames + '개/' + r.seconds +
      '초, 간격 평균 ' + r.intervalMs.avg + ' 중앙 ' + r.intervalMs.p50 + ' 95% ' + r.intervalMs.p95 + ' 99% ' + r.intervalMs.p99 +
      ' 최대 ' + r.intervalMs.max + 'ms, 그리기 평균 ' + r.renderMs.avg + ' 95% ' + r.renderMs.p95 + ' 최대 ' + r.renderMs.max +
      'ms, 새 타일 ' + r.tilesRendered + '장';
  }

  function describeFrames(name, r) {
    return name + ': 평균 ' + r.fpsAvg + ' FPS, 프레임 ' + r.frames + '개/' + r.seconds + '초, 간격 평균 ' + r.intervalMs.avg +
      ' 중앙 ' + r.intervalMs.p50 + ' 95% ' + r.intervalMs.p95 + ' 최대 ' + r.intervalMs.max + 'ms, 그 사이 트리를 다시 그린 횟수 ' + r.treeRenders;
  }

  // 트리를 움직이지 않고 화면 갱신 간격만 잰다. 게임 자신의 프레임 시간을 알기 위한 기준이다.
  // touch가 참이면 프레임마다 캔버스의 점 몇 개만 바꾼다. 화면이 조금이라도 바뀌면 드는 고정 비용(화면 전체 복사)을 보려는 것이다.
  function measureFrames(options) {
    return new Promise(function (resolve, reject) {
      var intervals = [];
      var started = 0, last = 0, flip = false;
      var rendersBefore = renderCount;
      function step(now) {
        if (options.token !== suiteToken) {
          reject(SUITE_ABORTED);
          return;
        }
        if (!started) { started = now; }
        if (last) { intervals.push(now - last); }
        last = now;
        if (now - started >= options.seconds * 1000) {
          var summary = summarize(intervals);
          resolve({
            touch: !!options.touch,
            seconds: +((now - started) / 1000).toFixed(2),
            frames: summary.count,
            fpsAvg: +(summary.avg ? 1000 / summary.avg : 0).toFixed(1),
            intervalMs: roundAll(summary),
            treeRenders: renderCount - rendersBefore,
          });
          return;
        }
        if (options.touch) {
          flip = !flip;
          ctx.setTransform(1, 0, 0, 1, 0, 0);
          ctx.fillStyle = flip ? '#0b0a08' : '#0c0b09';
          ctx.fillRect(0, 0, 2, 2);
        }
        nextFrame(step);
      }
      nextFrame(step);
    });
  }

  // 타일을 다 그려 화면이 더 바뀌지 않을 때까지 기다린다(길어도 3초).
  function waitUntilQuiet(token) {
    return new Promise(function (resolve, reject) {
      var started = 0, quiet = 0;
      function step(now) {
        if (token !== suiteToken) {
          reject(SUITE_ABORTED);
          return;
        }
        if (!started) { started = now; }
        quiet = dirty ? 0 : quiet + 1;
        if (quiet >= 5 || now - started > 3000) {
          resolve();
          return;
        }
        nextFrame(step);
      }
      nextFrame(step);
    });
  }

  // 도는 중인 측정 묶음을 버리고 화면을 평소 상태로 되돌린다.
  function abortSuite(reason) {
    if (!suiteStage) { return; }
    var stage = suiteStage;
    suiteStage = null;
    suiteToken++;
    cancelBench();
    setRenderScale(1);
    setMode('tiles');
    reportToGame('[' + stage + '] 측정 중단: ' + reason);
  }

  // 한 단계의 측정 묶음(약 36초). 모두 화면 갱신에 맞춰 재므로 게임을 세우지 않는다.
  //   1. 기준: 트리를 그리지 않을 때의 프레임 간격(게임 자신의 몫)
  //   2. 기준: 캔버스의 점 몇 개만 바꿀 때(화면이 바뀌면 드는 고정 비용)
  //   3. 타일 방식, 화면 그대로의 해상도
  //   4. 타일 방식, 가로·세로 절반 해상도
  // 도중에 단계가 바뀌거나 화면이 닫히면 버리고, 새 단계에서 처음부터 다시 잰다.
  function runGameSuite(stage) {
    abortSuite('단계가 바뀜');
    cancelBench();  // 화면의 "측정" 버튼으로 돌리던 것이 있으면 그 결과가 이 묶음의 것으로 섞이지 않게 버린다
    var token = ++suiteToken;
    suiteStage = stage;
    var out = { stage: stage, env: null };
    function alive() {
      if (token !== suiteToken) { throw SUITE_ABORTED; }
    }
    function status(step) {
      setGameStatus(stageLabel(stage) + ' · 측정 중 (' + step + ') · F6을 누르면 이 측정을 버리고 넘어갑니다');
    }
    function extra(result) {
      return +(result.intervalMs.avg - out.idle.intervalMs.avg).toFixed(2);
    }
    setMode('tiles');
    setRenderScale(1);
    resize();
    fitMain();
    if (!allocatedList.length) { allocateDemo(123); }
    out.env = environment();
    reportToGame('[' + stage + '] 측정 시작. 환경 ' + JSON.stringify(out.env));
    status('1/4 기준');
    waitUntilQuiet(token).then(function () {
      alive();
      return measureFrames({ seconds: 3, touch: false, token: token });
    }).then(function (result) {
      alive();
      out.idle = result;
      reportToGame('[' + stage + '] ' + describeFrames('기준(트리를 그리지 않음)', result));
      status('2/4 기준');
      return measureFrames({ seconds: 3, touch: true, token: token });
    }).then(function (result) {
      alive();
      out.touch = result;
      dirty = true;
      reportToGame('[' + stage + '] ' + describeFrames('기준(점 몇 개만 바꿈)', result));
      status('3/4 타일, 전체 해상도');
      return runBench({ mode: 'tiles' });
    }).then(function (result) {
      alive();
      out.tilesFull = result;
      reportToGame('[' + stage + '] ' + describeRaf('타일', result));
      setRenderScale(0.5);
      status('4/4 타일, 절반 해상도');
      return runBench({ mode: 'tiles' });
    }).then(function (result) {
      alive();
      out.tilesHalf = result;
      reportToGame('[' + stage + '] ' + describeRaf('타일', result));
      setRenderScale(1);
      fitMain();
      elBench.hidden = true;  // 결과는 로그에 남는다. 화면에 남겨 두면 직접 만져 볼 때 트리를 가린다
      reportToGame('[' + stage + '] 검색: ' + runSearchProbe());
      out.extraMs = { touch: extra(out.touch), tilesFull: extra(out.tilesFull), tilesHalf: extra(out.tilesHalf) };
      reportToGame('[' + stage + '] 요약: 기준 간격 ' + out.idle.intervalMs.avg + 'ms(' + out.idle.fpsAvg + ' FPS). 거기에 더해진 시간: 점 몇 개만 바꿈 ' +
        out.extraMs.touch + 'ms, 타일 ' + out.tilesFull.canvas + ' ' + out.extraMs.tilesFull + 'ms(' + out.tilesFull.fpsAvg + ' FPS), 타일 ' +
        out.tilesHalf.canvas + ' ' + out.extraMs.tilesHalf + 'ms(' + out.tilesHalf.fpsAvg + ' FPS)');
      reportToGame('[' + stage + '] 자세한 값 ' + JSON.stringify(out));
      reportToGame('[' + stage + '] 측정 끝');
      suiteStage = null;
      measuredStages[stage] = true;
      if (stage === 'paused' && !measuredStages.live) {
        // 사람이 눌러 주기를 기다리지 않고 이어서 잰다. 게임 쪽이 단계를 바꾸고 poeTreeOnShow('live')를 부른다.
        setGameStatus(stageLabel(stage) + ' · 측정 끝. 이어서 게임을 돌린 채 잽니다');
        requestNext();
      } else {
        setGameStatus(stageLabel(stage) + ' · 측정이 모두 끝났습니다. 닫았다가(F6) 다시 열면 측정 없이 만져 볼 수 있습니다');
      }
    }).catch(function (error) {
      if (error === SUITE_ABORTED) {
        return;  // abortSuite가 이미 정리했다
      }
      reportToGame('[' + stage + '] 측정 중 오류: ' + (error && error.stack ? error.stack : error));
      if (token === suiteToken) {
        // 아직 이 묶음이 화면의 주인일 때만 정리하고 알린다. 새 묶음이 돌고 있으면 그쪽의 표시를 건드리지 않는다.
        suiteStage = null;
        cancelBench();
        setRenderScale(1);
        setGameStatus(stageLabel(stage) + ' · 측정 중 오류(로그 확인)');
      }
    });
  }

  function runSearchProbe() {
    var parts = [];
    ['키스톤', 'maximum life', 'resolute technique'].forEach(function (query) {
      elSearch.value = query;
      var ms = runSearch();
      parts.push("'" + query + "' " + matchList.length + '개 ' + ms.toFixed(1) + 'ms');
    });
    elSearch.value = '';
    runSearch();
    return parts.join(', ');
  }

  function stageLabel(stage) {
    return stage === 'paused' ? '게임을 멈추고 엶' : (stage === 'live' ? '게임을 돌린 채 엶' : stage);
  }

  // DLL이 부른다. 단계 이름 앞에 '-'가 붙어 있으면 측정하지 않고 열기만 한다(INI의 AutoBench = false).
  // 측정이 도는 중에 다른 단계로 넘어오면 하던 측정을 버리고 새 단계에서 다시 잰다.
  window.poeTreeOnShow = function (argument) {
    var text = String(argument || '');
    var skip = text.charAt(0) === '-';
    var stage = skip ? text.slice(1) : text;
    shownStage = stage;
    suspended = false;
    startLoop();
    if (skip) {
      abortSuite('측정 없이 다시 열림');
      resize();
      setGameStatus(stageLabel(stage));
      reportToGame('[' + stage + '] 열림(측정 안 함). 환경 ' + JSON.stringify(environment()));
    } else if (measuredStages[stage]) {
      abortSuite('이미 잰 단계로 넘어감');
      resize();
      setGameStatus(stageLabel(stage) + ' · 이 단계는 이미 쟀습니다. 직접 만져 보세요');
      reportToGame('[' + stage + '] 열림(이번 실행에서 이미 잼)');
    } else {
      runGameSuite(stage);
    }
  };

  // DLL이 부른다. 화면이 닫혔다. 숨긴 화면도 스크립트는 계속 돌고 그 시간만큼 게임의 프레임이 늦어지므로, 하던 일을 모두 멈춘다.
  window.poeTreeOnHide = function () {
    abortSuite('화면이 닫힘');
    drag = null;
    canvas.classList.remove('dragging');
    hideTooltip();
    clearIme();
    elSearch.blur();
    shownStage = null;
    suspended = true;
    // 닫혀 있는 동안에는 타일을 쥐고 있지 않는다(한 장에 약 1MB, 한도까지 차면 160MB가 넘는다). 다시 열면 몇 프레임에 걸쳐 다시 만든다.
    tiles.clear();
    dirty = true;
    setGameStatus('닫힘');
    reportToGame('닫힘: 화면 갱신을 멈춤');
  };

  // 측정 도중에 눌러도 된다. 게임 쪽이 다음 단계를 알려 오면 하던 측정을 버리고 새로 잰다.
  document.getElementById('btn-next').addEventListener('click', requestNext);
  document.getElementById('btn-close').addEventListener('click', function () { sendCommand('close'); });
  window.addEventListener('keydown', function (event) {
    if (event.key === 'Escape' && inGame() && document.activeElement !== elSearch && !imeBusy()) {
      sendCommand('close');
    }
  });

  // 화면의 오류가 게임 쪽 로그에 남게 한다.
  window.addEventListener('error', function (event) {
    reportToGame('화면 오류: ' + event.message + ' (' + event.filename + ':' + event.lineno + ')');
  });
  // 한글 입력 확인용: 조합이 시작되고 끝날 때의 글자를 게임 쪽 로그에 남긴다.
  elSearch.addEventListener('compositionstart', function () { reportInput('표준 조합 이벤트 시작'); });
  elSearch.addEventListener('compositionend', function (event) {
    reportInput("표준 조합 이벤트 끝 '" + event.data + "', 입력값 '" + elSearch.value + "'");
  });

  // 밖에서(브라우저 도구나 DLL 쪽에서) 부를 수 있게 내놓는다.
  window.PoETree = {
    runBench: runBench,
    runSyncBench: runSyncBench,
    setMode: setMode,
    setRenderScale: setRenderScale,
    allocateDemo: allocateDemo,
    search: function (text) { elSearch.value = text; return { ms: runSearch(), matches: matchList.length }; },
    info: function () {
      return { nodes: nodeCount, edges: edges.length, groups: groups.length, mode: settings.mode, tiles: tiles.size,
               scale: camera.scale, width: view.width, height: view.height, dpr: view.dpr };
    },
  };

  // ---------------------------------------------------------------- 시작
  window.addEventListener('resize', resize);
  setMode(settings.mode);
  setRenderScale(settings.renderScale);
  resize();
  fitMain();
  var loadStarted = performance.now();
  loadImages().then(function () {
    window.__loadMs = +(performance.now() - loadStarted).toFixed(1);
    // 그림이 다 오기 전에 화면이 열렸다면 그림 없이 만든 타일이 있다. 버리고 다시 만들게 한다.
    tiles.clear();
    dirty = true;
    if (missingImages.length) {
      var text = '불러오지 못한 그림: ' + missingImages.join(', ');
      showMessage(text + '\n해당 그림을 쓰는 부분은 빠진 채로 그립니다. tools/build_site.py 를 다시 실행해 보세요.');
      setTimeout(function () { elMessage.hidden = true; }, 6000);
      reportToGame(text);
    }
    startLoop();
    if (params.get('alloc') === '1') { allocateDemo(123); }
    if (params.get('bench') === '1') { setTimeout(function () { runBench(); }, 500); }
  });
})();
