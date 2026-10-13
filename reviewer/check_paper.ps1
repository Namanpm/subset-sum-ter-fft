# Regenerates every number and table in the article from the frozen inputs and checks the DOCX.
# Needs Python 3 with NumPy, Matplotlib and lxml; no build, no native executables. A few seconds.
#
#   pwsh -NoProfile -File reviewer/check_paper.ps1
#   pwsh -NoProfile -File reviewer/check_paper.ps1 -Python C:\path\to\python.exe
#
# Steps, all from paper/ (paper/README.md):
#   1. scaling_model.py              occupancy and cost model from data/scaling_inputs.json
#   2. build_results.py --tables-only  hash-checks every frozen input, recomputes medians, ratios,
#                                    cluster intervals and the fourteen tables
#   3. validate_docx_only.py --portable  every equation, table cell and figure of the DOCX against
#                                    the regenerated payloads; report in paper/qa/portable/
# Then it confirms the regenerated files in paper/data/ and paper/tables/ equal the committed ones.
# Line endings are ignored in that comparison, because Python writes CRLF on Windows and LF elsewhere.
# The script never touches PDF or TeX files.
param([string]$Python = "python")

$ErrorActionPreference = "Stop"
Set-Location -LiteralPath (Join-Path (Split-Path -Parent $PSScriptRoot) "paper")
$env:PYTHONIOENCODING = "utf-8"

function Get-ContentDigests {
    # Latin-1 maps every byte to one char and back, so removing CR in the string removes CR bytes.
    $latin1 = [Text.Encoding]::Latin1
    $digests = @{}
    foreach ($f in Get-ChildItem -File data, tables) {
        $text = $latin1.GetString([IO.File]::ReadAllBytes($f.FullName)).Replace("`r", "")
        $digests["$($f.Directory.Name)/$($f.Name)"] = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($latin1.GetBytes($text)))
    }
    $digests
}

$before = Get-ContentDigests
foreach ($step in @(@("scaling_model.py"), @("build_results.py", "--tables-only"), @("validate_docx_only.py", "--portable"))) {
    Write-Host "> $Python $($step -join ' ')"
    $output = & $Python @step 2>&1
    if ($LASTEXITCODE -ne 0) { $output | Select-Object -Last 20 | Write-Host; throw "$($step[0]) exited $LASTEXITCODE" }
}
$after = Get-ContentDigests

$changed = @($after.Keys | Where-Object { $before[$_] -ne $after[$_] } | Sort-Object)
if ($changed.Count -gt 0) {
    Write-Host "FAIL  regenerated files differ from the committed ones: $($changed -join ', ')" -ForegroundColor Red
    exit 1
}
$report = Get-Content -Raw qa/portable/docx_validation.json | ConvertFrom-Json
Write-Host ("PASS  {0} equations, {1} tables ({2} data rows), {3} figures verified in the DOCX (sha256 {4})" -f
    $report.native_word_equations, $report.tables, $report.total_table_data_rows, $report.figures, $report.docx_sha256) -ForegroundColor Green
Write-Host "PASS  regenerated paper/data and paper/tables equal the committed files ($($after.Count) files)" -ForegroundColor Green
