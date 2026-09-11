import assert from "node:assert/strict";
import test from "node:test";
import { PerspectiveCamera, Vector3 } from "three";
import { bindCanvasInput } from "../src/features/galaxy/runtime/canvasInput.ts";
import { CameraMotion } from "../src/features/galaxy/runtime/cameraMotion.ts";
import { ResourceScope } from "../src/features/galaxy/runtime/resourceScope.ts";

function harness(onCancel = () => {}) {
  const canvas = new EventTarget();
  canvas.getBoundingClientRect = () => ({ left: 0, top: 0, width: 800, height: 600 });
  canvas.focus = () => {};
  canvas.clientHeight = 600;
  const counts = { picks: 0, overviews: 0, cancels: 0, wheelPixels: [] };
  const camera = new PerspectiveCamera(45, 800 / 600, 0.1, 1600);
  camera.position.set(0, 0, 100);
  const scope = new ResourceScope();
  const input = bindCanvasInput(
    {
      canvas,
      camera,
      target: new Vector3(),
      motion: {
        cancel() {
          counts.cancels++;
          onCancel();
        },
        zoomBy(pixels) {
          counts.wheelPixels.push(pixels);
        },
      },
      pick() {
        counts.picks++;
      },
      overview() {
        counts.overviews++;
      },
    },
    scope,
  );
  function send(type, values = {}) {
    const event = new Event(type, { cancelable: true });
    Object.assign(event, { pointerId: 1, isPrimary: true, clientX: 400, clientY: 300, button: 0, detail: 1, ...values });
    canvas.dispatchEvent(event);
    return event;
  }
  return { counts, input, scope, send };
}

test("click selects once, middle drag never selects, and a drag returning to its start stays a drag", () => {
  const { counts, scope, send } = harness();
  try {
    send("pointerdown");
    send("pointerup");
    send("click");
    send("click");
    assert.equal(counts.picks, 1);
    send("pointerdown", { button: 1 });
    send("pointermove", { clientX: 430 });
    send("pointerup");
    send("click");
    assert.equal(counts.picks, 1);
    send("pointerdown");
    send("pointermove", { clientX: 430 });
    send("pointermove");
    send("pointerup");
    send("click");
    assert.equal(counts.picks, 1);
    assert.equal(counts.cancels, 3);
    send("pointerdown");
    send("pointerup", { clientX: 430 });
    send("click");
    assert.equal(counts.picks, 1);
  } finally {
    scope.dispose();
  }
});

test("pressing during focus cancels the tween before the first sub-threshold movement", () => {
  const position = new Vector3(0, 0, 100);
  const target = new Vector3();
  const motion = new CameraMotion(position, target);
  const { counts, scope, send } = harness(() => motion.cancel());
  try {
    motion.moveTo(new Vector3(50, 40, 90), new Vector3(50, 0, 0));
    motion.update(0.4);
    const atPress = position.clone();
    send("pointerdown");
    send("pointermove", { clientX: 401 });
    motion.update(0.1);
    assert.deepEqual(position, atPress, "focus must not pull against the first pixel of dragging");
    assert.equal(counts.cancels, 1);
    send("pointerup", { clientX: 401 });
    send("click");
    assert.equal(counts.picks, 1, "small click tolerance is separate from taking camera control");
  } finally {
    scope.dispose();
  }
});

test("double click returns to overview; cancelled and secondary pointers cannot select", () => {
  const { counts, input, scope, send } = harness();
  try {
    send("pointerdown");
    send("pointerup");
    send("click", { detail: 2 });
    send("dblclick");
    assert.equal(counts.picks, 0);
    assert.equal(counts.overviews, 1);
    send("pointerdown");
    input.cancelPointer();
    send("pointerup");
    send("click");
    send("pointerdown", { isPrimary: false });
    send("pointerup");
    send("click");
    assert.equal(counts.picks, 0);
    assert.equal(send("keydown", { key: "Escape" }).defaultPrevented, true);
    assert.equal(counts.overviews, 2);
  } finally {
    scope.dispose();
  }
});

test("wheel units are normalized and disposing removes all input listeners", () => {
  const { counts, scope, send } = harness();
  for (const deltaMode of [0, 1, 2]) assert.equal(send("wheel", { deltaY: 2, deltaMode }).defaultPrevented, true);
  assert.deepEqual(counts.wheelPixels, [2, 32, 1200]);
  scope.dispose();
  send("pointerdown");
  send("pointerup");
  send("click");
  send("dblclick");
  send("keydown", { key: "Escape" });
  assert.equal(send("wheel", { deltaY: 2, deltaMode: 0 }).defaultPrevented, false);
  assert.equal(counts.picks, 0);
  assert.equal(counts.overviews, 0);
  assert.deepEqual(counts.wheelPixels, [2, 32, 1200]);
});
