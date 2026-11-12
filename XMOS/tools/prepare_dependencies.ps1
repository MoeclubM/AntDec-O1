param()

$ErrorActionPreference = 'Stop'
$taskXmos = Split-Path -Parent $PSScriptRoot
$taskModules = Join-Path $taskXmos 'modules'
$taskDependencies = Get-Content -LiteralPath (Join-Path $taskXmos 'dependencies.json') -Raw | ConvertFrom-Json

foreach ($taskDependency in $taskDependencies) {
    $taskModule = Join-Path $taskModules $taskDependency.name
    if (-not (Test-Path -LiteralPath $taskModule)) {
        & git -c core.autocrlf=false clone --depth 1 --branch "v$($taskDependency.version)" --single-branch $taskDependency.url $taskModule
        if ($LASTEXITCODE) { throw "Cannot fetch $($taskDependency.name)" }
    }

    $taskInfo = Join-Path $taskModule "$($taskDependency.name)/lib_build_info.cmake"
    $taskBuildInfo = Get-Content -LiteralPath $taskInfo -Raw
    $taskVersion = [regex]::Match($taskBuildInfo, '(?i)set\s*\(\s*LIB_VERSION\s+([\d.]+)').Groups[1].Value
    if ($taskVersion -ne $taskDependency.version) {
        throw "$($taskDependency.name) requires $($taskDependency.version); existing directory reports $taskVersion"
    }

    $taskPatch = Join-Path $taskXmos "patches/$($taskDependency.name).patch"
    if (-not (Test-Path -LiteralPath $taskPatch)) { continue }
    & git -C $taskModule apply --check --quiet $taskPatch 2>$null
    if ($LASTEXITCODE -eq 0) {
        & git -C $taskModule apply $taskPatch
        if ($LASTEXITCODE) { throw "Cannot apply patch for $($taskDependency.name)" }
    } else {
        & git -C $taskModule apply --reverse --check --quiet $taskPatch 2>$null
        if ($LASTEXITCODE) { throw "$($taskDependency.name) differs from the project patch; preserve local edits before updating" }
    }
}

Write-Output "XMOS dependencies ready ($($taskDependencies.Count) modules)."
