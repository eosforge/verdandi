# Admin architecture

> Target roles follow the [map architecture](../../docs/architecture.md): Star is a star, Planet is a fully authorized cache relay, and application satellites attach directly to a Star or to a Planet. The following describes the implemented older display model, not completion of that migration.

## Live management and demo boundaries

App explicitly selects the demo map or `app/management/Management.vue`. Live management validates HTTP/NDJSON through `api.ts`, uses Cookies and cancellable bounded requests, and keeps network DTOs and real identities out of Three.js frame logic. It currently uses tables, not a connection graph inferred from trusted membership; complete Orrery visualization is deferred. Mode switches/unmount cancel this page's networking and timers, without signing out other pages or stopping services.

## Dependency direction

```text
main → app/App + AppShell + styles
          ↓ choose source and inject GalaxyData
       galaxy/index → ui/GalaxyView → ui/PlanetDetails
                            ↓
                  composables/useGalaxyScene
                            ↓ dynamic import
                  runtime/createGalaxyScene
                   ├─ scene/createSceneView → canvas, camera, controls, sizing
                   ├─ scene/createSelectionController → selection, permission, focus
                   ├─ createGalaxyObjects → objects/ models, motion, index, decoration
                   ├─ lensingRenderer → color/depth capture, refraction, composition
                   ├─ celestialAssets → assets/*.glb + blackHole/prepareModel
                   ├─ CameraMotion
                   ├─ FrameOrbitControls
                   ├─ canvasInput
                   ├─ FrameLoop
                   └─ ResourceScope

model/types + presentation + layout + orbitalPlane + orbit + orbitPlanner + systemLayout + validate ← data/demo, ui, composables, runtime
```

`GalaxyData` is a presentation snapshot, not Astrolabe's wire protocol. Stars, planets, and links have stable IDs. Planet coordinates are initial layout hints relative to a Star; collision planning may change actual starts/orbits without mutating input. Star coordinates are also hints; runtime expands spacing for complete system extents and stores display centers separately in `StarSystem.center`. Links are explicit; rendering never invents a complete graph. Initialization validates uniqueness, ownership, finite coordinates, and edge endpoints. This checks typed internal snapshots; network input separately needs structure/size/authentication checks. Presentation validation does not implement management permission tiers.

## Responsibilities and ownership

| Module | Owns | Does not own |
| --- | --- | --- |
| App | Theme, layout, data-source choice | Internal scene state |
| GalaxyView / PlanetDetails / PlanetList | Container, FPS, errors, details, list categories, ID selection | Frame computation, GPU resources |
| useGalaxyScene | Async loading, Vue projections, snapshot replacement, retry | Picking, camera matrices |
| createGalaxyScene | Async initialization, input/frame wiring, failure/disposal | Selection rules, GPU algorithms |
| scene/createSceneView | Canvas, camera, controls, sizing | Selection, business data, RAF |
| scene/createSelectionController | Sole selection state, ID permission, smooth focus commands | DOM, rendering, resource creation |
| celestialAssets | Same-origin loading, structure validation, GPU registration | Business selection, animation clocks |
| createGalaxyObjects | Object composition, selection decoration | DOM events, RAF |
| objects/createStarSystems + createPlanetShells | Global orbit planning, models, instance batches | Picking, selection, frame loop |
| objects/createObjectMotion | Spin, black-hole animation, independent orbital periods | New resources, rendering |
| objects/createObjectIndex | Read-only ID index, layered picking, live world coordinates | Modeling, separate selection state |
| objects/createSceneDecorations | Shared lights, available-node links, selection rings | Business state, animation |
| blackHole/ | Optical settings/math/tables/materials, GLB preparation, instance animation | Scene composition, data source, Vue |
| CameraMotion | Continuous approach, pointer-anchored zoom | RAF, renderer |
| canvasInput | Click/drag distinction, wheel normalization, double-click, Escape | Detail UI, business data |
| FrameLoop | Sole RAF, 60 FPS target, measured FPS, pause/stop | Vue, DOM visibility decisions |
| ResourceScope | Reverse, idempotent, best-effort complete cleanup | Business-state recovery |

