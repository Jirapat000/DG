$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$runtime = Join-Path $root '.runtime\python314'
$python = Join-Path $runtime 'python.exe'
$installer = Join-Path $root '.runtime\python-3.14.10-amd64.exe'
$venvPython = Join-Path $root '.venv\Scripts\python.exe'

if (-not (Test-Path -LiteralPath $python)) {
    New-Item -ItemType Directory -Force -Path (Split-Path $installer) | Out-Null
    if (-not (Test-Path -LiteralPath $installer)) {
        Invoke-WebRequest -Uri 'https://www.python.org/ftp/python/3.14.10/python-3.14.10-amd64.exe' -OutFile $installer
    }
    & $installer /quiet 'InstallAllUsers=0' "TargetDir=$runtime" 'Include_pip=1' 'Include_launcher=0' 'Include_test=0' 'Include_doc=0' 'Include_tcltk=0' 'Shortcuts=0' 'PrependPath=0'
    if (-not (Test-Path -LiteralPath $python)) { throw 'Python installer finished without creating the project-local interpreter.' }
}

if (-not (Test-Path -LiteralPath $venvPython)) {
    & $python -m venv (Join-Path $root '.venv')
    if ($LASTEXITCODE -ne 0) { throw "Creating the project virtual environment failed with exit code $LASTEXITCODE" }
}

& $venvPython -m pip install --upgrade pip
if ($LASTEXITCODE -ne 0) { throw "Updating pip failed with exit code $LASTEXITCODE" }
& $venvPython -m pip install -r (Join-Path $root 'requirements.txt')
if ($LASTEXITCODE -ne 0) { throw "Installing project and model dependencies failed with exit code $LASTEXITCODE" }
& $venvPython -m pip install --upgrade torch torchvision --index-url 'https://download.pytorch.org/whl/cu132'
if ($LASTEXITCODE -ne 0) { throw "Installing the CUDA-enabled PyTorch build failed with exit code $LASTEXITCODE" }
& $venvPython -m pip install huggingface_hub
if ($LASTEXITCODE -ne 0) { throw "Installing the dataset download client failed with exit code $LASTEXITCODE" }
& $venvPython -m pip install onnx onnxslim onnxruntime
if ($LASTEXITCODE -ne 0) { throw "Installing ONNX export dependencies failed with exit code $LASTEXITCODE" }
& $venvPython (Join-Path $PSScriptRoot 'prepare_floatwaste.py')
if ($LASTEXITCODE -ne 0) { throw "Preparing the FloatWaste dataset failed with exit code $LASTEXITCODE" }
& $venvPython (Join-Path $PSScriptRoot 'train_floatwaste.py') --epochs 80 --imgsz 640 --batch 4 --device 0
if ($LASTEXITCODE -ne 0) { throw "YOLO detector training failed with exit code $LASTEXITCODE" }
& $venvPython (Join-Path $PSScriptRoot 'export_onnx.py')
if ($LASTEXITCODE -ne 0) { throw "ONNX export failed with exit code $LASTEXITCODE" }
