// 验证场景选择契约, 使用真实相机运动且不创建 DOM 或 WebGL 上下文.
import assert from "node:assert/strict";
import test from "node:test";
import { PerspectiveCamera, Vector3 } from "three";
import { CameraMotion } from "../src/features/galaxy/runtime/cameraMotion.ts";
import { sceneConfig } from "../src/features/galaxy/runtime/config.ts";
import { createSelectionController } from "../src/features/galaxy/runtime/scene/createSelectionController.ts";

function fixture() {
  const makeStar = (id, x) => ({
    id,
    name: id,
    color: "#ffffff",
    status: "available",
    position: [x, 0, 0],
    planets: [{ id: `${id}/planet`, starId: id, name: "Planet", kind: "Registry", position: [8, 2, 0] }],
  });
  const stars = [makeStar("a", -20), makeStar("b", 20)];
  const camera = new PerspectiveCamera(sceneConfig.camera.fieldOfView, 1, 0.1, 1600);
  camera.position.fromArray(sceneConfig.camera.overviewPosition);
  const target = new Vector3(...sceneConfig.camera.overviewTarget);
  const motion = new CameraMotion(camera.position, target);
  const events = [];
  let active = true;
  let momentumClears = 0;
  let speedWrites = 0;
  const currentPlanetPosition = new Vector3(-10, 6, 9);
  const systemsById = new Map(stars.map((star) => [star.id, { star, orbitExtent: 70 }]));
  const controller = createSelectionController({
    camera,
    controls: { target, clearMomentum: () => momentumClears++ },
    motion,
    isActive: () => active,
    onSelect: (value) => events.push(["selection", value]),
    objects: {
      systemsById,
      findPlanet(starId, planetId) {
        const star = systemsById.get(starId)?.star;
        const planet = star?.planets.find((planet) => planet.id === planetId);
        return planet ? { star, planet } : undefined;
      },
      worldPosition: () => currentPlanetPosition.clone(),
      showSelection: (value) => events.push(["decoration", value]),
      setStarRotationSpeed: () => {
        speedWrites++;
        return true;
      },
    },
  });
  return {
    controller,
    camera,
    target,
    motion,
    stars,
    events,
    currentPlanetPosition,
    stop: () => {
      active = false;
    },
    get momentumClears() {
      return momentumClears;
    },
    get speedWrites() {
      return speedWrites;
    },
  };
}

test("overview and cross-system commands cannot select planets; selecting a star starts a smooth approach", () => {
  const view = fixture();
  const { controller, camera, motion, target } = view;
  const original = camera.position.clone();
  assert.equal(controller.selectPlanet("a", "a/planet"), false);
  assert.equal(controller.focusPlanet("a", "a/planet"), false);
  assert.equal(controller.selectStar("missing"), false);
  assert.equal(view.events.length, 0);
  assert.equal(controller.selectStar("a"), true);
  assert.equal(controller.selection.planet, null);
  assert.deepEqual(camera.position, original, "commands must not jump the camera");
  assert.equal(view.momentumClears, 1);
  assert.deepEqual(
    view.events.map(([kind]) => kind),
    ["decoration", "selection"],
  );
  assert.equal(controller.selectPlanet("b", "b/planet"), false);
  assert.equal(controller.focusPlanet("b", "b/planet"), false);
  assert.equal(controller.selectPlanet("a", "missing"), false);
  motion.update(sceneConfig.camera.transitionSeconds);
  assert.deepEqual(target.toArray(), view.stars[0].position);
  assert.ok(camera.position.distanceTo(target) > sceneConfig.camera.starDistance, "focus includes the planned orbit extent");
  assert.equal(controller.selectPlanet("a", "a/planet"), true);
  assert.equal(controller.selection.planet.id, "a/planet");
});

test("planet focus uses the current orbit position; overview clears selection and is idempotent", () => {
  const view = fixture();
  view.controller.selectStar("a");
  assert.equal(view.controller.focusPlanet("a", "a/planet"), true);
  view.motion.update(sceneConfig.camera.transitionSeconds);
  assert.deepEqual(view.target, view.currentPlanetPosition);
  assert.notDeepEqual(view.target.toArray(), view.stars[0].planets[0].position);
  view.controller.overview();
  assert.equal(view.controller.selection, null);
  const count = view.events.length;
  const clears = view.momentumClears;
  view.controller.overview();
  assert.equal(view.events.length, count);
  assert.equal(view.momentumClears, clears);
  view.motion.update(sceneConfig.camera.transitionSeconds);
  assert.deepEqual(view.camera.position.toArray(), sceneConfig.camera.overviewPosition);
  assert.deepEqual(view.target.toArray(), sceneConfig.camera.overviewTarget);
});

test("failed or disposed scenes reject every control command without further effects", () => {
  const view = fixture();
  view.controller.selectStar("a");
  view.controller.selectPlanet("a", "a/planet");
  assert.equal(view.controller.setStarRotationSpeed("a", 0.4), true);
  const selection = view.controller.selection;
  const count = view.events.length;
  const clears = view.momentumClears;
  view.stop();
  assert.equal(view.controller.selectStar("b"), false);
  assert.equal(view.controller.selectPlanet("a", "a/planet"), false);
  assert.equal(view.controller.focusPlanet("a", "a/planet"), false);
  assert.equal(view.controller.setStarRotationSpeed("a", 0.8), false);
  view.controller.overview();
  assert.equal(view.controller.selection, selection);
  assert.equal(view.events.length, count);
  assert.equal(view.momentumClears, clears);
  assert.equal(view.speedWrites, 1);
});
