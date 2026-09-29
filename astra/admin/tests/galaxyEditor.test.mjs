import assert from "node:assert/strict";
import test from "node:test";
import { demoGalaxy } from "../src/features/galaxy/data/demo.ts";
import { createDraftStar, draftFromGalaxy, galaxyFromDraft, editorLimits } from "../src/features/galaxy/model/galaxyEditor.ts";
import { validateGalaxyData } from "../src/features/galaxy/model/validate.ts";

const entry = (id, satellites = 0, status = "available") => ({ id, name: id, satellites, status });
const pulsar = (enabled) => ({ id: "central", enabled, name: "P" });

test("draft edits are isolated and black-hole state belongs to the same star list", () => {
  const before = structuredClone(demoGalaxy);
  const draft = draftFromGalaxy(demoGalaxy);
  assert.equal(draft.stars.find((star) => star.id === "star-orion").status, "black-hole");
  draft.stars = [entry("A", 0), entry("B", 13), entry("C", 24), entry("H1", 0, "black-hole"), entry("H2", 0, "black-hole")];
  draft.stars[0].name = " A ";
  draft.pulsar = { ...pulsar(true), name: " Center " };
  const result = galaxyFromDraft(draft);
  assert.deepEqual(demoGalaxy, before);
  assert.deepEqual(
    result.stars.slice(0, 3).map((star) => star.planets.length),
    [0, 13, 24],
  );
  assert.equal(result.stars[0].name, "A");
  assert.equal(result.stars.find((star) => star.id === result.centerStarId).name, "Center");
  assert.equal(result.links.length, 3);
  const ordinary = new Set(["A", "B", "C"]);
  assert.ok(result.links.every((link) => ordinary.has(link.source) && ordinary.has(link.target)));
  assert.ok(result.stars.slice(3).every((star) => !star.planets.length));
  assert.doesNotThrow(() => validateGalaxyData(result));
});

test("a status change preserves identity and removes incident links; restoring normal state reconnects it", () => {
  const draft = { stars: [entry("A", 7), entry("B"), entry("C")], pulsar: pulsar(false) };
  assert.equal(galaxyFromDraft(draft).links.length, 3);
  draft.stars[0].status = "black-hole";
  const changed = galaxyFromDraft(draft);
  assert.equal(changed.stars[0].id, "A");
  assert.equal(changed.stars[0].name, "A");
  assert.equal(changed.stars[0].planets.length, 0);
  assert.deepEqual(changed.links, [{ source: "B", target: "C" }]);
  assert.equal(draft.stars[0].satellites, 7, "switching the draft back before confirmation preserves the configured count");
  draft.stars[0].status = "available";
  assert.equal(galaxyFromDraft(draft).stars[0].planets.length, 7);
  assert.equal(galaxyFromDraft(draft).links.length, 3);
  draft.stars.splice(1, 1);
  assert.deepEqual(
    galaxyFromDraft(draft).stars.map((star) => star.id),
    ["A", "C"],
  );
  const added = createDraftStar(draft);
  draft.stars.push(added);
  assert.notEqual(createDraftStar(draft).id, added.id);
  assert.equal(draftFromGalaxy(changed).stars[0].status, "black-hole");
});

test("pulsar-only, lone star and black-hole-only snapshots are supported", () => {
  for (const draft of [
    { stars: [], pulsar: pulsar(true) },
    { stars: [entry("S", 1)], pulsar: pulsar(false) },
    { stars: [entry("H1", 0, "black-hole"), entry("H2", 0, "black-hole")], pulsar: pulsar(false) },
  ]) {
    const result = galaxyFromDraft(draft);
    assert.equal(result.centerStarId !== undefined, draft.pulsar.enabled);
    assert.equal(result.links.length, 0);
    assert.doesNotThrow(() => validateGalaxyData(result));
  }
  const invalid = galaxyFromDraft({ stars: [], pulsar: pulsar(true) });
  invalid.stars.push({ ...invalid.stars[0], id: "second-pulsar" });
  assert.throws(() => validateGalaxyData(invalid), /one pulsar/);
});

test("invalid drafts fail before a replacement snapshot is published", () => {
  const draft = { stars: [entry("A", 1)], pulsar: pulsar(false) };
  for (const satellites of [null, -1, 1.5, NaN, Infinity, editorLimits.satellites + 1])
    assert.throws(() => galaxyFromDraft({ ...draft, stars: [entry("A", satellites)] }), /卫星数量/);
  for (const name of ["   ", "a".repeat(65)]) assert.throws(() => galaxyFromDraft({ ...draft, stars: [{ ...entry("A"), name }] }), /名称/);
  assert.throws(() => galaxyFromDraft({ ...draft, stars: [entry("A", 0, "unknown")] }), /状态/);
  assert.throws(() => galaxyFromDraft({ ...draft, stars: [entry("A"), entry("A")] }), /Duplicate/);
  assert.throws(() => galaxyFromDraft({ ...draft, stars: [] }), /至少/);
  assert.throws(() => galaxyFromDraft({ ...draft, stars: Array.from({ length: editorLimits.stars + 1 }, (_, i) => entry(`S${i}`)) }), /上限/);
});
