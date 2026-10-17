# HTTP and WebSocket receiver lifetime regression

Run from the repository root using the installed Node.js and Windows MSVC:

```powershell
node tests/dog_host/http_lifetime_host/run.mjs
```

The runner compiles the repository's local copies of `http_client.cc`, `web_socket.cc`, `esp_tcp.cc`, and `esp_ssl.cc`. Only FreeRTOS scheduling/event groups, sockets, and TLS I/O are host shims. It does not start WSL, download tools, open any real network socket, touch a serial port, or change networking.

The controlled callback barriers verify that an owner cannot destroy its event group while a callback is still unwinding. Active and passive disconnects verify one notification, one socket close, and draining an already disconnected receiver. Callback initiated close verifies that the receive task never joins itself. Task allocation failure verifies socket/TLS cleanup. HTTP tests cover truncated bodies and repeated full-queue producer shutdown. WebSocket tests cover fast handshakes, owner teardown after close frames, reconnect state, and inline pong handling. The stress loop runs 100 pairs of active TCP/TLS closes. There are currently 770 individual checks, not 770 independent test scenarios.

Results are written to `build/dog_host/http_lifetime_host/fixed-result.json` and build logs beside it. Deleted host event groups remain allocated and marked as deleted to detect any use after their logical destruction without deliberately introducing native undefined behavior.

An unmodified upstream copy of the component can demonstrate the two original lifetime failures. Point `HTTP_LIFETIME_COMPONENT` at a pristine upstream `esp-ml307` checkout (the repository's `components/78__esp-ml307` already contains the fix):

```powershell
$env:HTTP_LIFETIME_COMPONENT = (Resolve-Path path\to\upstream\esp-ml307).Path
node tests/dog_host/http_lifetime_host/run.mjs --baseline
Remove-Item Env:HTTP_LIFETIME_COMPONENT
```

The baseline executable intentionally exits 1: it detects that HTTP deletes dependent resources before its pending receiver finishes, and that TCP Disconnect returns while a passive disconnect callback is still running. Its result is `baseline-result.json`. This demonstrates the unsafe lifecycle, not the ESP-specific semaphore assertion itself.

These tests cover native owner/callback ordering and simulated transport concurrency. ESP scheduler, real TLS/socket behavior, OTA service, Wi-Fi, and board boot must be validated separately on the device. Destroying a transport owner from inside its own receive callback is unsupported; callbacks may request close, with owner destruction deferred to its owning task.
