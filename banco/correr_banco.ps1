# correr_banco.ps1 - Compila y corre el banco de pruebas del firmware en Windows.
# Usa gcc si esta en el PATH; si no, busca Visual Studio (cl.exe).
#
#   powershell -ExecutionPolicy Bypass -File banco\correr_banco.ps1          (todo)
#   powershell -ExecutionPolicy Bypass -File banco\correr_banco.ps1 -Rapido  (sin las corridas largas)
#   ... -Verbose  guarda tambien cada renglon de telemetria en los .log
param([switch]$Rapido, [switch]$Verbose)

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8
$banco = $PSScriptRoot
$sal   = Join-Path $banco 'salida'
New-Item -ItemType Directory -Force $sal | Out-Null

# ---- Compilador ----
$gcc = Get-Command gcc -ErrorAction SilentlyContinue
$vcvars = $null
if (-not $gcc) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($vs) { $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat' }
    }
    if (-not $vcvars -or -not (Test-Path $vcvars)) { throw 'No hay gcc ni Visual Studio (cl.exe).' }
}

function Compilar([string]$nombre, [string[]]$defs, [switch]$Estricto) {
    $exe = Join-Path $sal "$nombre.exe"
    $src = Join-Path $banco 'banco.c'
    if ($gcc) {
        $d = ($defs | ForEach-Object { "-D$_" }) -join ' '
        $w = if ($Estricto) { '-Wall -Wextra' } else { '-w' }
        $out = cmd /c "gcc -std=c99 $w -Wno-unknown-pragmas -DBANCO_PC $d -I`"$banco`" `"$src`" -lm -o `"$exe`" 2>&1"
    } else {
        $d = ($defs | ForEach-Object { "/D$_" }) -join ' '
        $w = if ($Estricto) { '/W4' } else { '/W1' }
        $cl = "cl /nologo /utf-8 $w /wd4068 /wd4996 /D_CRT_SECURE_NO_WARNINGS /DBANCO_PC $d /I`"$banco`" `"$src`" /Fe`"$exe`" /Fo`"$sal\\`""
        $out = cmd /c "`"$vcvars`" >nul 2>nul && $cl 2>&1"
    }
    $codigo = $LASTEXITCODE
    $out | Where-Object { $_ -and $_ -notmatch '^banco\.c$' } | ForEach-Object { Write-Host "  [$nombre] $_" }
    if ($codigo -ne 0) { throw "Fallo la compilacion de $nombre" }
    Write-Host "  $nombre  ($($defs -join ' '))"
    return $exe
}

Write-Host '=== Compilando para la PC ==='
$real    = Compilar 'banco_real'     @('_16F1847') -Estricto                  # todo
$r1827   = Compilar 'banco_r1827'    @('_16F1827') -Estricto                  # sin extras
$ventana = Compilar 'banco_ventana'  @('_16F1827', 'MODO_SALIDA=1') -Estricto
$sim     = Compilar 'banco_sim'      @('_16F1827', 'SIMULAR_SENSORES=1') -Estricto
$simf    = Compilar 'banco_simfalla' @('_16F1827', 'SIMULAR_SENSORES=1', 'SIM_FALLA_SEG=600', 'SIM_FALLA_MODO=0')
$prueba  = Compilar 'banco_prueba'   @('_16F1827', 'MODO_PRUEBA=1')

