# Astra C++26 Services

Current C++ servers target Linux x64/GCC 16.2.0. Implemented paths include Pulsar SQLite membership, Go Polaris, Star's three domains/login/Watch, single-stream multinode recovery, Comet C++ writes/reads/keepalive, essential Go Astrolabe, metrics, and Admin adapters. Planet remains frozen. See [status](architecture.md#status) and [validation](validation.md).

[Architecture](architecture.md) owns scope, [maintenance](project.md) source duties, [storage](../common/README.md) native domains, and [Pulsar](../pulsar/README.md) time. [Polaris](../polaris/README.md) persists Almanac; [Astrolabe](../astrolabe/README.md) is database-free management/observation. [Comet C++](../comet/cpp/README.md) is the first SDK; [Go](../comet/go/README.md) is future work, not its prerequisite.

## Deployment and wiring

There is no standalone architectural fallback. Single-machine/All-in-One deployments still complete Pulsar admission/first calibration, initial Polaris Almanac, and initial peer/dynamic synchronization. Colocation, missing material, and offline dependencies cannot bypass initialization. [Architecture](architecture.md#startup-and-operation) owns startup budgets/running-node degradation.

Star business authentication/TLS are independent, both enabled by default and individually explicitly disableable; configuration errors never downgrade automatically. APIKEY/APISECRET authenticate login, not scope permissions/registration ownership. External entries always isolate __ Sectors. Credentials arrive only from Polaris internal Almanac, never a standalone credential file/second bootstrap authority.

Missing/invalid internal material fails; temporary unreachability follows initialization/backoff. Business TLS requires valid server certificate/key, and disabling it does not unprotect infrastructure. A valid empty credential table completes initial sync but, with auth enabled, rejects all Comet logins rather than allow anonymity.

### Listeners and configuration entries

Business/internal services use separate gRPC Server/Builders. Adding plaintext business ports to a Server containing internal RPCs would expose those RPCs and is prohibited.

| Entry | Caller/services | Controls |
| --- | --- | --- |
| Public business | Comet Session, Almanac reads, Catalog/Ephemeris | Independent auth/TLS; always isolate __ |
| Internal system | Authorized peer streams/required control | Internal TLS/Pulsar identity; role-limited duties |
| Metrics HTTP | Astrolabe /metrics reads | Optional independent readonly listener/budget; no external monitoring prerequisite |

Peers cannot publish Almanac for Polaris; Astrolabe cannot bypass it. APIKEY is not an internal role. Internal listeners precede public readiness for cold recovery, independent of browser login/unavailable Comet sessions.

Star initiates synchronization to its directory's sole Polaris, receiving pushes/reporting installation without another inbound injection RPC. The outgoing stream shares controlled storage with the two servers; internal identity/TLS remain independent of Comet switches. See [Polaris stream](../proto/README.md#polaris-stream).

Each server bounds streams/messages/resources/shutdown work. Separate servers do not mean dedicated CPUs; bound snapshots/parsing/sending. The preauthentication Hello limit of 4 KiB is not the authenticated-message limit. Internal messages default to 8 MiB, negotiating the smaller peer limit without removing business budgets. Long streams use callbacks/async paths rather than blocking service threads in synchronous Read.

### Business listener and material options

These options are implemented; [validation](validation.md) identifies actual tested sources/configurations.

| Option | Meaning |
| --- | --- |
| --listen / --advertise | Internal listener/reachable registration address, not Comet address |
| --comet=IP:PORT | Explicit separate business listener, never implicit internal-port reuse |
| --auth=true\|false | Comet login only; default true |
| --tls=true\|false | Star–Comet TLS only; default true |
| --comet-identity=directory | External cert.pem/key.pem; never implicitly borrow node identity |
| --metrics=IP:PORT | Separate readonly HTTP metrics; disabled by default, independent of Comet TLS |
| COMET_CA_FILE CMake cache | Optional SDK-embedded CA, not a server private key |

Conflicting addresses fail. Explicit comet-identity with business TLS off is invalid. No --standalone, local-credentials bootstrap, or separately configured Polaris address exists. Directory discovery, Almanac intake, and initial dynamic sync gate public opening; dynamic-source timeout follows bounded degradation.

[Comet TLS](../comet/cpp/README.md#tls) describes external/embedded trust and certificate provenance. Without business TLS, credentials/payloads lose confidentiality on that link; login is not encryption. See [Astrolabe observation](../astrolabe/README.md#live-observation); absence of external monitoring does not block initialization.

## Build

From root with existing tools/caches, without implicit downloads:

```bash
bash build.sh build --profile debug
bash build.sh build --profile release
```

Output under build/cmake/<profile>/ contains C++ star/planet/pulsar, Go polaris/astrolabe, and Comet static libraries. The C++ Astrolabe placeholder target is removed; Planet remains frozen. [Validation](validation.md) owns profile results. Windows can browse the solution/build standalone Comet; ordinary server build/test rejects Windows without automatic remote execution/compiler switching.

### Visual Studio and Windows adaptation

Generate from Astra root with existing Python, CMake, and Visual Studio:

```powershell
.\build.ps1 solution --prefix D:\path\to\approved\comet-msvc\install
```

Open build/Astra.sln. Comet, Star, Pulsar, Common, and Planet projects live in build/<Project>/; coordinator metadata in build/Astra/, without overwriting the root build/CMakeCache.txt Ninja configuration. CMake generates .vcxproj; the tool publishes .sln from actual GUIDs/configurations/paths and supports CMake versions defaulting to .slnx. Regenerate after source additions/removals.

Ordinary solution uses LANGUAGES NONE: no compiler probe, compilation, tests, or downloads. Default --prefix is deps/comet-msvc/install under ASTRA_CACHE_ROOT (default build/). Outputs always stay in Astra/build regardless of dependency cache. Use solution --browse-only for browsing only.

Only Release/x64 with prepared /MD dependencies is configured. Explicit Build Solution/Comet configures/builds the standalone SDK in build/Comet/native/, without tests/install. Star/Pulsar/Common/Planet are marked browse only and fail explicit builds; empty-target success is not compilation. No Debug configuration masquerades with Release dependencies.

solution --runtime performs experimental C++26 compilation probes for actual reflection, annotations, expansion, and contracts. Missing capabilities fail. Even supported syntax cannot unlock full services until Windows time-quality, stdout-backpressure, and database-initialization durability are implemented. No automatic browsing fallback/contract reduction. This command needs current build authorization.

Nonreflection native adaptation has an independent verification project, executable with build/test authorization:

```powershell
cmake -S cmake/platform -B build/Astra/platform -G "Visual Studio 18 2026" -A x64 -DBUILD_TESTING=ON
cmake --build build/Astra/platform --config Release --parallel 4
ctest --test-dir build/Astra/platform -C Release --output-on-failure --no-tests=error
```

It reuses actual Clock/Filter, native timing/random/stop, Metrics HTTP state machine, and cases. Common/Star objects/artifacts live in build/Common/platform/ and build/Star/platform/. It requires no gRPC, reflection, or new tools and does not prove complete services/mixed clusters. See [Windows design](features/windows.md) and [actual results](validation.md).

Ordinary builds do not run protoc. [dependencies.lock.json](../dependencies.lock.json) alone owns versions/sources/checksums; tools live in build/tools, dependencies in build/deps/astra. Committed common/src/generated is updated only through [explicit generation](../proto/README.md#generation), never hand-edited. gRPC uses bundled BoringSSL, not system OpenSSL. Ephemeris recovery capabilities use that Crypto library; core-only native state builds still require prepared gRPC/Protobuf exports but neither link nor run gRPC services. See [licenses](../licenses/README.md).

Non-core-only tools/build.py also requires project Go 1.27.1/approved modules. Go children use build/deps/go and build/cache/go with GOPROXY=off, GOTOOLCHAIN=local, readonly modules; missing dependencies fail. Pulsar's C SQLite consumes only cached/checksummed amalgamation; Star business state remains memory-only.

--jobs/--test-jobs separately cap concurrency selected from actual CPUs, available memory, and cgroup v2 budgets. Explicit caps cannot exceed estimated resource budgets; Go tests/CTest run sequentially. Commands still need current permission.

tools/prepare_dependencies.py fetch/build is separate dependency preparation requiring specific download/build authorization. Missing tools fail without installation. TSan uses a separate instrumented linux-gcc16-tsan prefix, preserving ordinary dependencies; ordinary virtual-memory limits cannot be imposed mechanically on shadow address space.

Default source/cache roots are current Astra/build. Reusing previous caches requires explicit ASTRA_CACHE_ROOT without moving installed prefixes/build trees. Sixteen configured VM cores do not justify sixteen simultaneous high-memory compilers.

## Startup

Identity directories contain ca.pem, cert.pem, key.pem, admission.pub, and login.json. Store passwords in protected login.json, never command lines/logs. Repository identities are tests only.

```bash
build/cmake/debug/star --listen=192.168.0.119:7442 --super=192.168.0.119:7440 --galaxy=alpha --group=east --identity=build/deployment/star-a
build/cmake/debug/planet --listen=192.168.0.119:7443 --super=192.168.0.119:7440 --galaxy=alpha --group=east --identity=build/deployment/planet-a
```

C++ uses --galaxy; do not apply legacy arguments to every program. --super targets Pulsar Orbit admission; responses carry Pulse addresses for sampling. An empty address provides connectivity admission only, not qualification for new finite leases.

TLS 1.3/h2; cross-language test certificates use ECDSA P-256/SHA-256 and admission signatures Ed25519. TLS server certificates and signed bearers have separate duties. Wildcard listeners require reachable --advertise; actual options/ranges/defaults are in configuration/--help. Executables determine roles; --worker-threads is unsupported.

stdin closure does not stop services. Linux SIGINT/SIGTERM stops admission and cancels/drains owned RPCs before exit. Argument errors return 2, runtime errors 1, normal exit 0. Planet currently has no business downstream service; initialized/upstream are not business-sync readiness.

## Verification and delivery

Tests/prerequisite builds need current authorization; see [methods](../tests/README.md) and [results](validation.md). Installation preserves root/third-party terms, excludes development GCC RPATH, and needs matching runtimes/target-distribution acceptance. Current scaffolding promises neither cross-distribution binary compatibility nor production readiness.

## Independent repository and existing caches

Run from Astra root; tools/ owns tooling and tests/ cross-process scenarios. Caches/tools default to build/. Explicitly reuse an approved premigration environment for one command:

```bash
ASTRA_CACHE_ROOT=/home/ubuntu/verdandi/build bash build.sh build --profile release
```

ASTRA_CACHE_ROOT selects existing tools/dependencies only; new CMake output remains build/cmake/<profile> and Go caches/temporaries stay in Astra/build. Old CMakeCache, virtual environments, and installed prefixes may contain absolute paths: renaming does not prove reuse. Migration authorizes no third-party downloads/rebuilds. Old evidence retains original paths/source identity. Formal Astra source packages exclude parent SDKs, caches, credentials, logs, and backups.
