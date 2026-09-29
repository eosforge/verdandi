# Verification

## Automated checks

After obtaining authorization for the current test run, execute `pnpm check`: formatting, dependency boundaries, Node regressions, strict type checks, then a Vite production build. It uses existing dependencies and installs no test framework, browser, or other tool.

| Area | Automated coverage |
| --- | --- |
| Data | Demo size, deterministic disk layout, duplicate IDs, incorrect ownership, invalid coordinates, dangling/duplicate edges |
| Camera | Approach/redirect/cancel, wheel continuity, frame-rate independence, anchor preservation, large framing, dynamic distance bounds |
| Scheduling | 30/32/60/120/144 Hz clocks, actual FPS, duplicate start, background recovery, long frames, drawing exceptions |
| Input | Click eligibility, no middle-button picking, no selection after dragging back to origin, multitouch/cancel, double-click, Escape, wheel units, listener disposal |
| Objects | 27 batches/252 instances, matrix upload, 15/6/stopped motion, layered picking, population-scaled stars, scaled picking, independent rotation/stop |
| Orbits | Shared plane/small inclination, focus ellipses, Kepler periods, analytic radial clearance, independent phases, system spacing, determinism |
| Assets | Local GLB decoding, shared-resource disposal, no external references, load errors, cancellation before/during load |
| Selection | Reject planet commands in overview/across systems, smooth focus, actual coordinates, reset without selection, reject commands after service stop |
| Architecture | Dynamic entry, numeric/shader dependency restrictions, type-only asset imports, runtime cycles |
| Cleanup | Reverse order, idempotence, continue after individual failures, immediate disposal of late resources |

Object tests replace only the 2D pixel container needed for rocky textures; GLB decoding, math, and ray picking use real Three.js. Tests also cover the selection controller, architectural rules, orbital layers, isolated editor drafts, validation, camera following, and orbital pause. This page states coverage intent; execution dates, inputs, and outcomes belong in the unified validation record. These tests do not validate GPU shaders, real drivers, default browser gestures, or Vue template runtime behavior.

## Browser acceptance checklist

After UI/runtime changes and test authorization, open the development service in an existing browser. Before release, also inspect production output with `pnpm preview`.

