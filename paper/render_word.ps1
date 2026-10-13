$ErrorActionPreference='Stop'
$taskRoot=$PSScriptRoot
$taskWord=$null
$taskDocument=$null
try {
    $taskWord=New-Object -ComObject Word.Application
    $taskWord.Visible=$false
    $taskWord.DisplayAlerts=0
    $taskDocument=$taskWord.Documents.Open((Join-Path $taskRoot 'ARWLF_Naman_PM.docx'),$false,$true)
    $null=$taskDocument.Fields.Update()
    foreach ($taskSection in $taskDocument.Sections) {
        foreach ($taskFooter in $taskSection.Footers) { $null=$taskFooter.Range.Fields.Update() }
    }
    $taskDocument.Repaginate()
    $taskDocument.ExportAsFixedFormat((Join-Path $taskRoot 'qa\word-render.pdf'),17)
    Write-Output "Word exported $($taskDocument.ComputeStatistics(2)) pages"
} finally {
    if ($taskDocument) { $taskDocument.Close(0) }
    if ($taskWord) { $taskWord.Quit() }
}
