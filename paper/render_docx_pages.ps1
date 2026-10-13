param(
    [string]$InputDocx = (Join-Path $PSScriptRoot 'ARWLF_Naman_PM.docx'),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot 'qa\continuation-20261009\pages'),
    [int]$Dpi = 150,
    [string]$PythonExecutable = 'C:\Users\naman\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe'
)
$ErrorActionPreference='Stop'
$taskWord=$null
$taskDocument=$null
$null=New-Item -ItemType Directory -Path $OutputDirectory -Force
try {
    $taskWord=New-Object -ComObject Word.Application
    $taskWord.Visible=$false
    $taskWord.DisplayAlerts=0
    $taskDocument=$taskWord.Documents.Open($InputDocx,$false,$true)
    $null=$taskDocument.Fields.Update()
    foreach ($taskSection in $taskDocument.Sections) {
        foreach ($taskFooter in $taskSection.Footers) {$null=$taskFooter.Range.Fields.Update()}
    }
    $taskDocument.ActiveWindow.View.Type=3
    $taskDocument.Repaginate()
    $taskCount=$taskDocument.ComputeStatistics(2)
    $taskPages=$taskDocument.ActiveWindow.ActivePane.Pages
    $taskMetadata=@()
    for($taskIndex=1;$taskIndex -le $taskCount;$taskIndex++) {
        $taskBytes=[byte[]]$taskPages.Item($taskIndex).EnhMetaFileBits
        $taskPath=Join-Path $OutputDirectory ('page-{0:d2}' -f $taskIndex)
        [System.IO.File]::WriteAllBytes(($taskPath+'.emf'),$taskBytes)
        $taskWidth=[int][Math]::Round($taskDocument.PageSetup.PageWidth/72*$Dpi)
        $taskHeight=[int][Math]::Round($taskDocument.PageSetup.PageHeight/72*$Dpi)
        $taskStart=$taskDocument.GoTo(1,1,$taskIndex).Start
        $taskEnd=if($taskIndex -lt $taskCount){$taskDocument.GoTo(1,1,($taskIndex+1)).Start}else{$taskDocument.Content.End}
        $taskText=$taskDocument.Range($taskStart,$taskEnd).Text
        [System.IO.File]::WriteAllText(($taskPath+'.txt'),$taskText,[System.Text.Encoding]::UTF8)
        $taskMetadata += [pscustomobject]@{Page=$taskIndex;Start=$taskStart;End=$taskEnd;Width=$taskWidth;Height=$taskHeight;Characters=$taskText.Length}
    }
    $taskMetadata | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'pages.json') -Encoding utf8
} finally {
    if($taskDocument){$taskDocument.Close(0)}
    if($taskWord){$taskWord.Quit()}
}
& $PythonExecutable -B (Join-Path $PSScriptRoot 'rasterize_word_emf.py') $OutputDirectory
if ($LASTEXITCODE -ne 0) {throw 'Native Word EMF rasterization failed'}
Write-Output "Word rendered $taskCount pages directly to EMF/PNG; input opened read-only, no PDF created."