1. Initial view has no left feature sidebar and shows nine normal stars with planets, black-hole Orion, independent Pulsar, and top-right FPS. The six additional normal stars each have 12..24 planets. The 36 normal-star edges are retained but hidden; black holes and Pulsar have no planets or links. No console runtime errors.
2. The map fills available width without a left placeholder. Resizing preserves proportions; the lower-right editor and camera coordinates remain usable. Visual verification after sidebar removal is still pending.
3. Clicking a star approaches continuously without opening details. Left rotation and middle panning interrupt approach. In close and expanded overview, held middle drag moves camera and target together without zoom, rotation, or browser autoscroll; release ends the gesture.
4. Wheel zoom in/out remains continuous near both distance limits, with no jumps or NaN.
5. Normal-star picking diameter is 3× the surface; black-hole picking is 3× the complete optical extent including the disk, not the planet-orbit extent. Picking follows orbital motion and population scaling. Both bodies double volume (radius about +26%). Exact bodies/eligible planets take priority; overlapping enlarged regions use the nearest ray entry. Pulsar scales by 8 but uses its actual core for picking. Overview cannot select planets; star view selects only its own planets, with correct ID/type/Star/demo source in details, and pauses all planet orbits.
6. Entity-type/list selection highlights only the target row; “approach planet” points to the selected entity's actual position.
7. Empty-background double-click, Escape with canvas focus, and detail close return smoothly to overview and clear details. With no selection, first pan/rotate/zoom, then double-click to force global framing; interrupting and repeating reset still works.
8. Hiding the page stops rendering; resumption does not catch up animation. Hot reload/unmount leaves no duplicate canvas, listeners, or RAF.
9. In an authorized development environment, simulate unavailable WebGL or context loss: show an error, and retry creates exactly one new canvas.
10. Snapshot replacement disposes the old scene and opens the new one in overview. Empty snapshots do not crash; invalid snapshots show errors.
11. Planet systems form thin disks, not spherical shells, in overview and star view. Side views retain slight thickness; system planes differ.
12. All 252 planets have distinct semimajor axes with stars at foci. System extents remain separated, outer periods longer, and layered picking consistent with 15/6/stopped motion.
13. The expanded map fits at first load and overview return. Portrait/landscape changes cause no wheel distance jump. Links/focus use new centers; black-hole background lensing has no clipping-depth anomaly.
14. Pulsar-core selection focuses smoothly without planet details. Glow fades continuously without a hard shell. Bipolar beams sweep with the tilted magnetic axis; filaments flow outward. Three magnetic layers have moving bright knots and fade at distance, without visible cylindrical proxies or rectangular backgrounds.
15. Pulsar rotates in about 2.4 seconds by default. `setStarRotationSpeed("star-pulsar", 0)` stops rotation; positive/negative speeds resume it. Other planet selection does not stop its spin. Inspect beams head-on, side-on, from inside, and overlapping other bodies.
16. Pulsar is the shared orbital center of all stars, including black holes. Initial/overview views target it, frame every system, and rotate around it. Filaments still flow when spin stops, without reset jumps across multiple four-second periods.
17. A small star population shares a compact inner layer with varied radius/phase/inclination, not a regular nonagon. Outer layers start above 12. Link endpoints follow normal stars. Black holes keep orbiting and participate in equal-weight center layout.
18. Focusing an orbiting star approaches smoothly, then holds it at screen center. Rotation and off-center pointer zoom preserve centering. Star view disallows panning; overview/planet details restore it. Star switches, planet pause, and overview return remain continuous; page resumption does not catch up displacement.
19. Mouse and keyboard open the lower-right triangle editor. Each star can switch normal/black-hole state without changing ID/name. Black-hole state disables satellite count, removes links/satellites on confirmation, and keeps orbiting; returning to normal restores connection participation. At most one pulsar.
20. Cancel, Escape, close, and backdrop clicks preserve the original map. Reopening resets to current settings. Empty names, invalid counts, and deleting all bodies produce errors while retaining the draft.
21. Generate pulsar-only, black-hole-only, single-star, no-pulsar, and multiple-black-hole scenes. Repeated rebuilds/rapid confirmations leave one canvas/RAF; stale loads cannot replace a newer map. Rebuild returns to overview.
22. Narrow windows retain usable name/count inputs, scrollable content, and an accessible confirm button. Modal focus never enters the canvas behind it.
23. World camera X/Y/Z at the triangle's left show two decimals and update during rotation, panning, zoom, and black-hole following. Text is copyable without disturbing canvas gestures; rebuild clears old readings.
24. First load, rebuild, and completed overview reset all use `(-178, 176, 1083)` with Pulsar centered. First frame/resizing must not clamp to an old distance limit. Reset after manual movement/selection smoothly returns to the same position. Later motion may leave the frame or pass behind the camera. Visual verification of fixed global coordinates is pending.
25. Star orbits, including black holes, use 15× in both overview and star view without phase reset or selection-dependent speed. Satellites use 15/6; planet details pause orbital motion. Check system clearance across complete cycles under different inclinations and the 12/13 layer boundary. Browser and automated acceptance are separate.
26. Dim blue-gray closed lines follow each star's tilted orbit; no inter-star links or planet orbits are shown. Guides bypass gravitational lensing but retain depth occlusion, remain visible without planet selection/black holes, align after rotation/focus/rebuild, and do not intercept clicks. No pulsar means no orbital guides. Visual and helper-layer regression acceptance are separate.
27. Toggling cosmic background preserves planet materials, lighting, and exposure. Soft dim warm/cool variation, faint grain, and sparse stars have no per-frame random flicker. Rotation preserves sky direction; approach creates no nearby cloud/star parallax. Foreground bodies occlude distant stars, black holes capture the background, and orbital guides remain separate. Check rebuild cleanup and FPS changes. The new background's GPU appearance/performance are unverified.

Items 8–10 concern browser lifecycle. Unexecuted items must be recorded explicitly, never replaced by pure-function results.

## Fixed black-hole views

The development page `/tests/visual/black-hole.html` uses production GLB, materials, and instance factories with DPR fixed at 1. Buttons select 0, -2, +2, 12, 45, and 90 degrees and distances 18, 48, 96, and 192. Flow is paused by default and can be played separately to compare disk-rim joins, broad secondary images, and scale filtering. During playback, inspect inward spirals and knot dissipation for at least two flow cycles without reset jumps. This is not a production build entry and installs no tools. It uses raw local model units; the full map scales the entire black hole by `0.75 × ∛2`.

`/tests/visual/galaxy-controls.html` exposes production controller buttons/results to check overview, cross-system rejection, and valid planet selection. Atlas can rotate at an explicit 0.3 rad/s or stop; that is an acceptance input, not a production default. The page neither accesses private scene state nor bypasses selection permissions. It disposes its scene on close and is not a production build entry.

## Latest execution record

Actual outcomes live in [validation](../../docs/validation.md#full-sdk-regression-results). This page maintains methods and acceptance boundaries only. Earlier individual adjustments and screenshots are available through Git history.