# ---- Compilacion real con XC8 (si esta instalado): revisa que quepa en cada chip ----
$xc8 = Get-ChildItem 'C:\Program Files\Microchip\xc8\*\bin\xc8-cc.exe' -ErrorAction SilentlyContinue | Sort-Object FullName | Select-Object -Last 1
$dfp = Get-ChildItem 'C:\Program Files\Microchip\MPLABX\*\packs\Microchip\PIC12-16F1xxx_DFP\*\xc8' -Directory -ErrorAction SilentlyContinue | Sort-Object FullName | Select-Object -Last 1
$memoria = @()
if ($xc8 -and $dfp) {
    Write-Host "=== Compilando con $($xc8.FullName) ==="
    $fw = Join-Path (Split-Path $banco -Parent) 'horno_pwm_16f18x7.c'
    foreach ($obj in @(@('16F1827', @()), @('16F1827', @('-DSIMULAR_SENSORES=1')), @('16F1847', @()), @('16F1847', @('-DSIMULAR_SENSORES=1')))) {
        $nom = "xc8_$($obj[0])" + $(if ($obj[1].Count) { '_sim' } else { '' })
        $dir = Join-Path $sal $nom
        New-Item -ItemType Directory -Force $dir | Out-Null
        Push-Location $dir
        $r = & $xc8.FullName "-mcpu=$($obj[0])" "-mdfp=$($dfp.FullName)" '-Os' @($obj[1]) '-o' 'horno.elf' $fw 2>&1 | Out-String
        $cod = $LASTEXITCODE
        Pop-Location
        $uso = ($r -split "`n" | Where-Object { $_ -match 'Program space' }) -replace '^\s+', ''
        Write-Host "  $nom : $(if ($cod -eq 0) { $uso } else { 'NO CABE o error (ver ' + $nom + '.log)' })"
        $r | Out-File -Encoding utf8 (Join-Path $sal "$nom.log")
        $memoria += [pscustomobject]@{ Prueba = $nom; Resultado = $(if ($cod -eq 0) { 'BIEN' } else { 'MAL' }) }
    }
} else {
    Write-Host '(XC8 no encontrado: se omite la compilacion para el PIC)'
}

$comunes = 'perfil', 'comandos', 'persistencia', 'sobretemp', 'sin_sensores', 'embalamiento', 'corto',
           'atipico', 'congelado1', 'congelado5', 'wdt', 'bor', 'migracion', 'corrupto'
$corridas = @()
foreach ($e in $comunes) { $corridas += , @($real, $e) }
foreach ($e in 'autoajuste', 'ia', 'mclr', 'pila', 'vdd') { $corridas += , @($real, $e) }
foreach ($e in $comunes) { $corridas += , @($r1827, $e) }
$corridas += @(@($ventana, 'perfil'), @($sim, 'perfil'), @($simf, 'simfalla'), @($prueba, 'prueba'))
if ($Rapido) { $corridas = $corridas | Where-Object { ($_[1] -ne 'perfil' -and $_[1] -ne 'autoajuste') -or $_[0] -eq $real } }

$resumen = @()
foreach ($c in $corridas) {
    $exe = $c[0]; $esc = $c[1]
    $base = [IO.Path]::GetFileNameWithoutExtension($exe)
    $tag = $base + '_' + $esc
    $argv = @($esc, '-o', (Join-Path $sal "$tag.txt"))
    if ($esc -eq 'comandos') { $argv += @('-ee_out', (Join-Path $sal "ee_$base.bin")) }
    if ($esc -eq 'persistencia') { $argv += @('-ee_in', (Join-Path $sal "ee_$base.bin")) }
    if ($Verbose) { $argv += '-v' }
    $log = & $exe @argv
    $codigo = $LASTEXITCODE
    $log | Out-File -Encoding utf8 (Join-Path $sal "$tag.log")
    Write-Host ''
    $log | Where-Object { $_ -match '\[FALLA\]|\[nota\]|Resultado|^==' } | ForEach-Object { Write-Host $_ }
    # Solo cuenta como bien si el banco llego al final y salio con 0
    $ok = ($codigo -eq 0) -and (@($log | Where-Object { $_ -match 'Resultado \[' }).Count -eq 1)
    $resumen += [pscustomobject]@{ Prueba = $tag; Resultado = $(if ($ok) { 'BIEN' } else { 'MAL' }) }
}

$resumen += $memoria
Write-Host ''
Write-Host '=== Resumen ==='
$resumen | Format-Table -AutoSize | Out-String | Write-Host
$malas = @($resumen | Where-Object { $_.Resultado -ne 'BIEN' }).Count
if ($malas) { Write-Host "$malas corridas con fallas (ver banco\salida\*.log)"; exit 1 }
Write-Host 'Todas las corridas pasaron.'
