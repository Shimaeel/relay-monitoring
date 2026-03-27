# ============================================================
#  serve.ps1 — Local HTTP server for relay-app with COOP/COEP
# ============================================================
#
#  SharedArrayBuffer requires Cross-Origin Isolation headers:
#    Cross-Origin-Opener-Policy: same-origin
#    Cross-Origin-Embedder-Policy: require-corp
#
#  Usage:
#    .\serve.ps1          # Serve on port 3000
#    .\serve.ps1 -Port 8080  # Custom port
#
#  Then open: http://localhost:3000/index.html
# ============================================================

param(
    [int]$Port = 3000
)

$listener = [System.Net.HttpListener]::new()
$listener.Prefixes.Add("http://localhost:$Port/")
$listener.Start()

Write-Host ""
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  Relay-App Static Server" -ForegroundColor Cyan
Write-Host "  http://localhost:$Port/" -ForegroundColor Green
Write-Host "  Cross-Origin Isolated: YES" -ForegroundColor Yellow
Write-Host "  Press Ctrl+C to stop" -ForegroundColor DarkGray
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

# MIME types for static files
$mimeTypes = @{
    ".html" = "text/html; charset=utf-8"
    ".css"  = "text/css; charset=utf-8"
    ".js"   = "application/javascript; charset=utf-8"
    ".json" = "application/json; charset=utf-8"
    ".png"  = "image/png"
    ".jpg"  = "image/jpeg"
    ".jpeg" = "image/jpeg"
    ".gif"  = "image/gif"
    ".svg"  = "image/svg+xml"
    ".ico"  = "image/x-icon"
    ".woff" = "font/woff"
    ".woff2"= "font/woff2"
    ".ttf"  = "font/ttf"
    ".map"  = "application/json"
}

$root = $PSScriptRoot

try {
    while ($listener.IsListening) {
        $context  = $listener.GetContext()
        $request  = $context.Request
        $response = $context.Response

        # Cross-Origin Isolation headers (required for SharedArrayBuffer)
        $response.Headers.Set("Cross-Origin-Opener-Policy", "same-origin")
        $response.Headers.Set("Cross-Origin-Embedder-Policy", "require-corp")

        # Resolve file path
        $urlPath = $request.Url.LocalPath
        if ($urlPath -eq "/") { $urlPath = "/index.html" }

        $filePath = Join-Path $root ($urlPath -replace "/", "\")

        if (Test-Path $filePath -PathType Leaf) {
            $ext = [System.IO.Path]::GetExtension($filePath).ToLower()
            $mime = if ($mimeTypes.ContainsKey($ext)) { $mimeTypes[$ext] } else { "application/octet-stream" }

            $response.ContentType = $mime
            $response.StatusCode  = 200

            $bytes = [System.IO.File]::ReadAllBytes($filePath)
            $response.ContentLength64 = $bytes.Length
            $response.OutputStream.Write($bytes, 0, $bytes.Length)

            Write-Host "  200  $urlPath" -ForegroundColor Green
        }
        else {
            $response.StatusCode = 404
            $body = [System.Text.Encoding]::UTF8.GetBytes("404 Not Found: $urlPath")
            $response.ContentLength64 = $body.Length
            $response.OutputStream.Write($body, 0, $body.Length)

            Write-Host "  404  $urlPath" -ForegroundColor Red
        }

        $response.OutputStream.Close()
    }
}
finally {
    $listener.Stop()
    $listener.Close()
    Write-Host "`nServer stopped." -ForegroundColor DarkGray
}
