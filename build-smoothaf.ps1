[CmdletBinding()]
param(
    [switch]$TestOnly,
    [switch]$Clean,
    [switch]$Release,
    [string]$LegacyRoot = $env:SMOOTHAF_LEGACY_ROOT,
    [string]$SymbolSource = $env:SMOOTHAF_SYMBOL_SOURCE
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($TestOnly -and ($Clean -or $Release)) { throw 'Incompatible options: -TestOnly cannot be combined with -Clean or -Release.' }
if ($Clean -and $Release) { throw 'Incompatible options: -Clean cannot be combined with -Release.' }

$ProjectRoot = (Resolve-Path -LiteralPath $PSScriptRoot).Path
$Dockerfile = Join-Path $ProjectRoot 'docker\Dockerfile'
$Image = 'smoothaf-legacy-build:bookworm'
$OutputRoot = Join-Path $ProjectRoot 'artifacts'
$ModulePath = Join-Path $OutputRoot 'v0.8\smoothaf.mo'
$ShortCommit = (& git -c core.autocrlf=true -C $ProjectRoot rev-parse --short=12 HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($ShortCommit)) { throw 'Unable to determine source commit for Build ID.' }
$Dirty = (& git -c core.autocrlf=true -C $ProjectRoot status --porcelain)
$BuildSuffix = if ($Dirty) { '-dirty' } else { '' }

if ([string]::IsNullOrWhiteSpace($LegacyRoot)) {
    $legacyCandidates = @(
        (Join-Path $ProjectRoot 'docs\references\magic-lantern-branch-lua_fix'),
        (Join-Path $ProjectRoot 'docs\references\magic-lantern')
    )
    foreach ($candidate in $legacyCandidates) {
        if (Test-Path -LiteralPath $candidate -PathType Container) {
            $LegacyRoot = $candidate
            break
        }
    }
}

if ([string]::IsNullOrWhiteSpace($SymbolSource)) {
    $symbolCandidates = @()
    if (-not [string]::IsNullOrWhiteSpace($LegacyRoot)) {
        $symbolCandidates += Join-Path $LegacyRoot '600D_102.SYM'
    }
    $symbolCandidates += Join-Path $ProjectRoot 'docs\references\600D_102.SYM'
    foreach ($candidate in $symbolCandidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            $SymbolSource = $candidate
            break
        }
    }
}

function Require-Command([string]$Name) {
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) { throw "Command not found: $Name" }
}

function Run-Docker([string[]]$Arguments) {
    & docker @Arguments
    if ($LASTEXITCODE -ne 0) { throw "Docker failed with exit code $LASTEXITCODE." }
}

Require-Command 'docker'
& docker info --format '{{.ServerVersion}}' *> $null
if ($LASTEXITCODE -ne 0) { throw 'Docker Desktop is not running.' }

Run-Docker @('build', '-t', $Image, '-f', $Dockerfile, $ProjectRoot)

if ($TestOnly) {
    $mount = "type=bind,source=$ProjectRoot,target=/src"
    Run-Docker @('run', '--rm', '--mount', $mount, '--workdir', '/tmp', $Image, 'sh', '-c', 'gcc -std=c99 -Wall -Wextra -O2 -o /tmp/test_smoothaf_runtime /src/tests/test_smoothaf_runtime.c && /tmp/test_smoothaf_runtime && gcc -std=c99 -Wall -Wextra -O2 -o /tmp/test_smoothaf_d1 /src/tests/test_smoothaf_d1.c && /tmp/test_smoothaf_d1 && gcc -std=c99 -Wall -Wextra -O2 -o /tmp/test_smoothaf_production_seams /src/tests/test_smoothaf_production_seams.c && /tmp/test_smoothaf_production_seams && gcc -std=c99 -Wall -Wextra -O2 -o /tmp/test_smoothaf_core /src/tests/test_smoothaf_core.c && /tmp/test_smoothaf_core && gcc -std=c99 -Wall -Wextra -O2 -o /tmp/test_smoothaf_algorithm /src/tests/test_smoothaf_algorithm.c && /tmp/test_smoothaf_algorithm && gcc -std=c99 -Wall -Wextra -O2 -o /tmp/test_smoothaf_monitor /src/tests/test_smoothaf_monitor.c && /tmp/test_smoothaf_monitor && gcc -std=c99 -Wall -Wextra -O2 -o /tmp/test_smoothaf_episode /src/tests/test_smoothaf_episode.c && /tmp/test_smoothaf_episode && gcc -std=c99 -Wall -Wextra -O2 -o /tmp/test_smoothaf_diagnostics /src/tests/test_smoothaf_diagnostics.c && /tmp/test_smoothaf_diagnostics && gcc -std=c99 -Wall -Wextra -Werror -O2 -I/src/src -o /tmp/test_smoothaf_observability /src/tests/test_smoothaf_observability.c /src/src/smoothaf_observability.c && /tmp/test_smoothaf_observability && gcc -std=c99 -Wall -Wextra -Werror -O2 -I/src/src -o /tmp/test_smoothaf_observability_wire /src/tests/test_smoothaf_observability_wire.c /src/src/smoothaf_observability.c && python3 -B /src/tests/check_smoothaf_observability_wire.py /tmp/test_smoothaf_observability_wire && gcc -std=c99 -Wall -Wextra -Werror -O2 -I/src/src -o /tmp/test_smoothaf_observability_identity /src/tests/test_smoothaf_observability_identity.c /src/src/smoothaf_observability.c && /tmp/test_smoothaf_observability_identity && gcc -std=c99 -Wall -Wextra -O2 -pthread -DSMOOTHAF_HOST_TEST -I/src/src -o /tmp/test_smoothaf_logger /src/tests/test_smoothaf_logger.c /src/src/smoothaf_logger.c /src/src/smoothaf_logger_fio.c && /tmp/test_smoothaf_logger && gcc -std=c99 -Wall -Wextra -Werror -O2 -DSMOOTHAF_HOST_TEST -I/src/src -o /tmp/test_smoothaf_logger_tsv /src/tests/test_smoothaf_logger_tsv.c /src/src/smoothaf_logger.c /src/src/smoothaf_logger_fio.c && /tmp/test_smoothaf_logger_tsv > /tmp/smoothaf_c_tsv.log && python3 -B /src/tests/check_smoothaf_logger_tsv.py /tmp/smoothaf_c_tsv.log && gcc -std=c99 -Wall -Wextra -O2 -I/src/src -o /tmp/test_smoothaf_fio_layout /src/tests/test_smoothaf_fio_layout.c && /tmp/test_smoothaf_fio_layout && gcc -std=c99 -Wall -Wextra -O2 -o /tmp/benchmark_smoothaf /src/tests/benchmark_smoothaf.c && /tmp/benchmark_smoothaf && python3 -B /src/tests/test_smoothaf_menu_regression.py && python3 -B /src/tests/test_smoothaf_loader_contract.py && python3 -B /src/tests/test_smoothaf_log_fixtures.py && python3 -B /src/tests/test_smoothaf_b1.py && python3 -B /src/tests/test_smoothaf_b2a.py --out /src/tests/fixtures/SMTH001.b2a.out && python3 -B /src/tests/test_smoothaf_d1.py')
    exit 0
}

