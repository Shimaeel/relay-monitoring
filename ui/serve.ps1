# Simple HTTP server with COOP/COEP headers for SharedArrayBuffer support
# Usage: .\serve.ps1 [port]
# Then open http://localhost:8080 in browser

param([int]$Port = 8080)

$listener = New-Object System.Net.HttpListener
$listener.Prefixes.Add("http://localhost:$Port/")
$listener.Start()

Write-Host "Serving UI at http://localhost:$Port"
Write-Host "Press Ctrl+C to stop"

$mimeTypes = @{
    ".html" = "text/html"
    ".js"   = "application/javascript"
    ".css"  = "text/css"
    ".json" = "application/json"
    ".png"  = "image/png"
    ".ico"  = "image/x-icon"
}

try {
    while ($listener.IsListening) {
        $context = $listener.GetContext()
        $request = $context.Request
        $response = $context.Response
        
        $localPath = $request.Url.LocalPath
        if ($localPath -eq "/") { $localPath = "/index.html" }
        
        $filePath = Join-Path $PSScriptRoot $localPath.TrimStart("/")
        
        # Required headers for SharedArrayBuffer
        $response.Headers.Add("Cross-Origin-Opener-Policy", "same-origin")
        $response.Headers.Add("Cross-Origin-Embedder-Policy", "require-corp")
        
        if (Test-Path $filePath -PathType Leaf) {
            $ext = [System.IO.Path]::GetExtension($filePath)
            $response.ContentType = $mimeTypes[$ext] ?? "application/octet-stream"
            $content = [System.IO.File]::ReadAllBytes($filePath)
            $response.ContentLength64 = $content.Length
            $response.OutputStream.Write($content, 0, $content.Length)
            Write-Host "$($request.HttpMethod) $localPath - 200"
        } else {
            $response.StatusCode = 404
            $msg = [System.Text.Encoding]::UTF8.GetBytes("Not Found")
            $response.OutputStream.Write($msg, 0, $msg.Length)
            Write-Host "$($request.HttpMethod) $localPath - 404"
        }
        
        $response.Close()
    }
} finally {
    $listener.Stop()
}
