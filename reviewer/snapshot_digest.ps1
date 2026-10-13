# Payload digest of a root snapshot, shared by check_integrity.ps1 and recapture_snapshot.ps1.
# Dot-source it: . "$PSScriptRoot/snapshot_digest.ps1"
#
# Header bytes 352-423 of a layout-1 snapshot (ter2_root/frozen/ter_root_snapshot.hpp) are run
# metadata: process_peak_working_set, capture_elapsed_ns, git_commit and header_checksum. The payload
# digest is the SHA-256 of every other byte: the three root lists and every deterministic header
# field. Two captures of the same root share it; their whole-file SHA-256 values never match.

$SnapshotDigestFile = "reviewer/snapshot_payload_digests.json"

function Get-SnapshotPayloadDigest([string]$path) {
    $metaStart = 352; $headerBytes = 424
    # .NET resolves relative paths against the process directory, not Set-Location; resolve here.
    $stream = [IO.File]::OpenRead((Resolve-Path -LiteralPath $path).ProviderPath)
    $hash = [Security.Cryptography.IncrementalHash]::CreateHash([Security.Cryptography.HashAlgorithmName]::SHA256)
    try {
        $buffer = [byte[]]::new(1MB)
        $stream.ReadExactly($buffer, 0, $headerBytes)
        $layout = [BitConverter]::ToUInt32($buffer, 8); $size = [BitConverter]::ToUInt32($buffer, 12)
        if ($layout -ne 1 -or $size -ne $headerBytes) {
            throw "$path has snapshot layout $layout with a $size-byte header; this digest is defined for layout 1 ($headerBytes bytes)"
        }
        $hash.AppendData($buffer, 0, $metaStart)
        while (($read = $stream.Read($buffer, 0, $buffer.Length)) -gt 0) { $hash.AppendData($buffer, 0, $read) }
        [Convert]::ToHexString($hash.GetHashAndReset()).ToLowerInvariant()
    } finally {
        $hash.Dispose(); $stream.Dispose()
    }
}
