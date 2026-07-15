param(
    [Parameter(Mandatory = $true)]
    [string[]]$Paths
)

$ErrorActionPreference = "Stop"
if (-not $env:WINDOWS_SIGNING_CERT_BASE64 -or -not $env:WINDOWS_SIGNING_CERT_PASSWORD) {
    Write-Warning "Signing secrets are not configured; artifacts will be unsigned and may trigger SmartScreen."
    exit 0
}

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$PfxPath = Join-Path $RepoRoot "artifacts\signing-certificate.pfx"
$bytes = [Convert]::FromBase64String($env:WINDOWS_SIGNING_CERT_BASE64)
[IO.File]::WriteAllBytes($PfxPath, $bytes)

try {
    $signtool = Get-Command signtool.exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source
    if (-not $signtool) {
        $signtool = Get-ChildItem "C:\Program Files (x86)\Windows Kits\10\bin" -Filter signtool.exe -Recurse -ErrorAction SilentlyContinue |
            Where-Object FullName -Match '\\x64\\signtool\.exe$' |
            Sort-Object FullName -Descending |
            Select-Object -First 1 -ExpandProperty FullName
    }
    if (-not $signtool) { throw "signtool.exe was not found." }

    foreach ($path in $Paths) {
        if (-not (Test-Path -LiteralPath $path)) { throw "Signing input missing: $path" }
        & $signtool sign /fd SHA256 /td SHA256 /tr "http://timestamp.digicert.com" /f $PfxPath /p $env:WINDOWS_SIGNING_CERT_PASSWORD $path
        if ($LASTEXITCODE -ne 0) { throw "Signing failed: $path" }
    }
} finally {
    Remove-Item -LiteralPath $PfxPath -Force -ErrorAction SilentlyContinue
}
