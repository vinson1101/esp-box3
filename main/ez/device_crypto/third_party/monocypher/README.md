# Monocypher Ed25519 source record

- Version: `4.0.3`
- Release archive: `https://monocypher.org/download/monocypher-4.0.3.tar.gz`
- Archive SHA-512:
  `40904ada5c7ee4f7741733e38b69a30a4b0561cbffba5ffe7c2dce16136d540251ec0d9056ff606510d3b5b708fb8a40db7e0870d4a0b2dc17ba2bfb880f8965`
- License: BSD-2-Clause OR CC0-1.0; see `LICENCE.md`.

The vendored files are unmodified release files. The optional Ed25519 implementation depends on Monocypher's
low-level EdDSA operations and secure wipe function, so the matching core source is compiled with function-section
garbage collection; unrelated algorithms are not referenced by the terminal API.

| File | SHA-256 |
|---|---|
| `LICENCE.md` | `5f8360e4c06ddcc584bdb4b210c6af824c4bb301e6a9a521869b6d90795ca4b3` |
| `monocypher.c` | `57eb914fc88136119bd41655cccb8c250048bf54d470540625186f8ab16f64be` |
| `monocypher.h` | `c494da712122da7ff679fdcf318a5317e84972b6c950fe9d896212947797facd` |
| `monocypher-ed25519.c` | `60fce3578fb00b00da96490653d993c4cb427b1e1be38183285c66e04d22cc18` |
| `monocypher-ed25519.h` | `abc4fad381879f5c29176ebe014b9189956b3dfe0a3e36459b6990bc57212380` |
