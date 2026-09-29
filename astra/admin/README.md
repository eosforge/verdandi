# Astra Admin

[English](README.md) | [简体中文](README_CN.md)

> The target map uses Star stars, Planet relays, and application satellites; see the [architecture](../docs/architecture.md). The demo still represents application entities as planets; that display migration is not implemented. The separate live management mode connects to Astrolabe. See the [validation record](../docs/validation.md) for evidence boundaries.

Astra's management frontend uses Vue 3, TypeScript, Vite, Naive UI, and Three.js, with [Astrolabe](../astrolabe/README.md#management-login-and-admin-integration) as its backend.
The full-viewport demo has no sidebar. It contains nine normal stars, one black-hole star, and an independent pulsar. Atlas, Lyra, and Vega have 36, 48, and 60 planets; Sirius, Capella, Rigel, Procyon, Altair, and Deneb have 12, 15, 18, 21, 24, and 18. The nine normal stars contain 252 Registry, Subscriber, and Publisher entities and 36 distinct pairwise connections. Orion and Pulsar have neither planets nor connections.

These entities and connections are local examples, not live cluster state. The top-level switch opens a separate live management mode; backend failure never silently falls back to the demo. Star links are currently hidden, with their data retained and no drawing or buffer updates while hidden. `sceneConfig.showStarLinks` controls visibility.

## Astrolabe integration

