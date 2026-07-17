# Jamulus — Agent Instructions

Real-time networked music app; one Qt/C++ qmake binary is both client and server. Stability outranks everything — it runs live performances.

Docs: [CONTRIBUTING.md](CONTRIBUTING.md) (process, style, licensing — applies to agents without exception), [COMPILING.md](COMPILING.md), [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md), [docs/DEPLOY.md](docs/DEPLOY.md), [docs/JAMULUS_PROTOCOL.md](docs/JAMULUS_PROTOCOL.md), [SECURITY.md](SECURITY.md) (vulnerabilities → team@jamulus.io, never a public issue).

## Rules

- Smallest possible PRs: one logical change each — one PR per new file, per independent fix, per typo. Each PR must stand alone and be reviewable in minutes. Never bundle refactors or reformatting.
- Agree on features in a GitHub issue or Discussion before coding.
- Never block the real-time audio path (sound callbacks, socket thread, server mix timer).
- All network input is untrusted: bounds-check everything, drop malformed packets silently.
- Don't edit generated files (`moc_*`, `ui_*`, `qrc_*`, `*.qm`) or `libs/` (`libs/oboe` is a submodule).

## Build, verify, submit

- Build: `qmake && make`. Headless server: `qmake "CONFIG+=headless serveronly" && make`.
- No test suite: run a headless server, connect a client to `127.0.0.1`, exercise the change, and say in the PR what you tested.
- `make clang_format` before committing. Qt ≥ 5.12.2 APIs only (`QT_VERSION_CHECK` guards), C++11, keep all platforms building.
- Strings: `tr ( "Hi, %1!" ).arg ( name )` — substitution, never concatenation.
- JSON-RPC changes: regenerate `docs/JSON-RPC.md` with `tools/generate_json_rpc_docs.py`.
- PR description: `CHANGELOG: <one sentence>` or `CHANGELOG: SKIP`; never edit the `ChangeLog` file. New dependencies or build changes: add `AUTOBUILD: Please build all targets`.
