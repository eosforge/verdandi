# 检查服务基础骨架. 只使用已准备的工具和项目缓存, 失败立即停止, 不安装软件.
param(
    [ValidateSet('all', 'peer', 'supervisor')][string]$Service = 'all',
    [ValidateRange(1, 8)][int]$Jobs = 2,
    [switch]$Race,
    [ValidateRange(0, 300)][int]$FuzzSeconds = 0
)
if ($MyInvocation.InvocationName -eq '.') { throw 'Execute this script; do not dot-source it.' }
$ErrorActionPreference = 'Stop'
$verdandiRoot = (Resolve-Path -LiteralPath "$PSScriptRoot/..").ProviderPath

# 所有入口均使用绝对脚本路径, 调用终端的工作目录和环境保持不变.
function Invoke-Checked([scriptblock]$Command) {
    & $Command
    if ($LASTEXITCODE -ne 0) { throw "Service check failed with exit code $LASTEXITCODE." }
}

# 服务构建不再调用 protoc, 协议一致性由下面独立的只读生成检查负责.
function Invoke-PeerCheck {
    & "$verdandiRoot/sdk/run-tool.ps1" -Executable cargo -WorkingDirectory "$verdandiRoot/peer" -Environment @{
        CARGO_HOME = "$verdandiRoot/build/deps/cargo"
        CARGO_TARGET_DIR = "$verdandiRoot/build/peer/target"
        CARGO_NET_OFFLINE = 'true'
    } -ToolArguments $args
}

Invoke-Checked { & "$verdandiRoot/scripts/generate-proto.ps1" -Check }
Invoke-Checked { Invoke-PeerCheck fmt --manifest-path ../proto/generator/Cargo.toml --all --check }
Invoke-Checked { Invoke-PeerCheck clippy --manifest-path ../proto/generator/Cargo.toml --frozen --all-targets --jobs $Jobs '--' '-D' warnings }
Invoke-Checked { Invoke-PeerCheck test --manifest-path ../proto/generator/Cargo.toml --frozen --jobs $Jobs }
if ($Service -in @('all', 'peer')) {
    Invoke-Checked { Invoke-PeerCheck fmt --all --check }
    Invoke-Checked { Invoke-PeerCheck clippy --workspace --frozen --all-targets --all-features --jobs $Jobs '--' '-D' warnings }
    Invoke-Checked { Invoke-PeerCheck test --workspace --frozen --all-features --jobs $Jobs '--' "--test-threads=$Jobs" }
    Invoke-Checked { Invoke-PeerCheck doc --workspace --frozen --no-deps --jobs $Jobs }
    Invoke-Checked { Invoke-PeerCheck build --workspace --frozen --release --bins --jobs $Jobs }
    Invoke-Checked { & "$verdandiRoot/build/peer/target/release/peer.exe" --version }
    Invoke-Checked { & "$verdandiRoot/build/peer/target/release/planet.exe" --version }
}
if ($Service -in @('all', 'supervisor')) {
    $verdandiGo = Join-Path $verdandiRoot 'supervisor/go.ps1'
    $formatting = & gofmt -l "$verdandiRoot/supervisor/cmd" "$verdandiRoot/supervisor/internal"
    if ($LASTEXITCODE -ne 0 -or $formatting) { throw "Go formatting check failed: $formatting" }
    Invoke-Checked { & $verdandiGo mod tidy -diff }
    Invoke-Checked { & $verdandiGo mod verify }
    Invoke-Checked { & $verdandiGo vet -p $Jobs ./... }
    Invoke-Checked { & $verdandiGo test -p $Jobs -count=1 -shuffle=on -timeout=30s ./... }
    if ($FuzzSeconds -gt 0) {
        # Go 自带覆盖引导 fuzz, 每个入口单独运行, 语料缓存仍在项目 GOCACHE 中.
        Invoke-Checked { & $verdandiGo test -run='^$' -fuzz=FuzzAccountFileNeverAcceptsInvalidRoles "-fuzztime=${FuzzSeconds}s" -parallel=2 ./internal/admission }
        Invoke-Checked { & $verdandiGo test -run='^$' -fuzz=FuzzPersistedMemberDecodeRoundTrips "-fuzztime=${FuzzSeconds}s" -parallel=2 ./internal/membership }
    }
    if ($Race) {
        # cgo 只在此子进程启用. 缺少 C 编译器时明确失败, 不自动安装或静默跳过.
        Invoke-Checked {
            & "$verdandiRoot/sdk/run-tool.ps1" -Executable go -WorkingDirectory "$verdandiRoot/supervisor" -Environment @{
                GOMODCACHE = "$verdandiRoot/build/deps/go/pkg/mod"
                GOCACHE = "$verdandiRoot/build/cache/go"
                TEMP = "$verdandiRoot/build/tmp/supervisor"; TMP = "$verdandiRoot/build/tmp/supervisor"; GOTMPDIR = "$verdandiRoot/build/tmp/supervisor"; GOMAXPROCS = '2'
                GOTOOLCHAIN = 'local'; GOPROXY = 'off'; GOSUMDB = 'off'; GOWORK = 'off'; GOFLAGS = '-mod=readonly'; CGO_ENABLED = '1'
            } -ToolArguments @('test', '-race', '-p', "$Jobs", '-count=1', '-shuffle=on', '-timeout=120s', './...')
        }
    }
    Invoke-Checked { & $verdandiGo build -trimpath -p $Jobs -o ../build/supervisor/supervisor.exe ./cmd/supervisor }
    Invoke-Checked { & "$verdandiRoot/build/supervisor/supervisor.exe" --version }
}
Write-Output 'Service checks passed.'
