"use strict";

const assert = require("node:assert/strict");
const { readFileSync } = require("node:fs");
const { join } = require("node:path");
const { isDeepStrictEqual } = require("node:util");
const { createTreeRules } = require("../../ui/tree.js");

if (process.argv.length !== 3) {
  throw new Error("Usage: node tools/ui-preview/test_tree_parity.js <C++ fixture.json>");
}
const readJson = (path) => JSON.parse(readFileSync(path, "utf8"));
const packDir = join(__dirname, "../../data/packs");
const rulesByGeneration = ["gen0/tree.json", "gen1/tree.json"]
  .map((path) => createTreeRules(readJson(join(packDir, path))));
const cases = readJson(process.argv[2]);
assert.deepEqual(new Set(cases.map(({ packGeneration }) => packGeneration)), new Set([0, 1]));
assert.ok(cases.some(({ packGeneration, state }) => packGeneration !== state.generation));

for (const probe of cases) {
  const { state, name, target, refundNode, mastery, settings } = probe;
  const rules = rulesByGeneration[probe.packGeneration];
  const path = rules.computeShortestPath(state, target);
  const candidate = probe.paths.find((entry) => isDeepStrictEqual(entry.path, path));
  assert.ok(candidate, `${name} / C++-validated path: ${JSON.stringify(path)}`);

  const node = rules.nodeById.get(target);
  const view = rules.resolveNode(node, state);
  assert.deepEqual({ name: view.name, stats: view.stats || [], asc: view.asc || "" }, probe.view, `${name} / view`);
  assert.equal(rules.isAllocatedJewelSocket(state, target), probe.allocatedJewelSocket, `${name} / socket`);
  assert.equal(10 * rules.jewelEffectMultiplier(state, target), probe.jewelStrength, `${name} / jewel strength`);
  assert.equal(rules.isMasteryUnlocked(rules.nodeById.get(mastery), state, new Set(state.allocatedNodes)),
    probe.masteryUnlocked, `${name} / mastery lock`);

  assert.deepEqual({ ...state, ...rules.pointTotals(state, probe.points) }, state, `${name} / absolute points`);
  function after(plan) {
    if (!plan) return null;
    const { cost, ...changes } = plan;
    const next = { ...state, ...changes };
    return { ...next, ...rules.pointTotals(next, probe.points) };
  }
  const allocation = rules.planAllocation(state, path, new Map(candidate.choices));
  assert.deepEqual(after(allocation), candidate.allocation, `${name} / allocation`);
  assert.equal(rules.refundNodeCost(state, refundNode, settings), probe.refundCost, `${name} / refund quote`);
  assert.deepEqual(rules.refundDependents(state, refundNode), probe.refundDependents, `${name} / refund blockers`);
  const refund = rules.planRefund(state, refundNode, settings);
  assert.deepEqual(after(refund), probe.refund, `${name} / refund and points`);
  if (refund) assert.equal(refund.cost, probe.refundCost, `${name} / refund cost`);
  if (candidate.choices.length) {
    assert.equal(rules.planAllocation(state, path, new Map()), null, `${name} / missing attribute choice`);
  }
  for (const expected of probe.refundAll) {
    const { ascendancyOnly } = expected;
    assert.equal(rules.refundAllCost(state, ascendancyOnly, settings), expected.quote, `${name} / refund all quote`);
    const plan = rules.planRefundAll(state, ascendancyOnly, settings);
    assert.deepEqual(after(plan), expected.state, `${name} / refund all (${ascendancyOnly})`);
    if (plan) assert.equal(plan.cost, expected.cost, `${name} / refund all cost`);
  }
  for (const expected of probe.attributeChanges) {
    const plan = rules.planAttributeChange(state, target, expected.option, settings);
    assert.deepEqual(after(plan), expected.state, `${name} / attribute change ${expected.option}`);
    if (plan) assert.equal(plan.cost, expected.cost, `${name} / attribute swap cost`);
  }
}
console.log(`Passive tree C++/JS parity: ${cases.length} cases passed.`);
