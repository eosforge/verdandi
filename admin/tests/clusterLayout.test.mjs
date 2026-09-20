import assert from "node:assert/strict";
import test from "node:test";
import { planClusterLayout } from "../src/features/galaxy/model/clusterLayout.ts";
import { writeStellarPosition, stellarLayerCapacity } from "../src/features/galaxy/model/stellarOrbits.ts";
import { galaxyFromDraft } from "../src/features/galaxy/model/galaxyEditor.ts";
import { systemSpacing } from "../src/features/galaxy/model/systemLayout.ts";

const distance = (a, b) => Math.hypot(...a.map((value, axis) => value - b[axis]));

test("inclined elliptical stellar systems retain clearance and a near-central centroid through independent periods", () => {
  for (const count of [1, 2, 3, 4, 5, 7, 9, 10, 11, 32]) {
    const data = galaxyFromDraft({
      stars: [
        ...Array.from({ length: count }, (_, i) => ({ id: `S${i}`, name: `S${i}`, status: "available", satellites: i })),
        { id: "H1", name: "H1", status: "black-hole", satellites: 0 },
        { id: "H2", name: "H2", status: "black-hole", satellites: 0 },
      ],
      pulsar: { id: "P", enabled: true, name: "P" },
    });
    const envelopes = data.stars.map((star, index) => ({ id: star.id, position: star.position, extent: 5 + ((index * 7) % 23) }));
    const original = structuredClone(data);
    const plan = planClusterLayout(data, envelopes);
    const orbit = plan.orbit;
    const extents = new Map(envelopes.map((item) => [item.id, item.extent]));
    assert.equal(orbit.members.length, count + 2, "black-hole stars participate in the same orbit plan");
    if (count === 9) {
      assert.ok(new Set(orbit.members.map((member) => member.path.semiMajorAxis)).size > 1, "nine stars must not form one regular polygon");
      assert.equal(
        new Set(orbit.members.map((member) => member.path.meanMotion)).size,
        1,
        "small populations share the inner layer instead of pushing out isolated stars",
      );
      assert.ok(new Set(orbit.members.map((member) => member.path.periapsis.join())).size > 1);
    }
    if (orbit.members.length > stellarLayerCapacity) assert.ok(new Set(orbit.members.map((member) => member.path.meanMotion)).size > 1);
    const maxPeriod = Math.max(...orbit.members.map((member) => (Math.PI * 2) / member.path.meanMotion));
    for (let step = 0; step <= 240; step++) {
      const positions = new Map(plan.positions);
      orbit.members.forEach((member, index) => {
        const position = [0, 0, 0];
        writeStellarPosition(orbit, index, (maxPeriod * 3.1 * step) / 240, position);
        positions.set(member.id, position);
        assert.ok(distance(position, orbit.center) + extents.get(member.id) <= orbit.extent + 1e-8);
      });
      const entries = [...positions];
      entries.forEach(([id, position], index) => {
        for (const [otherId, otherPosition] of entries.slice(index + 1))
          assert.ok(
            distance(position, otherPosition) >= (extents.get(id) + extents.get(otherId)) * systemSpacing.extentRatio + systemSpacing.gap - 1e-8,
            `${id} and ${otherId} overlap`,
          );
      });
      if (count > 1) {
        const mean = [0, 0, 0];
        for (const member of orbit.members) positions.get(member.id).forEach((value, axis) => (mean[axis] += value / orbit.members.length));
        assert.ok(distance(mean, orbit.center) < orbit.extent * 0.18, "equal-weight centroid stays near the pulsar despite different periods");
      }
    }
    assert.deepEqual(data, original);
    const reordered = planClusterLayout({ ...data, stars: [...data.stars].reverse() }, [...envelopes].reverse());
    assert.deepEqual(reordered, plan, "input iteration order does not change layout");
  }
});

test("no pulsar leaves stars static; empty ordinary populations are valid", () => {
  for (const [count, pulse] of [
    [3, false],
    [0, true],
    [0, false],
  ]) {
    const data = galaxyFromDraft({
      stars: [
        ...Array.from({ length: count }, (_, i) => ({ id: `S${i}`, name: `S${i}`, status: "available", satellites: 0 })),
        { id: "H", name: "H", status: "black-hole", satellites: 0 },
      ],
      pulsar: { id: "P", enabled: pulse, name: "P" },
    });
    const plan = planClusterLayout(
      data,
      data.stars.map((star) => ({ id: star.id, position: star.position, extent: 10 })),
    );
    assert.equal(plan.orbit !== undefined, pulse);
    assert.equal(plan.positions.size, data.stars.length);
    assert.ok([...plan.positions.values()].flat().every(Number.isFinite));
  }
});

test("status alone never excludes a star from orbit and barycenter planning", () => {
  const data = galaxyFromDraft({
    stars: Array.from({ length: 5 }, (_, i) => ({ id: `S${i}`, name: `S${i}`, status: "available", satellites: 0 })),
    pulsar: { id: "P", enabled: true, name: "P" },
  });
  const envelopes = data.stars.map((star) => ({ id: star.id, position: star.position, extent: 10 }));
  const normal = planClusterLayout(data, envelopes);
  data.stars[0].status = "black-hole";
  const changed = planClusterLayout(data, envelopes);
  assert.deepEqual(changed, normal, "same size and identity retain identical orbital participation across state changes");
  const member = changed.orbit.members.findIndex((star) => star.id === "S0");
  const position = [0, 0, 0];
  writeStellarPosition(changed.orbit, member, 10, position);
  assert.ok(distance(position, changed.positions.get("S0")) > 0.1, "the black-hole state actually revolves");
});
