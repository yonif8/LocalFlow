# Portable Ed25519 release verification

LocalFlow compiles the portable Ed25519 implementation distributed by Sparkle
2.9.6. The source archive is pinned to Sparkle commit
`ac2def288cbff5cfc7df3ffef6abdf45b72bcb0a` and SHA-256
`49c528efd86801a378ba7196da3ac9722a1165fbf4813ed76c9d636ad054fb9e`.

Upstream: <https://github.com/sparkle-project/Sparkle/tree/2.9.6/Vendor/ed25519-sparkle>

Only the portable C implementation is linked into the Windows release verifier
and its build-time signing utility. The original zlib license is reproduced in
`LICENSE.txt`.
