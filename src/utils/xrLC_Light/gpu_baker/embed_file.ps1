# Embeds a binary file (nvcc PTX output) as a C++ byte array header.
# The array must be char-sized: optixModuleCreate consumes (const char*, byte count).
# Usage: powershell -NoProfile -File embed_file.ps1 -In file.ptx -Out header.h -Name ArrayName
param(
	[Parameter(Mandatory = $true)][string]$In,
	[Parameter(Mandatory = $true)][string]$Out,
	[Parameter(Mandatory = $true)][string]$Name
)

$ErrorActionPreference = 'Stop'
$bytes = [IO.File]::ReadAllBytes($In)
$content = "static const char $Name[$($bytes.Length)] = { " + ([string]::Join(',', [int[]]$bytes)) + " };`r`nstatic const unsigned int $Name`_size = $($bytes.Length);`r`n"
[IO.File]::WriteAllText($Out, $content, [System.Text.Encoding]::ASCII)
Write-Host "embed_file: $In ($($bytes.Length) bytes) -> $Out"
