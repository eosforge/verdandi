import assert from "node:assert/strict";
import test from "node:test";
import { DataUtils } from "three";
import { blackHoleOptics } from "../src/features/galaxy/runtime/blackHole/config.ts";
import { columnAtImpact, impactAtColumn, traceLightOrbit } from "../src/features/galaxy/runtime/blackHole/optics.ts";
import { createBendingTexture, createImageBoundsTexture, createObserverTexture } from "../src/features/galaxy/runtime/blackHole/lookupTextures.ts";

test("zero gravity reproduces a straight ray and exits after half an orbit", () => {
  const impact = 7;
  const orbit = traceLightOrbit(impact, 0);
  const step = blackHoleOptics.maxAngle / (orbit.inverseRadii.length - 1);
  assert.equal(orbit.captured, false);
  assert.ok(Math.abs(orbit.escapeAngle - Math.PI) <= step);
  for (let index = 0; index < orbit.inverseRadii.length; index++) {
    const expected = index * step <= Math.PI ? Math.sin(index * step) / impact : 0;
    assert.ok(Math.abs(orbit.inverseRadii[index] - expected) < 1e-7);
  }
});

test("the critical impact separates capture from escape and gravity increases deflection", () => {
  const radius = blackHoleOptics.schwarzschildRadius;
  const captured = traceLightOrbit(4, radius);
  const near = traceLightOrbit(5, radius);
  const far = traceLightOrbit(10, radius);
  assert.equal(captured.captured, true);
  assert.equal(captured.escapeAngle, null);
  assert.ok(Math.abs(captured.inverseRadii.at(-1) - 1 / radius) < 1e-7, "captured rays never reappear at infinity after termination");
  assert.equal(near.captured, false);
  assert.equal(far.captured, false);
  assert.ok(near.escapeAngle > far.escapeAngle);
  assert.ok(far.escapeAngle > Math.PI);
  for (const orbit of [captured, near, far]) assert.ok(orbit.inverseRadii.every((value) => Number.isFinite(value) && value >= 0));
});

test("the orbit grid covers captured and escaped rays continuously around the critical impact", () => {
  const { width, minImpact, shadowRadius, maxImpact } = blackHoleOptics;
  assert.ok(Math.abs(impactAtColumn(0) - minImpact) < 1e-12);
  assert.equal(impactAtColumn((width - 1) / 2), shadowRadius);
  assert.equal(impactAtColumn(width - 1), maxImpact);
  for (let column = 1; column < width; column++) {
    assert.ok(impactAtColumn(column) > impactAtColumn(column - 1));
    assert.ok(Math.abs(columnAtImpact(impactAtColumn(column)) - column) < 1e-8);
  }
});

test("finite observer phases locate the camera on inbound light orbits at near and far distances", () => {
  const texture = createObserverTexture();
  try {
    const { width, height, maxAngle, schwarzschildRadius, observerHeight, observerMinRadius } = blackHoleOptics;
    const data = texture.image.data;
    for (const impact of [2, 4, 7]) {
      const column = Math.round(columnAtImpact(impact));
      const orbit = traceLightOrbit(impactAtColumn(column), schwarzschildRadius);
      for (const radius of [8, 48, 240]) {
        const row = Math.round((observerMinRadius / radius) * (observerHeight - 1));
        const phase = DataUtils.fromHalfFloat(data[(row * width + column) * 2]);
        const sample = (phase / maxAngle) * (height - 1);
        const lower = Math.floor(sample);
        const inverse = orbit.inverseRadii[lower] * (1 - sample + lower) + orbit.inverseRadii[lower + 1] * (sample - lower);
        assert.ok(Math.abs(inverse - row / (observerHeight - 1) / observerMinRadius) < 2e-4);
      }
    }
  } finally {
    texture.dispose();
  }
});

test("higher-order image bounds remain finite and narrow toward the critical shadow", () => {
  const bending = createBendingTexture();
  const bounds = createImageBoundsTexture(bending);
  try {
    const { height, maxAngle, maxImpact, shadowRadius } = blackHoleOptics;
    const data = bounds.image.data;
    const widths = [3.5, 6.5, 8.5].map((phase) => {
      const row = Math.round((phase / maxAngle) * (height - 1));
      const inner = data[row * 4];
      const outer = data[row * 4 + 1];
      assert.ok(Number.isFinite(inner) && Number.isFinite(outer));
      assert.ok(inner >= shadowRadius && outer <= maxImpact && outer > inner);
      const radius = data[row * 4 + 2];
      const correction = data[row * 4 + 3];
      assert.ok(radius >= blackHoleOptics.discInner && radius <= blackHoleOptics.discOuter);
      assert.ok(Number.isFinite(correction) && correction >= 0);
      const flux = ((blackHoleOptics.discInner * 1.36) / radius) ** 3 * (1 - Math.sqrt(blackHoleOptics.discInner / radius)) * 7;
      assert.ok(flux * correction > 0 && flux * correction <= 1, "integrated unresolved emission cannot exceed the disc's peak emission");
      return outer - inner;
    });
    assert.ok(widths[0] > widths[1] && widths[1] > widths[2]);
  } finally {
    bending.dispose();
    bounds.dispose();
  }
});