if ([string]::IsNullOrWhiteSpace($LegacyRoot)) { throw 'Set -LegacyRoot to a compatible Magic Lantern Legacy Lua Fix tree.' }
if ([string]::IsNullOrWhiteSpace($SymbolSource)) { throw 'Set -SymbolSource to the private 600D_102.SYM file.' }
if (-not (Test-Path -LiteralPath $LegacyRoot -PathType Container)) { throw "Legacy tree not found: $LegacyRoot" }
if (-not (Test-Path -LiteralPath $SymbolSource -PathType Leaf)) { throw "Symbol table not found: $SymbolSource" }

if ($Clean -and (Test-Path -LiteralPath $OutputRoot)) {
    Get-ChildItem -LiteralPath $OutputRoot -Filter '*.tmp' -File -ErrorAction SilentlyContinue | Remove-Item -Force
    if (Test-Path -LiteralPath $ModulePath) { Remove-Item -LiteralPath $ModulePath -Force }
}

$stage = Join-Path ([IO.Path]::GetTempPath()) ('smoothaf-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage | Out-Null
try {
    $legacyStage = Join-Path $stage 'legacy'
    Copy-Item -LiteralPath $LegacyRoot -Destination $legacyStage -Recurse -Force

    $integration = Join-Path $legacyStage 'modules\smoothaf'
    New-Item -ItemType Directory -Path $integration -Force | Out-Null
    Copy-Item (Join-Path $ProjectRoot 'src\smoothaf.c') $integration -Force
    Copy-Item (Join-Path $ProjectRoot 'src\smoothaf_core.h') $integration -Force
    Copy-Item (Join-Path $ProjectRoot 'src\smoothaf_algorithm.h') $integration -Force
    Copy-Item (Join-Path $ProjectRoot 'src\smoothaf_direction.h') $integration -Force
    Copy-Item (Join-Path $ProjectRoot 'src\smoothaf_recovery.h') $integration -Force
    Copy-Item (Join-Path $ProjectRoot 'src\smoothaf_build_id.h') $integration -Force
    Copy-Item (Join-Path $ProjectRoot 'src\smoothaf_logger.h') $integration -Force
    Copy-Item (Join-Path $ProjectRoot 'src\smoothaf_logger.c') $integration -Force
    Copy-Item (Join-Path $ProjectRoot 'src\smoothaf_observability.h') $integration -Force
    Copy-Item (Join-Path $ProjectRoot 'src\smoothaf_observability.c') $integration -Force
    Copy-Item (Join-Path $ProjectRoot 'src\smoothaf_logger_fio.h') $integration -Force
    Copy-Item (Join-Path $ProjectRoot 'src\smoothaf_logger_fio.c') $integration -Force
    Copy-Item (Join-Path $ProjectRoot 'src\smoothaf_menu_toggle.h') $integration -Force
    Copy-Item (Join-Path $ProjectRoot 'src\smoothaf_task_lifecycle.h') $integration -Force
     Copy-Item (Join-Path $ProjectRoot 'src\smoothaf_d1.h') $integration -Force
    $commit = $ShortCommit
    $buildId = "${commit}${BuildSuffix}-600D.102"
    Set-Content -LiteralPath (Join-Path $integration 'smoothaf_build_id.h') -Value @("#ifndef SMOOTHAF_BUILD_ID_H", "#define SMOOTHAF_BUILD_ID_H", "#define SMOOTHAF_BUILD_ID `"$buildId`"", "#define SMOOTHAF_BUILD_CONFIG `"600D.102`"", "static inline const char *smoothaf_build_id_value(void)", "{", "    return SMOOTHAF_BUILD_ID;", "}", "#endif") -Encoding ascii
    Copy-Item (Join-Path $ProjectRoot 'src\smoothaf_diagnostics.h') $integration -Force
    Copy-Item (Join-Path $ProjectRoot 'src\smoothaf_monitor.h') $integration -Force
    Copy-Item (Join-Path $ProjectRoot 'src\smoothaf_episode.h') $integration -Force
    Copy-Item (Join-Path $ProjectRoot 'integration\Makefile') $integration -Force
    Copy-Item (Join-Path $ProjectRoot 'integration\README.rst') $integration -Force
    Copy-Item (Join-Path $ProjectRoot 'docker\generate-strings.py') $integration -Force
    Copy-Item (Join-Path $ProjectRoot 'docker\checkdep_py3.py') (Join-Path $legacyStage 'modules') -Force

    $symbolTarget = Join-Path $legacyStage 'platform\600D.102\magiclantern.sym'
    $symbolDir = Split-Path -Parent $symbolTarget
    if (-not (Test-Path -LiteralPath $symbolDir -PathType Container)) { throw "Legacy symbol directory not found: $symbolDir" }
    Copy-Item -LiteralPath $SymbolSource -Destination $symbolTarget -Force

    $mount = "type=bind,source=$stage,target=/src"
    Run-Docker @('run', '--rm', '--mount', $mount, '--workdir', '/src/legacy/modules/smoothaf', $Image, 'sh', '-c', 'make clean all PYTHON=python3')

    $built = Join-Path $integration 'smoothaf.mo'
    if (-not (Test-Path -LiteralPath $built -PathType Leaf)) { throw 'Legacy build completed without producing smoothaf.mo.' }
    New-Item -ItemType Directory -Path (Split-Path -Parent $ModulePath) -Force | Out-Null
    Copy-Item -LiteralPath $built -Destination $ModulePath -Force

    $bytes = [IO.File]::ReadAllBytes($ModulePath)
    $sha = ([BitConverter]::ToString(([Security.Cryptography.SHA256]::Create().ComputeHash($bytes))).Replace('-', '').ToLowerInvariant())
    $size = $bytes.Length
    Write-Output "SIZE=$size"
    Write-Output "SHA256=$sha"
    Write-Output "BUILD_ID=$buildId"
    $elf = Join-Path $integration 'smoothaf.elf'
    if (Test-Path -LiteralPath $elf -PathType Leaf) {
        $fileInfo = & docker @('run', '--rm', '--mount', $mount, $Image, 'sh', '-c', 'file /src/legacy/modules/smoothaf/smoothaf.elf')
        Write-Output "ELF=$fileInfo"
        $undefined = & docker @('run', '--rm', '--mount', $mount, $Image, 'sh', '-c', 'arm-none-eabi-nm -u /src/legacy/modules/smoothaf/smoothaf.elf')
        Write-Output 'UNDEFINED_SYMBOLS_BEGIN'
        $undefined | Write-Output
        Write-Output 'UNDEFINED_SYMBOLS_END'
    } else {
        Write-Output 'ELF=NOT_EMITTED_BY_LEGACY_BUILD'
        Write-Output 'UNDEFINED_SYMBOLS=NOT_AVAILABLE'
    }
    $strings = & docker @('run', '--rm', '--mount', $mount, $Image, 'sh', '-c', "strings -a /src/legacy/modules/smoothaf/smoothaf.mo | grep -F '$buildId'")
    if ($LASTEXITCODE -ne 0 -or -not ($strings -match [regex]::Escape($buildId))) { throw 'Final module does not contain the expected Build ID.' }
    $depPath = Join-Path $integration 'smoothaf.dep'
    if (-not (Test-Path -LiteralPath $depPath -PathType Leaf)) {
        throw "ARM build completed without producing smoothaf.dep."
    }
    $depCount = @(
        Get-Content -LiteralPath $depPath |
            Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
    ).Count
    Write-Output "DEP=$depCount"

    if ($Release) {
        $releaseDir = Join-Path $OutputRoot ("releases\smoothaf-$sha")
        New-Item -ItemType Directory -Path $releaseDir -Force | Out-Null
        Copy-Item -LiteralPath $ModulePath -Destination (Join-Path $releaseDir 'smoothaf.mo') -Force
        Set-Content -LiteralPath (Join-Path $releaseDir 'SHA256.txt') -Value "$sha  smoothaf.mo" -Encoding ascii
        Write-Output "RELEASE=$releaseDir"
    }
}
finally {
    if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
}
