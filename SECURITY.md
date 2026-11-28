# Security Policy

## Supported Versions

Security fixes are applied to the latest minor release line and the active development branch (`main`).

| Version | Supported |
| :--- | :--- |
| `1.2.x` | Yes |
| `< 1.2.0` | No |

## Reporting a Vulnerability

Please **do not** report security vulnerabilities through public GitHub issues.

Instead, open a private report using [GitHub Security Advisories](https://github.com/nhatquang1701/Koi/security/advisories/new). Include:

- Koi version and build commit (`koi --version`)
- Operating system and architecture
- Minimal reproducer (crafted FEN, UCI transcript, malformed `.nnue` network, Polyglot `.bin` book, or Syzygy tablebase path)
- Expected vs. observed behavior (crash, sanitizer report, out-of-bounds read/write, hang)

## Scope

Koi is designed to fail closed on untrusted inputs. Reports are especially welcome for memory safety, denial-of-service, or path-handling bugs in:

- UCI / XBoard / USI / UCCI protocol parsing and bounded line reading
- FEN and move-list validation
- `.nnue` binary container parsing and verification
- Polyglot `.bin` opening book loading
- Syzygy `.rtbw` / `.rtbz` discovery, magic validation, and probing
- CPU variant auto-dispatch (`koi` launcher)
