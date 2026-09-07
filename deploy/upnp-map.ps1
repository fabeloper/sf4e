# Asks the router (via UPnP) to forward the sf4e lobby server's UDP ports to
# THIS machine. Run it on the machine that runs the server, any time the
# router has forgotten the mappings (typically after it reboots).
#
#   powershell -ExecutionPolicy Bypass -File upnp-map.ps1
#
# Works on routers that answer UPnP discovery. If it reports "no UPnP router
# found", forward the ports by hand in the router's app or web page instead.

$ErrorActionPreference = 'Continue'
$ports = @(23400..23420) + @(24001..24020)

# This machine's address on the network the router is on.
$route = Get-NetRoute -DestinationPrefix '0.0.0.0/0' | Sort-Object RouteMetric | Select-Object -First 1
$myIp = (Get-NetIPAddress -InterfaceIndex $route.InterfaceIndex -AddressFamily IPv4).IPAddress
$gateway = $route.NextHop
Write-Host "This machine: $myIp   router: $gateway"

# Find the router's UPnP service.
$msearch = "M-SEARCH * HTTP/1.1`r`nHOST: 239.255.255.250:1900`r`nMAN: `"ssdp:discover`"`r`nMX: 2`r`nST: urn:schemas-upnp-org:service:WANIPConnection:1`r`n`r`n"
$bytes = [Text.Encoding]::ASCII.GetBytes($msearch)
$udp = New-Object System.Net.Sockets.UdpClient
$udp.Client.ReceiveTimeout = 4000
$location = $null
foreach ($target in @($gateway, '239.255.255.250')) {
    try {
        $udp.Send($bytes, $bytes.Length, $target, 1900) | Out-Null
        $ep = New-Object System.Net.IPEndPoint([Net.IPAddress]::Any, 0)
        $resp = [Text.Encoding]::ASCII.GetString($udp.Receive([ref]$ep))
        $location = (($resp -split "`r`n" | Where-Object { $_ -match '^LOCATION:' }) -replace '^LOCATION:\s*', '').Trim()
        if ($location) { break }
    } catch { }
}
$udp.Close()
if (-not $location) {
    Write-Host "No UPnP router found. Forward UDP 23400-23420 and 24001-24020 to $myIp by hand." -ForegroundColor Yellow
    exit 1
}

$xml = [xml](Invoke-WebRequest -Uri $location -TimeoutSec 8 -UseBasicParsing).Content
$svcNode = $xml.SelectNodes('//*[local-name()="service"]') | Where-Object { $_.serviceType -match 'WAN(IP|PPP)Connection' } | Select-Object -First 1
if (-not $svcNode) { Write-Host "Router answered but has no WAN connection service." -ForegroundColor Yellow; exit 1 }
$svc = $svcNode.serviceType
$base = [uri]$location
$ctl = if ($svcNode.controlURL -match '^http') { $svcNode.controlURL } else { "$($base.Scheme)://$($base.Host):$($base.Port)$($svcNode.controlURL)" }
Write-Host "Router UPnP: $($xml.root.device.manufacturer) $($xml.root.device.modelName) at $ctl"

function Soap($action, $inner) {
    $body = "<?xml version=`"1.0`"?><s:Envelope xmlns:s=`"http://schemas.xmlsoap.org/soap/envelope/`" s:encodingStyle=`"http://schemas.xmlsoap.org/soap/encoding/`"><s:Body><u:$action xmlns:u=`"$svc`">$inner</u:$action></s:Body></s:Envelope>"
    try {
        $r = Invoke-WebRequest -Uri $ctl -Method Post -Body $body -ContentType 'text/xml; charset="utf-8"' -Headers @{ SOAPAction = "`"$svc#$action`"" } -TimeoutSec 12 -UseBasicParsing
        return $true
    } catch { return $false }
}

$added = 0; $failed = @()
foreach ($port in $ports) {
    $inner = "<NewRemoteHost></NewRemoteHost><NewExternalPort>$port</NewExternalPort><NewProtocol>UDP</NewProtocol><NewInternalPort>$port</NewInternalPort><NewInternalClient>$myIp</NewInternalClient><NewEnabled>1</NewEnabled><NewPortMappingDescription>sf4e lobby</NewPortMappingDescription><NewLeaseDuration>0</NewLeaseDuration>"
    if (Soap 'AddPortMapping' $inner) { $added++ } else { $failed += $port }
    Start-Sleep -Milliseconds 120
}
# Routers get flaky under a burst; one retry pass for the stragglers.
foreach ($port in @($failed)) {
    Start-Sleep -Milliseconds 300
    $inner = "<NewRemoteHost></NewRemoteHost><NewExternalPort>$port</NewExternalPort><NewProtocol>UDP</NewProtocol><NewInternalPort>$port</NewInternalPort><NewInternalClient>$myIp</NewInternalClient><NewEnabled>1</NewEnabled><NewPortMappingDescription>sf4e lobby</NewPortMappingDescription><NewLeaseDuration>0</NewLeaseDuration>"
    if (Soap 'AddPortMapping' $inner) { $added++; $failed = $failed | Where-Object { $_ -ne $port } }
}
Write-Host "Mapped $added of $($ports.Count) ports to $myIp." -ForegroundColor Green
if ($failed.Count -gt 0) { Write-Host "Still missing: $($failed -join ', ') (run again in a minute)" -ForegroundColor Yellow }
