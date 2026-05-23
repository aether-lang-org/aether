# Smoke tape for the std.http.server.vcr v0.1 module.
#
# Servirtium markdown format: each interaction is a `## Interaction N:`
# header followed by four `### ...` sections (request headers, request
# body, response headers, response body). For GET-only v0.1 only the
# response-body section is consulted by the parser; the others are
# ignored at load time but kept so the file stays valid Servirtium and
# stays interchangeable with tapes from other Servirtium implementations.

## Interaction 0: GET /ok

### Request headers recorded for playback:

```
Host: 127.0.0.1:18094
```

### Request body recorded for playback ():

```
```

### Response headers recorded for playback:

```
Content-Type: text/plain
```

### Response body recorded for playback (200: text/plain):

```
ok-body
```
