param([switch]$SkipBuild)
$ErrorActionPreference='Stop'
$taskRoot=$PSScriptRoot
$taskPython='C:\Users\naman\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe'
$taskTex='C:\Users\naman\AppData\Local\Programs\MiKTeX\miktex\bin\x64\pdflatex.exe'
$env:PYTHONIOENCODING='utf-8'
Push-Location -LiteralPath $taskRoot
try {
    if (-not $SkipBuild) {
        & $taskPython (Join-Path $taskRoot 'build_article.py')
        if ($LASTEXITCODE -ne 0) { throw 'Article source generation failed' }
    }
    for ($taskPass=1;$taskPass -le 3;$taskPass++) {
        & $taskTex --disable-installer -interaction=nonstopmode -halt-on-error -file-line-error -output-directory=qa manuscript.tex *> (Join-Path $taskRoot "qa\latex-pass-$taskPass.txt")
        if ($LASTEXITCODE -ne 0) { throw "LaTeX pass $taskPass failed; inspect qa\latex-pass-$taskPass.txt" }
    }
    Copy-Item -LiteralPath (Join-Path $taskRoot 'qa\manuscript.pdf') -Destination (Join-Path $taskRoot 'ARWLF_Naman_PM.pdf') -Force
} finally { Pop-Location }
