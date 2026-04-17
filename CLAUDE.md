# Jamulus Fork — mcfnord/jamulus

This is a fork of [jamulussoftware/jamulus](https://github.com/jamulussoftware/jamulus).
The `main` branch tracks upstream. Custom work lives in separate branches and will be
consolidated into a single `jamfan` release branch. Changes here are not expected to be
accepted upstream.

## Custom Branches (to be merged into `jamfan`)

### `central-defense`
Adds `src/centraldefense.cpp`: rejects server connections from a centralized blocklist
of ASNs and IPv4 CIDR masks. The blocklist URL and ASN lookup base URL are configurable.
Defaults to `https://jamulus.live/ip-lookup` for ASN lookups.

### `earlier-join-notification`
Fires a connection notification at the earliest socket step, before the full handshake
completes. Modifies `socket.cpp` and `serverlogging`.

### `make_welcome`
Debian packaging: `postinst` creates `/etc/jamulus/welcome.html` on install. The systemd
service already passes `-w /etc/jamulus/welcome.html` to the binary.

## Planned Custom Features (not yet implemented)

### Single release branch: `jamfan`
All of the above branches will be consolidated into `jamfan`, rebased onto upstream
`main`, and used to build and release custom server and client binaries.

### Chat URL reporting (server + client)
Chat messages are scanned for two URL categories: **chord websites** and **video call
URLs**. When a match is found, only the extracted URL (not the full message) is POSTed
to a configurable endpoint (default: `https://jamulus.live/`). Everything that doesn't
match is discarded — no full message content is forwarded. The receiving web app is
**jamfan22** at https://github.com/mcfnord/jamfan22/ — check that repo for the expected
POST format. Must fail silently if the endpoint is unreachable.

### Recording banner API
The server already exposes an RPC/API. Extend it with a method to toggle the red
RECORDING banner on all connected clients — the same banner shown when the server itself
is recording. This allows an external recording tool to signal participants that recording
is in progress. No new protocol needed; extend the existing server API surface.

### Resilience requirement
All calls to remote URLs (blocklist, ASN lookup proxy, chat reporting) must tolerate
network failures silently. The binary must never crash or block due to a failed remote
call. Test infrastructure must verify that endpoint failure does not prevent normal
operation.

## Branch strategy
`main` tracks upstream exactly — no custom commits. `jamfan` is rebased onto `main`
(history rewrite is acceptable). Pull upstream into `main`, then `git rebase main jamfan`.
