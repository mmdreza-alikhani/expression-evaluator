# Build and test all three implementations on Windows (MSVC + Go + CPython).
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root

Write-Host '== build C (MSVC) =='
Push-Location c
& cmd /c build_msvc.bat
$cBuild = $LASTEXITCODE
Pop-Location
if ($cBuild -ne 0) { throw 'C build failed' }

Write-Host ''
Write-Host '== build Go =='
Push-Location go
go build -o ..\bin\evaluator-go.exe .
$goBuild = $LASTEXITCODE
Pop-Location
if ($goBuild -ne 0) { throw 'Go build failed' }

Write-Host ''
Write-Host '== Python tests =='
python python\evaluator.py --test tests\cases.txt
$pyStatus = $LASTEXITCODE

Write-Host ''
Write-Host '== Go tests =='
.\bin\evaluator-go.exe -test -cases tests\cases.txt
$goStatus = $LASTEXITCODE

Write-Host ''
Write-Host '== C tests =='
.\bin\evaluator.exe --test tests\cases.txt
$cStatus = $LASTEXITCODE

Write-Host ''
Write-Host '== summary =='
$results = @{ python = $pyStatus; go = $goStatus; c = $cStatus }
foreach ($key in 'python', 'go', 'c') {
    $ok = ($results[$key] -eq 0)
    Write-Host ("{0,-7} {1}" -f $key, $(if ($ok) { 'PASS' } else { 'FAIL' }))
}

if ($pyStatus -ne 0 -or $goStatus -ne 0 -or $cStatus -ne 0) { exit 1 }
exit 0
