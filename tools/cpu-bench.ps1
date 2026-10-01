# Single-core SHA-256 throughput on cpu0 (silver) and cpu4 (gold): scales with the core clock.
[Console]::OutputEncoding = [Text.Encoding]::UTF8
$buf = New-Object byte[] (64MB)
$sha = [System.Security.Cryptography.SHA256]::Create()
$p = [Diagnostics.Process]::GetCurrentProcess()
$p.PriorityClass = 'High'
foreach ($c in @(@(0x01, 'cpu0 silver'), @(0x10, 'cpu4 gold'))) {
    $p.ProcessorAffinity = [IntPtr]$c[0]
    $null = $sha.ComputeHash($buf, 0, 4MB)
    $best = 0
    for ($i = 0; $i -lt 3; $i++) {
        $sw = [Diagnostics.Stopwatch]::StartNew()
        $null = $sha.ComputeHash($buf)
        $sw.Stop()
        $best = [math]::Max($best, 64 / $sw.Elapsed.TotalSeconds)
    }
    "{0}: {1:N0} MB/s" -f $c[1], $best
}
"load: " + (Get-CimInstance Win32_Processor | Measure-Object LoadPercentage -Average).Average + "%"
