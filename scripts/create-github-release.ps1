param(
    [string]$Tag = "v0.1.0",
    [string]$Repo = "pfcdev/TaskWidgets",
    [switch]$Draft,
    [switch]$Prerelease
)

$ErrorActionPreference = "Stop"

function Get-GitHubHeaders {
    $CredentialInput = "protocol=https`nhost=github.com`n`n"
    $CredentialOutput = $CredentialInput | git credential fill
    $Map = @{}
    foreach ($Line in $CredentialOutput) {
        $Index = $Line.IndexOf("=")
        if ($Index -gt 0) {
            $Map[$Line.Substring(0, $Index)] = $Line.Substring($Index + 1)
        }
    }

    if (-not $Map.ContainsKey("username") -or -not $Map.ContainsKey("password")) {
        throw "GitHub credentials were not available from git credential manager."
    }

    $Token = [Convert]::ToBase64String(
        [Text.Encoding]::ASCII.GetBytes("$($Map["username"]):$($Map["password"])"))
    return @{
        Authorization = "Basic $Token"
        Accept = "application/vnd.github+json"
        "User-Agent" = "TaskbarStatsReleaseScript"
        "X-GitHub-Api-Version" = "2022-11-28"
    }
}

function Publish-WithGitHubRest {
    param(
        [string]$Repo,
        [string]$Tag,
        [string[]]$Files,
        [switch]$Draft,
        [switch]$Prerelease
    )

    $Parts = $Repo.Split("/", 2)
    if ($Parts.Count -ne 2) {
        throw "Repo must be in owner/name form."
    }

    $Owner = $Parts[0]
    $Name = $Parts[1]
    $Headers = Get-GitHubHeaders

    $Release = $null
    try {
        $Release = Invoke-RestMethod `
            -Method Get `
            -Uri "https://api.github.com/repos/$Owner/$Name/releases/tags/$Tag" `
            -Headers $Headers
    } catch {
        if ($_.Exception.Response.StatusCode.value__ -ne 404) {
            throw
        }
    }

    if ($null -eq $Release) {
        $Body = @{
            tag_name = $Tag
            name = "TaskbarStats $Tag"
            body = "TaskbarStats product build."
            draft = [bool]$Draft
            prerelease = [bool]$Prerelease
        } | ConvertTo-Json

        $Release = Invoke-RestMethod `
            -Method Post `
            -Uri "https://api.github.com/repos/$Owner/$Name/releases" `
            -Headers $Headers `
            -ContentType "application/json" `
            -Body $Body
    }

    foreach ($File in $Files) {
        $FileName = Split-Path $File -Leaf
        foreach ($Asset in @($Release.assets)) {
            if ($Asset.name -eq $FileName) {
                Invoke-RestMethod `
                    -Method Delete `
                    -Uri "https://api.github.com/repos/$Owner/$Name/releases/assets/$($Asset.id)" `
                    -Headers $Headers | Out-Null
            }
        }

        $UploadBase = [string]($Release.upload_url -replace "\{\?name,label\}", "")
        $EscapedName = [System.Uri]::EscapeDataString($FileName)
        $UploadUri = "{0}?name={1}" -f $UploadBase, $EscapedName
        Invoke-RestMethod `
            -Method Post `
            -Uri $UploadUri `
            -Headers $Headers `
            -ContentType "application/octet-stream" `
            -InFile $File | Out-Null
    }
}

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$BuildScript = Join-Path $RepoRoot "scripts\build-product.ps1"
$InstallerBuildScript = Join-Path $RepoRoot "scripts\build-installer.ps1"
$MsiBuildScript = Join-Path $RepoRoot "scripts\build-msi.ps1"
$ArtifactDir = Join-Path $RepoRoot "artifacts\TaskbarStats"
$Exe = Join-Path $ArtifactDir "TaskbarStats.exe"
$Sha = Join-Path $ArtifactDir "TaskbarStats.exe.sha256"
$InstallerExe = Join-Path $RepoRoot "artifacts\TaskbarStatsSetup.exe"
$InstallerSha = Join-Path $RepoRoot "artifacts\TaskbarStatsSetup.exe.sha256"
$Msi = Join-Path $RepoRoot "artifacts\TaskbarStats.msi"
$MsiSha = Join-Path $RepoRoot "artifacts\TaskbarStats.msi.sha256"

$ReleaseVersion = $Tag.TrimStart("v", "V")
if ($ReleaseVersion -notmatch '^\d+\.\d+\.\d+(\.\d+)?$') {
    throw "Tag must be a numeric version like v0.1.0 or v0.1.0.1."
}

powershell -ExecutionPolicy Bypass -File $BuildScript -Configuration Release -Version $ReleaseVersion
powershell -ExecutionPolicy Bypass -File $InstallerBuildScript -Configuration Release -Version $ReleaseVersion -SkipProductBuild
powershell -ExecutionPolicy Bypass -File $MsiBuildScript -Configuration Release -Version $ReleaseVersion -SkipProductBuild

if (-not (Test-Path $Exe)) {
    throw "Expected artifact not found: $Exe"
}
if (-not (Test-Path $InstallerExe)) {
    throw "Expected installer artifact not found: $InstallerExe"
}
if (-not (Test-Path $Msi)) {
    throw "Expected MSI artifact not found: $Msi"
}

$Hash = (Get-FileHash $Exe -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content -Path $Sha -Value "$Hash  TaskbarStats.exe" -Encoding ASCII
$InstallerHash = (Get-FileHash $InstallerExe -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content -Path $InstallerSha -Value "$InstallerHash  TaskbarStatsSetup.exe" -Encoding ASCII
$MsiHash = (Get-FileHash $Msi -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content -Path $MsiSha -Value "$MsiHash  TaskbarStats.msi" -Encoding ASCII

$ReleaseFiles = @($Exe, $Sha, $Msi, $MsiSha, $InstallerExe, $InstallerSha)

if (Get-Command gh -ErrorAction SilentlyContinue) {
    $ReleaseExists = $false
    try {
        gh release view $Tag --repo $Repo *> $null
        $ReleaseExists = $true
    } catch {
        $ReleaseExists = $false
    }

    if ($ReleaseExists) {
        gh release upload $Tag @ReleaseFiles --repo $Repo --clobber
    } else {
        $Args = @(
            "release", "create", $Tag,
            $Exe, $Sha, $Msi, $MsiSha, $InstallerExe, $InstallerSha,
            "--repo", $Repo,
            "--title", "TaskbarStats $Tag",
            "--notes", "TaskbarStats product build. Use TaskbarStatsSetup.exe for normal Windows installation/update."
        )
        if ($Draft) {
            $Args += "--draft"
        }
        if ($Prerelease) {
            $Args += "--prerelease"
        }

        gh @Args
    }
} else {
    Write-Warning "GitHub CLI was not found. Falling back to GitHub REST API with git credential manager."
    Publish-WithGitHubRest -Repo $Repo -Tag $Tag -Files $ReleaseFiles -Draft:$Draft -Prerelease:$Prerelease
}

Write-Host "Release asset uploaded: $Repo $Tag"
