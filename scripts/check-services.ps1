# 检查服务基础骨架. 只使用已准备的工具和项目缓存, 失败立即停止, 不安装软件.
param(
    [ValidateSet('all', 'star', 'supervisor')][string]$Service = 'all',
    [ValidateSet('cpp')][string]$Implementation = 'cpp',
    [ValidateRange(1, 8)][int]$Jobs = 2,
    [switch]$Race,
    [ValidateRange(0, 300)][int]$FuzzSeconds = 0
)
if ($MyInvocation.InvocationName -eq '.') { throw 'Execute this script; do not dot-source it.' }
$ErrorActionPreference = 'Stop'
$astraRoot = (Resolve-Path -LiteralPath "$PSScriptRoot/..").ProviderPath
if ($Implementation -eq 'cpp' -and $Service -in @('all', 'star')) {
    throw 'C++26 service checks require Linux; the Rust service is retired.'
}

# 所有入口均使用绝对脚本路径, 调用终端的工作目录和环境保持不变.
function Invoke-Checked([scriptblock]$Command) {
    & $Command
    if ($LASTEXITCODE -ne 0) { throw "Service check failed with exit code $LASTEXITCODE." }
}

# 服务构建不再调用 protoc, 协议一致性由下面独立的只读生成检查负责.
function Invoke-GeneratorCheck {
    & "$astraRoot/sdk/run-tool.ps1" -Executable cargo -WorkingDirectory $astraRoot -Environment @{
        CARGO_HOME = "$astraRoot/build/deps/cargo"
        CARGO_TARGET_DIR = "$astraRoot/build/proto/target"
        CARGO_NET_OFFLINE = 'true'
    } -ToolArguments $args
}

Invoke-Checked { & "$astraRoot/scripts/generate-proto.ps1" -Check }
Invoke-Checked { Invoke-GeneratorCheck fmt --manifest-path ./proto/generator/Cargo.toml --all --check }
Invoke-Checked { Invoke-GeneratorCheck clippy --manifest-path ./proto/generator/Cargo.toml --frozen --all-targets --jobs $Jobs '--' '-D' warnings }
Invoke-Checked { Invoke-GeneratorCheck test --manifest-path ./proto/generator/Cargo.toml --frozen --jobs $Jobs }
if ($Service -in @('all', 'supervisor')) {
    $astraGo = Join-Path $astraRoot 'supervisor/go.ps1'
    $formatting = & gofmt -l "$astraRoot/supervisor/cmd" "$astraRoot/supervisor/internal"
    if ($LASTEXITCODE -ne 0 -or $formatting) { throw "Go formatting check failed: $formatting" }
    Invoke-Checked { & $astraGo mod tidy -diff }
    Invoke-Checked { & $astraGo mod verify }
    Invoke-Checked { & $astraGo vet -p $Jobs ./... }
    Invoke-Checked { & $astraGo test -p $Jobs -count=1 -shuffle=on -timeout=60s ./... }
    if ($FuzzSeconds -gt 0) {
        # Go 自带覆盖引导 fuzz, 每个入口单独运行, 语料缓存仍在项目 GOCACHE 中.
        Invoke-Checked { & $astraGo test -run='^$' -fuzz=FuzzAccountFileNeverAcceptsInvalidRoles "-fuzztime=${FuzzSeconds}s" -parallel=2 ./internal/admission }
        Invoke-Checked { & $astraGo test -run='^$' -fuzz=FuzzPersistedMemberDecodeRoundTrips "-fuzztime=${FuzzSeconds}s" -parallel=2 ./internal/membership }
    }
    if ($Race) {
        # cgo 只在此子进程启用. 缺少 C 编译器时明确失败, 不自动安装或静默跳过.
        Invoke-Checked {
            & "$astraRoot/sdk/run-tool.ps1" -Executable go -WorkingDirectory "$astraRoot/supervisor" -Environment @{
                GOMODCACHE = "$astraRoot/build/deps/go/pkg/mod"
                GOCACHE = "$astraRoot/build/cache/go"
                TEMP = "$astraRoot/build/tmp/supervisor"; TMP = "$astraRoot/build/tmp/supervisor"; GOTMPDIR = "$astraRoot/build/tmp/supervisor"; GOMAXPROCS = '2'
                GOTOOLCHAIN = 'local'; GOPROXY = 'off'; GOSUMDB = 'off'; GOWORK = 'off'; GOFLAGS = '-mod=readonly'; CGO_ENABLED = '1'
            } -ToolArguments @('test', '-race', '-p', "$Jobs", '-count=1', '-shuffle=on', '-timeout=180s', './...')
        }
    }
    Invoke-Checked { & $astraGo build -trimpath -p $Jobs -o ../build/supervisor/supervisor.exe ./cmd/supervisor }
    Invoke-Checked { & "$astraRoot/build/supervisor/supervisor.exe" --version }
}
Write-Output 'Service checks passed.'
