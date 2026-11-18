# Instala el motor de IA de AI Voice Converter para el usuario actual.
#  1) Busca Python 3.10 (si no hay, descarga e instala Python 3.10.11 solo para este usuario)
#  2) Crea %LOCALAPPDATA%\AIVoiceConverter\venv con PyTorch 2.1.1 (CUDA 11.8 si hay GPU NVIDIA) + rvc-python + ffmpeg
#  3) Descarga los modelos base de RVC (hubert, rmvpe)
# Solo ASCII a proposito: Windows PowerShell 5 lee los .ps1 sin BOM como ANSI.

param(
    [Parameter(Mandatory = $true)][string]$EngineScript,
    [string]$PythonInstaller = "",
    [switch]$CpuOnly
)

# 'Continue': con 'Stop', Windows PowerShell 5 corta ante cualquier linea que un .exe escriba en stderr.
# Los errores reales se comprueban a mano con $LASTEXITCODE y 'throw'.
$ErrorActionPreference = 'Continue'
$ProgressPreference = 'SilentlyContinue'   # Invoke-WebRequest es muy lento con la barra
try { [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 } catch {}

$PythonVersion = '3.10.11'   # ultima 3.10 con instalador binario; rvc-python/fairseq exigen 3.10
$PythonUrl     = "https://www.python.org/ftp/python/$PythonVersion/python-$PythonVersion-amd64.exe"

$Root    = Join-Path $env:LOCALAPPDATA 'AIVoiceConverter'
$Venv    = Join-Path $Root 'venv'
$VenvPy  = Join-Path $Venv 'Scripts\python.exe'
$LogFile = Join-Path $Root 'install.log'

New-Item -ItemType Directory -Force -Path $Root | Out-Null
Start-Transcript -Path $LogFile -Force | Out-Null

function Write-Step([string]$msg) {
    Write-Host ''
    Write-Host "=== $msg ===" -ForegroundColor Cyan
}

function Test-PythonExe([string]$exe) {
    # Devuelve $true si es un Python real 3.10 (descarta el atajo de Microsoft Store)
    if (-not $exe -or -not (Test-Path $exe)) { return $false }
    if ($exe -like '*\WindowsApps\*') { return $false }
    try {
        $out = & $exe -c "import sys; print(int(sys.version_info[:2] == (3,10)))" 2>$null
        return ($LASTEXITCODE -eq 0 -and "$out".Trim() -eq '1')
    } catch { return $false }
}

function Find-Python {
    foreach ($v in '3.10') {
        try {
            $p = & py "-$v" -c "import sys; print(sys.executable)" 2>$null
            if ($LASTEXITCODE -eq 0 -and (Test-PythonExe "$p".Trim())) { return "$p".Trim() }
        } catch {}
    }
    foreach ($v in '310') {
        $c = Join-Path $env:LOCALAPPDATA "Programs\Python\Python$v\python.exe"
        if (Test-PythonExe $c) { return $c }
        foreach ($pf in $env:ProgramFiles, ${env:ProgramFiles(x86)}) {
            if ($pf) {
                $c = Join-Path $pf "Python$v\python.exe"
                if (Test-PythonExe $c) { return $c }
            }
        }
    }
    $cmd = Get-Command python -ErrorAction SilentlyContinue
    if ($cmd -and (Test-PythonExe $cmd.Source)) { return $cmd.Source }
    return $null
}

function Show-Engine-Output($line) {
    $t = "$line".Trim()
    if (-not $t) { return }
    if ($t -match '^PROGRESS (\d+)') { Write-Host ("[{0,3}%]" -f $Matches[1]) }
    elseif ($t -match '^STATUS (.*)') { Write-Host ("  -> " + $Matches[1]) -ForegroundColor Yellow }
    elseif ($t -match '^ERROR (.*)')  { Write-Host ("  !! " + $Matches[1]) -ForegroundColor Red }
    elseif ($t -match '^(LOG|RESULT) (.*)') { if ($Matches[1] -eq 'LOG') { Write-Host ("     " + $Matches[2]) -ForegroundColor DarkGray } }
    else { Write-Host "     $t" -ForegroundColor DarkGray }
}

try {
    Write-Host 'AI Voice Converter - instalacion del motor de IA' -ForegroundColor Green
    Write-Host "Registro: $LogFile"

    # 1) Python ---------------------------------------------------------
    Write-Step 'Buscando Python 3.10'
    $py = Find-Python
    if (-not $py) {
        if ($PythonInstaller -and (Test-Path $PythonInstaller)) {
            Write-Host 'No se encontro. Usando el instalador de Python incluido ...'
            $installer = $PythonInstaller
        } else {
            Write-Host "No se encontro. Descargando Python $PythonVersion desde python.org ..."
            $installer = Join-Path $env:TEMP "python-$PythonVersion-amd64.exe"
            Invoke-WebRequest -Uri $PythonUrl -OutFile $installer -UseBasicParsing -ErrorAction Stop
        }
        Write-Host 'Instalando Python (solo para este usuario) ...'
        $p = Start-Process -FilePath $installer -Wait -PassThru -ErrorAction Stop -ArgumentList @(
            '/quiet', 'InstallAllUsers=0', 'PrependPath=1', 'Include_launcher=0', 'Include_test=0', 'Include_doc=0')
        if ($installer -ne $PythonInstaller) { Remove-Item $installer -ErrorAction SilentlyContinue }
        if ($p.ExitCode -ne 0) { throw "El instalador de Python fallo (codigo $($p.ExitCode))." }
        $py = Find-Python
        if (-not $py) { throw 'Python se instalo pero no se pudo localizar. Reinicia y vuelve a intentar desde el menu Motor del plugin.' }
    }
    Write-Host "Usando: $py"

    # 2) Entorno virtual + PyTorch + RVC -----------------------------------
    Write-Step 'Instalando PyTorch 2.1.1 + RVC (varios GB, puede tardar bastante)'
    $setupArgs = @('-u', $EngineScript, 'setup', '--venv', $Venv)
    if ($CpuOnly) { $setupArgs += '--cpu-only' }
    & $py @setupArgs 2>&1 | ForEach-Object { Show-Engine-Output $_ }
    if ($LASTEXITCODE -ne 0) { throw "La instalacion del motor fallo (codigo $LASTEXITCODE). Mira el registro: $LogFile" }

    # 3) Modelo --------------------------------------------------------------
    Write-Step 'Descargando modelos base de RVC'
    & $VenvPy -u $EngineScript fetch-model 2>&1 | ForEach-Object { Show-Engine-Output $_ }
    if ($LASTEXITCODE -ne 0) {
        Write-Host 'No se pudo descargar el modelo ahora; se descargara en la primera separacion.' -ForegroundColor Yellow
    }

    # 4) Resumen --------------------------------------------------------------
    Write-Step 'Verificando'
    & $VenvPy -u $EngineScript probe 2>&1 | ForEach-Object {
        if ("$_" -like 'PROBE *') {
            $info = ("$_".Substring(6)) | ConvertFrom-Json
            if ($info.cuda) { Write-Host ("Listo: GPU $($info.gpu) (CUDA)") -ForegroundColor Green }
            elseif ($info.nvidia_gpu_present) { Write-Host 'Listo en CPU. Hay GPU NVIDIA pero PyTorch quedo sin CUDA; revisa el registro.' -ForegroundColor Yellow }
            else { Write-Host 'Listo en CPU (no se detecto GPU NVIDIA).' -ForegroundColor Green }
        }
    }
    Write-Host ''
    Write-Host 'Instalacion terminada. Reinicia tu DAW y vuelve a escanear los plugins VST3.' -ForegroundColor Green
    Stop-Transcript | Out-Null
    Start-Sleep -Seconds 4
    exit 0
}
catch {
    Write-Host ''
    Write-Host ("ERROR: " + $_.Exception.Message) -ForegroundColor Red
    Write-Host "Podes reintentar desde el plugin: menu Motor > Instalar motor. Registro: $LogFile"
    try { Stop-Transcript | Out-Null } catch {}
    Read-Host 'Presiona Enter para cerrar'
    exit 1
}
