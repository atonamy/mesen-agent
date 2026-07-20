$ErrorActionPreference = "Stop"

$repository = if ($env:MESEN_AGENT_REPOSITORY) { $env:MESEN_AGENT_REPOSITORY } else { "atonamy/mesen-agent" }
$installDir = if ($env:MESEN_AGENT_INSTALL_DIR) { $env:MESEN_AGENT_INSTALL_DIR } else { Join-Path $env:USERPROFILE ".local\bin" }
$version = $env:MESEN_AGENT_VERSION

if (-not [Environment]::Is64BitOperatingSystem) {
    throw "mesen-agent supports Windows x64 only."
}

if (-not $version) {
    $release = Invoke-RestMethod -Headers @{ "User-Agent" = "mesen-agent-installer" } -Uri "https://api.github.com/repos/$repository/releases/latest"
    $version = $release.tag_name
}
if (-not $version.StartsWith("v")) { $version = "v$version" }

$asset = "mesen-agent-windows-x64.zip"
$releaseUrl = "https://github.com/$repository/releases/download/$version"
$workDir = Join-Path ([System.IO.Path]::GetTempPath()) ("mesen-agent-install-" + [Guid]::NewGuid().ToString("N"))

try {
    New-Item -ItemType Directory -Path $workDir | Out-Null
    $archive = Join-Path $workDir $asset
    $checksums = Join-Path $workDir "SHA256SUMS"
    Invoke-WebRequest -Uri "$releaseUrl/$asset" -OutFile $archive
    Invoke-WebRequest -Uri "$releaseUrl/SHA256SUMS" -OutFile $checksums

    $checksumLine = Get-Content $checksums | Where-Object { $_ -match "\s\*?$([regex]::Escape($asset))$" } | Select-Object -First 1
    if (-not $checksumLine) { throw "Checksum for $asset is missing." }
    $expected = ($checksumLine -split "\s+")[0].ToLowerInvariant()
    $actual = (Get-FileHash -Algorithm SHA256 $archive).Hash.ToLowerInvariant()
    if ($actual -ne $expected) { throw "Checksum verification failed for $asset." }

    $unpacked = Join-Path $workDir "unpacked"
    Expand-Archive -Path $archive -DestinationPath $unpacked
    $cli = Join-Path $unpacked "mesen-agent.exe"
    $core = Join-Path $unpacked "MesenCore.dll"
    if (-not (Test-Path $cli) -or -not (Test-Path $core)) {
        throw "Release archive does not contain mesen-agent.exe and MesenCore.dll."
    }

    New-Item -ItemType Directory -Force -Path $installDir | Out-Null
    Copy-Item -Force $cli (Join-Path $installDir "mesen-agent.exe")
    Copy-Item -Force $core (Join-Path $installDir "MesenCore.dll")
    & (Join-Path $installDir "mesen-agent.exe") --help | Out-Null
    Write-Host "Installed mesen-agent $version to $(Join-Path $installDir 'mesen-agent.exe')"
}
finally {
    if (Test-Path $workDir) { Remove-Item -Recurse -Force $workDir }
}
