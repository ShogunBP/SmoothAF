$ErrorActionPreference = "Stop"

$project = "D:\Projetos\Community\SmoothAF"
$expectedCommit = "9fd1ab556da3c3fc8c174cfb15919b423d69163b"

Set-Location $project

# 1. Configurar dependências
$env:SMOOTHAF_LEGACY_ROOT = Join-Path $project "docs\references\magic-lantern-branch-lua_fix"
$env:SMOOTHAF_SYMBOL_SOURCE = Join-Path $env:SMOOTHAF_LEGACY_ROOT "600D_102.SYM"

foreach ($path in @($env:SMOOTHAF_LEGACY_ROOT, $env:SMOOTHAF_SYMBOL_SOURCE)) {
    if (-not (Test-Path $path)) {
        throw "Dependência não encontrada: $path"
    }
}

Write-Host "`n[OK] Dependências encontradas" -ForegroundColor Green

# 2. Confirmar branch e commit
$head = (git rev-parse HEAD).Trim()
$branch = (git branch --show-current).Trim()

Write-Host "Branch: $branch"
Write-Host "Commit: $head"

if ($head -ne $expectedCommit -or $branch -ne "feature/af-algorithm") {
    throw "Branch ou commit diferente do esperado. Build interrompido."
}

# 3. Confirmar Docker
docker info *> $null
if ($LASTEXITCODE -ne 0) {
    throw "Docker Linux Engine indisponível."
}

Write-Host "[OK] Docker disponível" -ForegroundColor Green

# 4. Testes
Write-Host "`n[TESTES]" -ForegroundColor Cyan
& ".\build-smoothaf.ps1" -TestOnly

if (-not $?) {
    throw "Falha na execução dos testes."
}

# 5. Backup do artefato anterior
$artifact = Join-Path $project "artifacts\v0.8\smoothaf.mo"

if (Test-Path $artifact) {
    $backup = "$artifact.pre-ram-restore-$(Get-Date -Format 'yyyyMMdd-HHmmss')"
    Copy-Item $artifact $backup -ErrorAction Stop
    Write-Host "Backup: $backup"
}

# 6. Build ARM
Write-Host "`n[BUILD ARM 600D.102]" -ForegroundColor Cyan
& ".\build-smoothaf.ps1"

if (-not $?) {
    throw "O script de build apresentou erro."
}

if (-not (Test-Path $artifact)) {
    throw "O arquivo smoothaf.mo não foi gerado."
}

# 7. Resultado
$file = Get-Item $artifact
$hash = Get-FileHash $artifact -Algorithm SHA256

Write-Host "`n========== RESULTADO ==========" -ForegroundColor Green
Write-Host "Arquivo: $($file.FullName)"
Write-Host "Tamanho: $($file.Length) bytes"
Write-Host "SHA256: $($hash.Hash)"
Write-Host "================================"