`createSelectionController` alone owns selection; composition exposes read-only getters to motion and picking. It borrows object/camera capabilities and rejects commands after failure/disposal through one liveness check. The object index exposes no mutable Map; it reuses the current system's pick-target array instead of duplicating all stars for each system. `PlanetList` uses fragment templates to preserve the detail panel's DOM layout.

`scripts/architecture-rules.mjs` defines layering/cycle rules; `check-boundaries.mjs` parses, traverses, and reports. Model code cannot depend on Vue, Three.js, or demo data. Object constructors may import asset contracts only as types. Black-hole numeric/shader layers cannot depend on Three.js or scene composition. Runtime cycles fail checks; type-only edges are excluded from initialization cycles.

Selection commands take IDs; invalid IDs or disposed instances return `false` without changing selection. `setStarRotationSpeed(starId, radiansPerSecond)` changes an available star's local-Y spin: normal default `sceneConfig.starRotationRadiansPerSecond = 2π/60`, pulsar default `2π/2.4`. Negative values reverse and zero stops. Nonfinite values, missing/black-hole nodes, and failed/disposed scenes are rejected without changing speed. Black holes still orbit. Speed is independent of population/selection; callers reapply overrides after rebuilding a snapshot.

`overview()` always clears drag inertia and starts a smooth global reset, even without selection or after an interrupted reset. It publishes an empty selection only when clearing an existing one; UI maintains no competing selection state. Middle-button `pointerdown`, `mousedown`, and `auxclick` prevent browser defaults/autoscroll but keep propagation for OrbitControls panning. Detail adjacency counts only edges with both endpoints available; source/explanation come from snapshot metadata.

## Lifecycle

1. Vue watches the canvas reference, snapshot reference, and retry count.
2. Replacement, retry, or unmount invalidates old loading, aborts model requests, and disposes the old scene. Late instances are disposed immediately.
3. Validate, then load required same-origin GLBs. Create the canvas only after all models arrive without cancellation. Register every acquired resource immediately in `ResourceScope`.
4. Partial initialization failure cleans acquired resources in reverse order and rethrows the original error.
5. Hidden documents stop RAF, discard wheel inertia, and retain approach progress. The first resumed frame has zero delta.
6. Drawing failure or WebGL context loss stops the loop, disables controls, and exposes retry.
7. Disposal removes listeners/observers and stops the loop, then releases instance resources, materials, textures, geometry, controls, canvas, and WebGL context.

Each scene owns its GPU resources. Sharing is confined to instances in that scene; there are no globally mutable Three.js singletons. Complete snapshot replacement rebuilds all objects and resets selection, so it cannot directly handle high-frequency streams.

## Planet orbits and instancing

