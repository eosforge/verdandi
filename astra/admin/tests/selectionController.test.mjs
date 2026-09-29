// 验证场景选择契约, 使用真实相机运动且不创建 DOM 或 WebGL 上下文.
import assert from "node:assert/strict";
import test from "node:test";
import { PerspectiveCamera, Vector3 } from "three";
import { CameraMotion } from "../src/features/galaxy/runtime/cameraMotion.ts";
import { sceneConfig } from "../src/features/galaxy/runtime/config.ts";
import { createSelectionController } from "../src/features/galaxy/runtime/scene/createSelectionController.ts";
import { createSceneFraming } from "../src/features/galaxy/runtime/scene/sceneFraming.ts";

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
  camera.position.fromArray(sceneConfig.camera.initialPosition);
  const target = new Vector3(...sceneConfig.camera.overviewTarget);
  const motion = new CameraMotion(camera.position, target);
  const events = [];
  let active = true;
  let momentumClears = 0;
  let speedWrites = 0;
  const currentPlanetPosition = new Vector3(-190, 6, 9);
  const systemsById = new Map(stars.map((star) => [star.id, { star, center: [star.position[0] * 10, 0, 0], orbitExtent: 70 }]));
  const controller = createSelectionController({
    camera,
    controls: { target, clearMomentum: () => momentumClears++ },
    motion,
    framing: createSceneFraming(),
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
    systemsById,
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
  assert.deepEqual(target.toArray(), view.systemsById.get("a").center);
  assert.notDeepEqual(target.toArray(), view.stars[0].position, "focus must use the expanded display center");
  assert.ok(camera.position.distanceTo(target) > sceneConfig.camera.starDistance, "focus includes the planned orbit extent");
  assert.equal(controller.selectPlanet("a", "a/planet"), true);
  assert.equal(controller.selection.planet.id, "a/planet");
});

test("planet focus uses the current orbit position; overview resets the camera without duplicate selection events", () => {
  const view = fixture();
  view.controller.selectStar("a");
  assert.equal(view.controller.focusPlanet("a", "a/planet"), true);
  view.motion.update(sceneConfig.camera.transitionSeconds);
  assert.deepEqual(view.target, view.currentPlanetPosition);
  assert.notDeepEqual(view.target.toArray(), view.stars[0].planets[0].position);
  const expectedApproach = view.currentPlanetPosition
    .clone()
    .sub(new Vector3(...view.systemsById.get("a").center))
    .normalize()
    .multiplyScalar(sceneConfig.camera.planetDistance)
    .add(new Vector3(0, 5, 0));
  assert.ok(view.camera.position.clone().sub(view.target).distanceTo(expectedApproach) < 1e-10);
  view.controller.overview();
  assert.equal(view.controller.selection, null);
  const count = view.events.length;
  const clears = view.momentumClears;
  view.controller.overview();
  assert.equal(view.events.length, count);
  assert.equal(view.momentumClears, clears + 1);
  view.motion.update(sceneConfig.camera.transitionSeconds);
  assert.deepEqual(view.camera.position.toArray(), sceneConfig.camera.initialPosition);
  assert.deepEqual(view.target.toArray(), sceneConfig.camera.overviewTarget);
});

test("overview restores a manually moved camera without a selection and restarts an interrupted return", () => {
  const view = fixture();
  view.camera.position.set(50, 25, 70);
  view.target.set(30, 5, -40);
  const moved = view.camera.position.clone();
  assert.equal(view.controller.selection, null);
  view.motion.zoomBy(-120, view.target);
  view.controller.overview();
  assert.deepEqual(view.camera.position, moved, "return remains smooth rather than teleporting");
  view.motion.update(sceneConfig.camera.transitionSeconds / 2);
  assert.notDeepEqual(view.camera.position, moved);
  view.motion.cancel();
  view.controller.overview();
  view.motion.update(sceneConfig.camera.transitionSeconds);
  assert.deepEqual(view.camera.position.toArray(), sceneConfig.camera.initialPosition);
  assert.deepEqual(view.target.toArray(), sceneConfig.camera.overviewTarget);
  assert.equal(view.momentumClears, 2);
  assert.equal(view.events.length, 0, "camera reset alone does not republish unchanged selection");
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