Admin supplies the login and management UI; Astrolabe supplies backend operations and data adapters. The [management contract](../astrolabe/README.md#management-login-and-admin-integration) is authoritative. The first version authenticates management users without role hierarchies, group authorization, or menu permission tables. Live mode implements Cookie login, directory/metrics observation, complete Almanac reads, single-key Set/Delete, and redacted credential management. [Validation](../docs/validation.md) separates automated checks from browser acceptance; these features do not turn the demo map into live topology.

The network layer requests management data and operations only from Go Astrolabe. The backend owns [accounts](../astrolabe/README.md#management-account) and authentication. [Sessions](../astrolabe/README.md#management-sessions) and [same-origin/cross-origin deployment](../astrolabe/README.md#same-origin-and-cross-origin-deployment) share one contract: no separately stored frontend login token and no authentication downgrade after a CORS failure. Browsers receive no Pulsar deployment passwords, signing credentials, or private keys; a user login does not register a node. A separate adapter converts network data to `GalaxyData`; renderers consume that presentation contract without login state or backend protocol in frame updates.

Admin [edits Almanac through Astrolabe and commits to Polaris](../astrolabe/README.md#saving-and-publishing). The UI distinguishes Polaris durable commit from installation at each Star; neither a page edit nor one Star applying it proves authoritative durability. Editing baselines come only from Polaris. Filtering, pagination, failed reads, and omissions at a lagging Star must not become deletion instructions. Partial success, uncertain outcomes, unreadable backends, and stale views remain distinct; the frontend neither falls back to demo data nor resends an old request automatically. The editor submits a complete Base64 Buffer with an explicit version. `__auth/comet` uses the dedicated credential API and never echoes an existing SECRET. Partial snapshots do not replace the old view; uncommitted and uncertain outcomes are displayed separately.

## Local development

Use Node.js 24 LTS and pnpm 12.3.4. `.node-version` pins the validated Node version, 24.21.0. From `admin/`:

```powershell
fnm use
pnpm dev
```

The development server listens locally by default; use `pnpm dev --host 0.0.0.0` for LAN access. Before first installation, a maintainer must explicitly approve the specific dependencies in `package.json`, their source, and installation location. Only then run `pnpm install --frozen-lockfile`. Dependencies come from the official npm registry into this directory's `node_modules/`. pnpm uses the user's store/cache settings; on the current workstation these are `D:\Program Data\pnpm\store` and `D:\Program Data\pnpm\cache`. Routine validation uses existing dependencies and does not include installation, upgrades, browser downloads, or publication.

## Development and validation

| Command | Purpose |
| --- | --- |
| `pnpm dev` | Vite development server and hot reload |
| `pnpm format` | Prettier formatting |
| `pnpm format:check` | Format checks |
| `pnpm check:boundaries` | Layering, runtime dependency cycles, and lazy-loading boundaries |
| `pnpm test` | Built-in Node.js tests, without a browser or additional test framework |
| `pnpm type-check` | Strict TypeScript and Vue template checks |
| `pnpm build` | Type checks and production output in `dist/` |
| `pnpm check` | Formatting, boundaries, regression tests, type checks, and production build |
| `pnpm preview` | Preview existing build output locally |

`pnpm check` also works in CI with dependencies already prepared. No repository workflow automatically installs dependencies or publishes. Unit tests cover motion, input, scheduling, snapshot validation, instancing, orbital picking, and cleanup; they do not replace WebGL verification in a real browser.

## Layout and extension

```text
src/
  main.ts                      # Startup
  app/                         # Composition, layout, global styles
  features/galaxy/
    index.ts                   # Public feature entry
    model/                     # Read-only presentation contracts, layout, validation
    data/                      # Local demo snapshots
    ui/                        # Canvas container, planet details, entity list
    composables/               # Vue lifecycle and asynchronous scene loading
    runtime/                   # Scene entry, input, frame scheduling, resource scopes
      scene/                   # Canvas setup and the single selection controller
      objects/                 # Systems, instance batches, motion, picking, decoration
      blackHole/               # Configuration, numerical optics, materials, instances
        shaders/               # Optical volume, disk emission/rim, background lensing
      pulsar/                  # Configuration, procedural models, volumetric beams
      background/              # Cosmic background, distant stars, separate shaders
      rendering/               # Render layers and shared screen geometry
      assets/                  # Star, planet, and black-hole GLB models
      materials/               # Stellar surfaces/coronas and planet textures
scripts/                       # Offline checks and model generators
tests/                         # Node.js regression tests
docs/                          # Architecture and browser acceptance
```

`src/app/App.vue` injects `demoGalaxy` into `GalaxyView`. Renderers neither import demo data nor request management APIs. Asset loaders fetch only the same-origin GLB files shipped with the application, on demand. Network DTOs must pass through a data adapter to `GalaxyData` before replacing a snapshot. Snapshot replacement currently rebuilds the scene and returns to overview; frequent live updates need a separate incremental-update/backpressure design.

See [architecture and lifecycle](docs/architecture.md), [browser acceptance](docs/verification.md), and [Admin maintenance rules](../docs/project.md#admin). Repository rules live in [AGENTS.md](../AGENTS.md), [development](../docs/development.md), and [coding and file organization](../docs/coding.md).

## Current interactions

- Left drag rotates, held middle drag pans, and the wheel zooms continuously around the pointer. Rotation and panning share frame updates with time-based damping. Pointer-down takes control immediately; focus clears old drag inertia.
- Normal-star picking diameter is three times its surface diameter; black-hole picking is three times the complete optical diameter, including the disk, independent of planetary orbit extent. Exact bodies and eligible planets take precedence; overlapping enlarged regions use the nearest ray entry. Overview selects stars only and approaches smoothly in about 1.2 seconds without opening details. Planets become selectable only inside their own system.
- Planets follow distinct elliptical semimajor axes near a shared plane, within 3° inclination variation. The star occupies a focus; near-star motion is faster and outer periods longer. Each planet has its own radial band, with model radius and clearance included independently of phase. Systems have independent planes.
- Satellite orbital time scales are 15 in overview and 6 in star view. Stars, including black holes, use a separate constant 15 scale regardless of selection. Selecting a planet pauses all orbital motion, but not rotation or radiation flow.
- System spacing accounts for complete orbital extent and leaves gaps. Overview, zoom limits, links, and focus use the expanded layout. Detail lists switch entities; “approach planet” targets its actual orbital position.
- Double-clicking empty canvas, pressing Escape while the canvas is focused, or closing details returns smoothly to overview and clears details. This also resets panning, rotation, and zoom with no selected node.
- Actual FPS appears at top right. A bottom-right triangle opens the map editor; its left side shows copyable world camera X/Y/Z coordinates to two decimals.
- Orion is a black hole without planets, links, or an X marker. Its body and enlarged picking region focus it without planet details.
- Normal and black-hole stars orbit Pulsar. Up to 12 share a compact inner layer with varied radius, phase, and inclination; additional layers appear only after capacity is exceeded. The equal-weight center stays near Pulsar. Inner orbits share a period to avoid overtaking; outer periods are longer. Planning checks a complete cycle with safety margin between samples.
- After focus, the camera follows the moving star at screen center; rotation and zoom remain centered on it. Star view disables panning, while overview and planet details retain it.
- First load, rebuild, and double-click overview reset use camera world position `(-178, 176, 1083)`, looking at the map center (Pulsar when present). Automatic framing estimates distance and clipping without replacing that position. Bodies may subsequently leave the frame or pass behind the camera.
- Dim blue-gray lines trace the actual tilted elliptical orbits of stars and black holes. These depth-tested guides bypass gravitational lensing. Inter-star links stay hidden.
- The deep-space background uses soft, dim warm/cool radiation textures, faint grain, and sparse distant stars. It changes no planet materials, lighting, or exposure. Directions remain fixed with no translation parallax or random flicker. This is an artistic background, not a measured microwave map.
- Pulsar centers overview. Its white-hot core has gradual glow, bipolar beams, and three magnetic-field layers with flowing filaments. Default rotation is one revolution per 2.4 seconds; clicking its core focuses it.
- Normal stars use `1.5 × ∛2` linear scale (about 1.89) after population scaling; black holes use `0.75 × ∛2` (about 0.945). Both double their previous volume. Pulsar scales by 8. Picking and orbital clearance scale with them.
- The editor supports up to 48 stars, each normal or black-hole state; normal stars allow 0..180 satellites. Pulsar is enabled separately, at most one. State changes preserve ID and name. Black holes generate no satellites or links; switching back restores connection participation.
- Confirmation rebuilds and returns to overview; cancellation preserves the original map. Settings affect only the current page. Without Pulsar, stars use a static layout. Display orbits do not simulate many-body gravity.
- Hidden pages stop drawing. Initialization or WebGL context failure shows an error and retry action.

## Implementation boundaries

- The 252 planets use 27 instance batches and individual Kepler periods. Radial bands and system bounds are planned at initialization, not every frame. Outer ellipses limit radial excursion to control system size. Spatial scales, spacing, and time are presentation choices, not physical interstellar distances or many-body simulation.
- Star size is piecewise linear in planet count: 10→0.5, 30→1, 60→1.5, 120→2, 180→3. Normal stars rotate once per minute by default. A per-node radians/second control also supports Pulsar.
- Normal stars, planets, and black holes use first-party GLB assets; Pulsar uses procedural spheres, closed beam volumes, and magnetic tubes. Resources have centralized cleanup; no third-party models/textures are downloaded. See [assets](src/features/galaxy/runtime/assets/README.md).
- Black holes combine precomputed light trajectories, disk emission, inward spiral flow, and screen-space background lensing. Warm gold streams, dimmer outer disks, and a three-times flow clock are display effects, not fluid simulation. Lensing captures scene color/depth, cannot recover off-screen or occluded information, and does not recursively refract multiple black holes. See [architecture](docs/architecture.md) for algorithms and ownership.
- Target rendering is 60 FPS with DPR capped at 1.5. Actual performance depends on GPU, browser, resolution, and refresh rate; this is not a large-scale qualification claim.

The demo still rebuilds complete snapshots. High-frequency deltas, backpressure, and live backend topology need explicit adapter/control contracts.

## License and status

Admin uses the repository [MIT License](../LICENSE). It remains an Alpha foundation with implemented management login, Astrolabe integration, and data editing. Complete live 3D topology is pending. Browser/GPU acceptance, large-scale performance, and long-term stability lack evidence; demo appearance and historical automated results do not qualify the current version.
