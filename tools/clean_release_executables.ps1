[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'Low')]
param([switch]$Apply)

# Generated developer binaries only. No recursive deletion, DLL or data removal.
$ErrorActionPreference = 'Stop'
$repositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$workspaceRoot = (Get-Item -LiteralPath $repositoryRoot).Parent.FullName
$expectedRelease = [IO.Path]::GetFullPath((Join-Path $workspaceRoot 'out/bin/Release'))
$releaseRoot = (Resolve-Path -LiteralPath $expectedRelease).Path
if (-not [string]::Equals($expectedRelease, $releaseRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Release directory resolved outside the expected workspace output.'
}
foreach ($directory in @('out', 'out/bin', 'out/bin/Release')) {
    if ((Get-Item -LiteralPath (Join-Path $workspaceRoot $directory)).Attributes -band
        [IO.FileAttributes]::ReparsePoint) {
        throw "Refusing cleanup through a directory link: $directory"
    }
}
$keep = @('SeisWaveLab_Curvilinear.exe', 'SeisWaveLab_MASW.exe', 'seis2d_experiment.exe',
          'seis2d_q_experiment.exe', 'seis2d_loess_pilot.exe', 'seis3d_curvilinear_demo.exe')
$cmake = Get-Content -LiteralPath (Join-Path $repositoryRoot 'cmake/Targets.cmake') -Raw
$known = @([regex]::Matches($cmake, 'add_executable\(\s*([A-Za-z][A-Za-z0-9_]*)\b') |
    ForEach-Object { $_.Groups[1].Value + '.exe' })
$known += @('compare_surface_vtk.exe', 'dem3d_capacity.exe', 'masw_forward_check.exe',
            'seis2d_sponge_audit.exe', 'seis3d_wave_audit_capture.exe')
$candidates = @()
$retained = @()
foreach ($file in Get-ChildItem -LiteralPath $releaseRoot -File -Filter '*.exe') {
    if ($keep -contains $file.Name -or $known -notcontains $file.Name) {
        $retained += $file.Name
        continue
    }
    $target = [IO.Path]::GetFullPath($file.FullName)
    if (-not [string]::Equals([IO.Path]::GetDirectoryName($target), $releaseRoot,
                             [StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetExtension($target) -ine '.exe') {
        throw "Refusing unexpected cleanup target: $target"
    }
    $candidates += [pscustomobject][ordered]@{ name = $file.Name; path = $target; bytes = $file.Length
        sha256 = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash; removed = $false }
}
$manifest = [ordered]@{
    date = [DateTime]::UtcNow.ToString('o'); root = $releaseRoot; apply = [bool]$Apply
    retained = @($retained | Sort-Object); candidates = $candidates
    runtime_build = 'cmake --build --preset x64-Release-runtime'
    developer_rebuild = 'cmake --build --preset x64-Release'
}
if ($Apply) {
    $reportFolder = Join-Path $workspaceRoot 'out/maintenance'
    New-Item -ItemType Directory -Path $reportFolder -Force | Out-Null
    $reportPath = Join-Path $reportFolder 'release-executable-cleanup.json'
    $manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $reportPath -Encoding utf8
    foreach ($candidate in $candidates) {
        if ($PSCmdlet.ShouldProcess($candidate.path, 'Delete regenerated developer executable')) {
            Remove-Item -LiteralPath $candidate.path -Force
            $candidate.removed = $true
            $manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $reportPath -Encoding utf8
        }
    }
}
[pscustomobject]@{
    Apply = [bool]$Apply
    Removed = @($candidates | Where-Object { $_.removed }).Count
    Candidates = @($candidates | ForEach-Object { $_.name })
    Retained = @($retained | Sort-Object)
    CandidateMiB = [math]::Round(($candidates | Measure-Object -Property bytes -Sum).Sum / 1MB, 2)
}