The 27 instance batches share planet GLB geometry/materials. `model/orbitalPlane.ts` derives a common plane from Star ID independently of coordinates/input order. System planes tilt 12..32 degrees from world XZ; individual planets deviate 0..3 degrees with aligned normals. These are display parameters, not a universal astronomical distribution. [NASA's planetary-system introduction](https://science.nasa.gov/universe/stars/planetary-system/) relates the Solar System's approximately coplanar, same-direction orbits to its protoplanetary disk. The [ecliptic](https://www.nasa.gov/image-article/plane-of-ecliptic/) specifically denotes Earth's orbital plane; other systems here have their own common planes.

`model/layout.ts` produces uniform in-plane azimuths rather than spherical layouts. `model/orbit.ts` derives slightly inclined elliptical candidates from stable IDs/hints. `model/orbitPlanner.ts` sorts by hinted radius and stable ID, assigning unique semimajor axes and exclusive radial bands, with the star at a focus. Mean angular velocity is `0.028 * (20/a)^1.5`, giving constant `n²a³` for the shared display gravitational parameter rather than quantized periods. Hint magnitude bounds the semimajor axis from below; its direction projects onto the planet's plane, without guaranteeing the final start.

Stable IDs assign radial excursion `a*e` limits of 0.15..0.35 world units, also capped by the candidate ellipse. Outer eccentricity decreases with semimajor axis to avoid exponential system expansion from fixed eccentricity; this is a readability choice. Adjacent bands satisfy `q_outer - Q_inner >= 2R + 0.12`, where q/Q are periapsis/apoapsis and R is the actual mesh bounding radius. The first band also clears the scaled star; bounds include GLB origin offsets. Distance between positions is at least the difference of their radial distances, so this separation covers every phase/inclination without synchronized clocks, sampling, or timed avoidance. Requests explicitly include `starId`, preserve input snapshots, and remain order-independent. Initialization sorts in O(N log N), assigns in O(N), and performs no candidate search.

`model/systemLayout.ts` bounds each system by maximum apoapsis plus planet radius and includes stellar corona, complete pulsar beams, or black-hole optical volume. Pairwise center distance is at least `1.02 * (extentA + extentB) + 1`. Uniform expansion around the original mean center never reduces hinted spacing; noncoincident centers retain relative directions. Coincident centers spread around a stable-ID circle before applying the same constraint. Disjoint bounds imply disjoint bodies. Initialization takes O(S²) pair comparisons. Models, links, picking, and focus share display centers; callbacks still return original Star snapshots.

Runtime solves positions with five Newton iterations, with no per-frame collision detection, yielding, or off-orbit correction. See [NASA's Kepler laws](https://science.nasa.gov/solar-system/orbits-and-keplers-laws/). Elements, gravity, space, and time are display choices, not ephemerides or many-body simulation. Selection sets satellite time scales to overview 15, star view 6, details 0. Each planet wraps accumulated time by its own period to limit long-term floating-point phase drift. Planet selection pauses all orbits; black-hole flow and star spin use independent clocks. Matrices/position buffers are reused; each batch uploads one `DynamicDrawUsage` instance matrix set per frame, about 15.75 KiB for 252 instances, with no upload while paused. Static bounding spheres cover apoapsis plus model radius for culling/picking. Selection rings and approach read live instance matrices, not initial coordinates.

## Picking and body scaling

`createObjectIndex` owns entity indexing/exact picking; `createStarPicker` only provides enlarged-region ray fallback with no visible geometry. Overview first picks star bodies; inside a system, its planets join exact picking. Other systems' planets cannot intercept star clicks. Without an exact hit, normal stars use 3× the world diameter of the Surface bounding sphere; black holes use 3× complete optical `bodyRadius`, including the disk. Live centers avoid duplicate scale multiplication; neither uses planetary orbit extent.

Tolerance follows world matrices without altering display/layout. Pulsars still use only the actual core. Overlaps select the nearest entry in the clipped ray interval, breaking equal-distance ties by stable ID, including cameras inside a region. Exact eligible planets outrank enlarged regions. `selectPlanet` and `focusPlanet` likewise require entry into the owning system and never implicitly cross views. IDs, not object identity, drive lookup.

Stars use GLB uneven surfaces and spherical coronas with per-Star color; rocky planets are colored by entity type. `model/presentation.ts` scales stars by total owned planet count: 10→0.5, 30→1, 60→1.5, 120→2, 180→3, interpolated and clamped. Only the GLB root scales: surface/corona/picking follow, planets do not; orbit planning includes the scaled core. Additionally, `sceneConfig.starScale = 1.5 * Math.cbrt(2)` doubles previous star volume, increasing radius/diameter about 26%; `pulsarScale = 8` scales the entire pulsar eightfold. Collision bounds use the same multiplier. Black-hole scale is `0.75 * Math.cbrt(2)`, doubling volume without normal-star population scaling.

## Camera and background

`scene/sceneFraming.ts` owns fixed global position, map target, and distance bounds. Complete bounds/FOV determine far clipping and zoom limits, without computing an unused automatic camera position or projecting every body. Orbits do not seize the manual camera; bodies may leave the frame or pass behind it. Star focus distance is at least 90 and grows with system extent. Focus/wheel share live distance limits. Resizing updates overview target/clipping without changing the user's pose.

`createSceneFraming` and `fitSceneFraming` share `camera.initialPosition = [-178, 176, 1083]` for load, rebuild, and every overview return. The target remains the map center; zoom/far clipping include the fixed position to prevent clamping. Lensing synchronizes near/far camera planes every frame to avoid decoding expanded scenes with stale depth ranges.

`background/createCosmicBackground.ts` draws one fullscreen triangle at the far plane in the ordinary scene layer, without depth writes or picking. World viewing direction drives two low-frequency procedural-noise layers with quintic interpolation and weak detail. Warm/cool colors artistically suggest radiation. Grain is pixel-fixed, not time-flickering. Sky draws before bodies, sets no environment map/lights/exposure, enters black-hole background capture, and is not redrawn in optical/helper layers. `sceneConfig.cosmicBackground` controls enablement, radiation, and grain strength. Scene scope owns resources; no downloads/new texture assets.

`createDistantStars.ts` uniformly samples the celestial sphere with a fixed seed: 2200 soft-edged points in one Points batch, mostly dim, with GLSL separate from construction. Only view rotation affects projection; far-plane stars neither illuminate nor pick nor write depth, and foreground objects occlude them. They enter ordinary-layer lensing capture.

Each available star keeps independent spin speed, updating its root in the existing frame loop without new materials/textures or planet uploads. Invalid deltas are ignored; zero speed leaves orientation untouched. Delta folds by rotation period before multiplication to avoid overflow for huge finite input. Stars/coronas are meshes, not Sprites or camera-facing cards; planet selection rings remain separate UI decoration.

## Stellar orbits and editor

Optional `Star.appearance` defaults to normal `star`; `pulsar` selects the procedural model, while `black-hole` status takes precedence. Default Pulsar is the shared center for nine normal stars and one black-hole star, without planets/links. Normal stars retain 36 pairwise links. Appearance does not encode backend role. Without actual masses, the equal-weight mean of all star coordinates, including black holes, anchors Pulsar; neither status nor population implies mass. `GalaxyData.centerStarId` explicitly chooses overview center, currently Pulsar; final framing encloses every system around its display position, also used by reset/rotation.

`model/clusterLayout.ts` lays out every noncentral star regardless of status. `stellarOrbits.ts` permits at most 12 per layer, using only the inner layer for small populations and balancing added layers after capacity. Alternating systems by size avoids grouping large neighbors. `stellarPacking.ts` uses balanced second-order perturbations of semimajor axes (0.92..1.08 of base), phases, and planes rather than a regular polygon; eccentricity is 0.02..0.04. Around a 12-degree inclination baseline, it tries ±3, ±9, and ±15 degrees, choosing the smallest safe complete-cycle radius.

Same-layer mean angular velocity is shared so relative geometry repeats without overtaking. From 512 cycle samples, subtract the sum of pairwise maximum speeds times half a sample interval to bound unsampled distance; actual system bounds and clearance determine minimum overall scale. Each pair is evaluated once and symmetric positions reuse the bound. This is not merely an initial-overlap check. Different layers use disjoint full radial bands and different periods. The innermost base period is 600 virtual seconds, with outer periods scaling as layer size^1.5, using the existing Kepler solver. Approximate phase balance keeps the equal-weight center near Pulsar, not exactly coincident; a single star's own center moves with its orbit. These stable display orbits claim neither many-body dynamics nor a mass-weighted physical barycenter. Without Pulsar, layout is static.

Black-hole status changes model/satellite/link visibility, not orbital membership or center weight; its optical volume participates in clearance. `createStellarMotion.ts` translates system parents only; satellite motion remains local. Stellar time scale is independently 15 in overview/star view and stopped in details. `createStellarOrbitLines.ts` samples each planned ellipse's semimajor axis, eccentricity, and basis into 256 closed segments, including black holes. All guides form one static LineSegments batch: dim translucent blue-gray, depth-tested, no depth write/picking/frame upload, scene-owned. Satellites use 15/6/stopped. Each orbit wraps time; spin, pulsar radiation, and black-hole flow use real frame delta. Overview clipping covers apoapsis but does not promise every moving body stays visible.

Models/picking/links share live center arrays; visible links update one buffer per frame. Following translates camera, target, unfinished approach, and zoom anchor together. After initial approach, `CameraMotion.keepCentered` locks target to the selected star's live center and compensates camera/zoom anchor to preserve view angle/distance. Star view disables panning; rotation and off-center pointer zoom preserve centering. Overview/details restore panning; reset stops following, and planet approach ignores star centering. No frame-by-frame centers are written into input snapshots or Vue state.

The triangle in `GalaxyEditor.vue` opens an independent draft: 0..48 stars, each normal/black-hole, 0..180 satellites for normal stars, plus optional single Pulsar. At least one body is required; trimmed names have 1..64 characters. Satellites use the existing application `Planet` model with three entity types assigned cyclically, without a multiple-of-three requirement. `model/galaxyEditor.ts` validates on confirm and creates a complete snapshot with pairwise links only between normal stars. Black holes retain star IDs/names; no separate black-hole list. Deletion/state changes do not renumber other stars. Switching back within one draft preserves satellite count; confirming a black hole emits zero satellites. Cancellation/close never changes current data.

`App.vue` keeps the snapshot in a shallowRef and replaces it on GalaxyView's regenerate event. Existing composable cancellation/disposal rebuilds overview. Settings are page-memory-only, never persisted or sent to management. Validation failure retains the draft; load failure uses the existing scene error/retry path. Camera X/Y/Z beside the triangle are selectable, two-decimal text. Optional `cameraPosition` callbacks report changes at most ten times/second using existing RAF, with no Vue update while stationary. Rebuild clears readings; cancelled-scene late callbacks are ignored.

## Pulsar

`runtime/pulsar/` separates settings, GLSL, and construction without extra GLBs/textures/dependencies. Core model radius is 3.6; population factor 0.5 and overall factor 8 give example radius 14.4. The solid sphere has white-hot poles, faint magnetic-latitude texture, and blue limb detail. Finite-path Gaussian emission integration produces warm inner glow/blue scattering rather than a bright shell. Spin axis tilts 30 degrees from world Y; magnetic axis tilts 30 degrees from spin.

Two opposite closed cylinders proxy beam volumes, each using 64 fixed line-of-sight emission/transmittance samples. Each beam has a bright thin core, five outward-flowing filaments, and sparse blue outer emission, falling to zero before endpoints/proxy boundaries. Each beam/glow owns a local-camera uniform, transformed immediately before drawing without modifying the camera or creating another loop.

`magneticField.ts` constructs 24 dipole filaments in three radii with staggered azimuth and slight twist, using `r = r_equator * sin(theta)^2`. Bright cores and soft outer layers merge into two mesh batches; temporary geometry is released immediately. Materials move knots along arcs and fade with projected size at distance. Core/glow pulses depend on view/world-magnetic-axis angle, strengthening within roughly 20..7 degrees of a pole, not a global sine flicker. Existing speed controls/frame loop drive spin independently of planet pause. Filaments share a per-instance four-second closed phase; stopping spin does not stop radiation, and instances share no mutable clock.

Only the spherical core picks; layout/overview bounds cover rotating full beams. Both beams share instance geometry; all final geometry/materials are scene-registered. The lighthouse concept follows [NASA's pulsar introduction](https://imagine.gsfc.nasa.gov/observatories/satellite/xmm/pulsar.html). Visible beams/field lines, sizes, and the 2.4-second period are illustrative, not spectra or magnetosphere simulation. Beams use scene depth tests but no per-sample occlusion against other transparent volumes. [Automated evidence](../../docs/validation.md#full-sdk-regression-results) does not replace independent GPU visual acceptance.

## Black holes and optical composition

`Star.status` explicitly declares `available | black-hole`; Orion exemplifies the latter. Even if input retains old entities/edges, black-hole assembly creates no planets, planet pick entries, or incident links. The stellar core still picks/focuses. Map assembly scales the entire model by `0.75 * Math.cbrt(2)`, including disk/shadow; enlarged picking uses full optical volume. Optical values below are unscaled local units.

GLB contains Core, Horizon, and Accretion/Disc/Glow. Runtime draws only the closed Horizon optical volume. Core hides its surface but keeps ray picking; Disc/Glow retain offline structure, avoiding double composition of ordinary spherical occlusion and ray imagery. Clones share geometry/materials/textures without changing pose. `createBlackHole` gives each instance a clock and three finite-lived knots. `onBeforeRender` binds instance uniforms and sets `uniformsNeedUpdate`, preventing shared-material reuse of another node's time. Local camera position is also inverse-transformed from the world matrix before drawing and passed as a uniform, not inverted per vertex or interpolated as a constant.

`blackHole/flow.ts` aligns CPU knots and GPU main-flow velocity: `1.02 * (discInner / r)^1.5` radians/real second, retaining the three-times virtual clock but removing a common uniform-speed term. At fixed radius, inner/outer rotations take about 6.2/61 seconds. Inward speed is `0.36 * sqrt(discInner / r)` model units/real second. GPU backtraces texture origins through that spiral field; CPU integrates knot radius/angular displacement through the same field, accelerating as knots contract. The thin layer uses 0.68× angular and 0.65× inward velocity. These are display speeds, not physical calibration.

Two half-cycle-offset fields reset at zero weight every 24 virtual seconds (8 real seconds), limiting inner-disk shear blur. Planets independently follow 15/6/stopped. Knots spawn, spiral inward, stretch azimuthally, and fade over independent lifetimes, disappearing before the inner edge without teleporting outward. Frames reuse vectors and create no materials/textures; off-screen instances skip knot computation.

`blackHole/optics.ts` integrates nonrotating light trajectories with RK4: `u'' + u = 1.5 rs u²`. `lookupTextures.ts` packs a 513×768 R16F lookup at initialization, with impact range 0.02..28 densified around critical 4.5. It covers capture/escape; captured radius stays at the horizon instead of interpolating back to infinity. A separate 513×128 RG16F table solves finite-distance viewing phase/escape angle, replacing the near-view infinity approximation.

Per pixel, the first two ray-plane/disk intersections accumulate emission/transmittance in path order, followed by capture-shadow composition. There are no preset arc outlines or camera-facing model rotation. Both intersections share emission/frequency-shift models; path occlusion determines visibility without manually switching disk-image energy by inclination. The inner disk is the nonrotating innermost stable circular orbit, `3 rs`, about 5.196; the outer edge is 24, fading from 19.5 to avoid truncating the foreground disk and exposing a detached broad secondary image. Knots stay inside this emitting disk.

Frequency shift uses conserved light angular momentum projected onto the disk axis and circular orbital angular frequency, including gravity/motion rather than straight source-camera direction. Bounded shifts and cubic intensity approximate RGB temperature/brightness, not full blackbody integration. The hot main ring is warm gold with emission increased at most 45%; the darker amber exterior retains density gaps and left/right asymmetry. Near exact side view, inclination sine below 1e-6 becomes zero to prevent neighboring pixels choosing different intersections from interpolation error.

Secondary images always query real disk intersections and retain radial heating/edges. Only when local image width approaches or falls below twice the pixel footprint does a continuous area-coverage approximation blend in. Distance alone must not replace a broad arc with a solid band at fixed emission radius. Coverage blends color without white hard edges from transparent boundaries. A 768×1 RGBA32F range table stores inner/outer bounds, emission-weighted radius, and energy correction; initialization integrates 32 radial emission samples per row instead of drawing the entire narrow image at peak brightness. Explicit nearest-two interpolation needs no float-linear-filter extension.

A 256×1024 flow texture bakes an azimuth-periodic 2D density field. RGBA hold long filaments, density clumps, intermittent medium-scale bands, and thin mist. Radial perturbation stays near filament width to avoid thick curls. Mipmaps/anisotropy filter subpixel flicker; corrected derivatives handle angular seams. Near views emphasize filaments, far views retain medium bands. Main/thin layers each use dual-phase advection: four texture samples per ordinary intersection. Coarse-mip scattering retains only main-layer average energy.

Density drives emission and transmission; outer disks thin gradually. Approximate inclination extends thin-disk path length so grazing views are denser; secondary-image subpixel coverage also preserves transmittance. Nonlinear radial coordinates pack inner filaments more tightly and spread outer mist. Main emission centers near `1.36 * discInner`; outer density fades early rather than keeping a uniformly bright circular edge, reaching zero within the existing optical volume. Radial scattering widens outward, retaining compact inner gold and dim amber/translucent outer threads without enlarging the model. Scene-owned textures are shared across black holes and never rebuilt/uploaded in frame loops. Optical volumes draw once from either inside or outside.

With black holes, `lensingRenderer.ts` renders ordinary scene color/depth into one shared target, copies it to canvas, draws black holes, then visible helper markers (selection rings/stellar orbit lines). `rendering/layers.ts` centralizes layer IDs; `rendering/fullscreenTriangle.ts` supplies shared construction for background/color-depth copying. Helper/optical layers do not depend back on composition. Orbit guides bypass background capture/refraction but depth-test against composited depth. Without black holes, helpers stay in the ordinary layer for one draw.

Targets resize/reuse at drawing-buffer dimensions with at most four MSAA samples. No black holes retains the original single-pass path. Scene scope owns resources; exceptions restore render target/camera layers. `blackHole/shaders/lensing.ts` projects background directions from lookup escape angles, attenuates finite-distance offsets by depth, and rejects foreground samples. Refraction is local to the optical volume and fades at its edge. Off-screen directions fall back to original background instead of stretching edge pixels indefinitely.

This screen-space approximation lacks off-screen/occluded information. Transparent halos/links have no independent depth. Multiple black holes share the unrefracted background without recursive imagery. Luminance compression whitens only the highest energy, retaining warm inner gold, outer amber, and dark gaps. Continuous inner emission carries brightness; weaker moving filaments have narrower peaks/shorter angular segments to avoid white concentric bands. Outer emission/optical thickness fades earlier; restrained broader scattering surrounds the main ring. This color adjustment still lacks visual acceptance.

Four fixed coarse-mip samples on either side add local radial scattering only around the actual emitting edge, not a fixed halo. This is artistic scattering, not full 2D bloom. Projected shadow radius 14..65 drawing-buffer pixels smoothly raises texture detail. Distant knots enlarge and fine texture receives stronger filtering without changing disk geometry. Knot motion fades at 2..8 pixels to avoid tiny-node flicker; at most three knots are computed per visible instance. Angular segmentation separates clumps and baking selects density peaks without lifting low-density tails into broad bright bands. Additional far-view derivative filtering is 1.15×, alongside mipmaps, anisotropy, and seam filtering. The thin layer is emission/transmission approximation, not 3D fluid simulation.

`blackHole/shaders/volume.ts` adds foreground rim only near side view. Rays intersect the outer cylinder/height interval, clip to the node foreground, then take four emission-integration steps. Elliptical maximum half-thickness 0.28 matches GLB. Screen footprint filters the rim; path sample spacing selects explicit mip levels to prevent sparse-sample barcode flicker. The loader derives disk normal/transforms from GLB Accretion orientation. The optical volume writes virtual depth near the node instead of using the front bounding sphere as a solid occluder. This is not curved-ray/scene intersection; other-body distortion comes from screen-space sampling, not complete scene ray tracing.

Emission, flow time, and RGB range are artistically adjusted. Disk imagery retains two thin-disk intersections; exact side-view rim has finite thickness through local straight-ray integration only. There is no full volumetric relativistic transfer, Kerr rendering, or physical steady accretion fluid. By explicit maintainer choice, captured-shadow interiors include faint stylized curvature from local spherical normals: dark shading, warm rim light, and a cool weak outline. Only visible shadow pixels compute it, composed after disk imagery using remaining transmittance. It adds no core surface, texture, draw call, shadow-outline change, or depth change. It suggests volume, not emitting/reflecting horizons or a result of the trajectory equation. Display status cannot diagnose real failures.

The frame scheduler caps rate by accumulated deadlines and measures completed draws. Delta is capped at 0.1 seconds to avoid excessive catch-up. Approach/zoom use delta, independent of refresh rate. Wheel events change target distance only, scaling camera and target together to preserve the pointer anchor. `FrameOrbitControls` defers OrbitControls' event-time `update()` to frame-time `updateFrame()`. Events accumulate rotation/panning; each frame consumes it with a 90 ms time constant, making damping independent of event density. Focus clears old inertia through public APIs while preserving pose. Pointer-down immediately interrupts approach/zoom; the 5-pixel threshold distinguishes clicks from drags only. Hiding clears drag inertia with no catch-up on resume.

## Extension boundaries

Live management uses [Go Astrolabe](../../astrolabe/README.md#management-login-and-admin-integration). Management login, topology DTOs, and errors belong in explicit contracts and independent adapters. Initial deployment management accounts authenticate only, without role levels or Sector/Spectrum permission tables. Fixed in-memory sessions and same/cross-origin requests follow backend contracts. Browser login is separate from infrastructure admission: renderers hold no service credentials and never connect directly to Star/Pulsar. Astrolabe submits Almanac edits to Polaris; durable commit and individual Star installation remain distinct. The frontend maintains no compensation queue and never writes observed views back as authoritative baselines.

New planet types require coordinated changes to presentation types, colors, construction, and tests. Frequent topology updates need explicit deltas, batching, selection preservation, and stale-data semantics. Large-scale LOD, instancing, culling, and label decisions require actual GPU measurements. Management authentication/networking are implemented separately; the demo renderer does not thereby acquire live topology, application routing, or a global state store.
