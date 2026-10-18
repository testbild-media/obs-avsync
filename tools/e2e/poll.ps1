# Polls /api/status every N seconds and appends compact lines (for debugging start-up behaviour).
param([int]$Port = 8765, [int]$Seconds = 40, [double]$Interval = 2, [string]$Out)
$end = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $end) {
    try {
        $s = Invoke-RestMethod -Uri "http://127.0.0.1:$Port/api/status" -TimeoutSec 2
        $line = "{0:HH:mm:ss.fff} ntp={1} sysoff={2} | " -f (Get-Date), $s.clock.ntp, [math]::Round($s.clock.systemOffsetMs, 1)
        foreach ($p in $s.probes) {
            if ($p.recent.Count -gt 0) {
                $line += "{0}:{1} [{2}] | " -f $p.kind, $p.name, (($p.recent | Select-Object -Last 4 | ForEach-Object { "{0}{1}={2:N1}" -f ($_.seq % 100), $(if ($_.marker) { 'M' } else { '' }), $_.latencyMs }) -join ' ')
            }
        }
        $line | Add-Content -Encoding utf8 $Out
    } catch { "{0:HH:mm:ss.fff} -" -f (Get-Date) | Add-Content -Encoding utf8 $Out }
    Start-Sleep -Milliseconds ($Interval * 1000)
}
