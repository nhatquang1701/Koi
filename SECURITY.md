# Security and Release Verification

## Verifying Official Release Downloads

Prebuilt archives are built exclusively from tagged commits on GitHub-hosted
CI runners (`windows-latest`, `ubuntu-24.04`, `macos-15`) and published with:

1. **GitHub SLSA Build Provenance Attestations** (`actions/attest-build-provenance`):
   Verify that an archive or executable came untouched from this repository's CI:
   ```bash
   gh attestation verify koi-engine-v1.1.6.zip --repo nhatquang1701/Koi
   ```
2. **SHA-256 Checksums**:
   Every release publishes `.sha256` sidecars alongside the archives and
   individual file hashes in `*.package.json`:
   ```powershell
   Get-FileHash .\koi-engine-v1.1.6.zip -Algorithm SHA256
   ```

## Anti-Malware and Binary Hardening

Official Windows builds are compiled cleanly with MSVC `/O2` (no packers, no
obfuscators, no network calls, no telemetry) and include:

- Embedded PE `VERSIONINFO` metadata (`CompanyName`, `ProductName`,
  `FileDescription`, `OriginalFilename`, `FileVersion`, source commit).
- Embedded application manifest declaring `<requestedExecutionLevel level="asInvoker"/>`
  (no administrator elevation).
- PE exploit-mitigation flags: Control Flow Guard (`/guard:cf` / `/GUARD:CF`),
  high-entropy ASLR (`/DYNAMICBASE` `/HIGHENTROPYVA`), DEP (`/NXCOMPAT`), and
  CET shadow-stack compatibility (`/CETCOMPAT`).
- Clean separation of process-launch APIs: `koi-engine-avx2.exe` and
  `koi-engine-avx512.exe` do not link `CreateProcessW` or handle-duplication
  APIs. Only `koi-engine.exe` carries CPU auto-dispatch so older GUIs can launch
  the fastest variant automatically; if your security software prefers direct
  execution, register `koi-engine-avx2.exe` or `koi-engine-avx512.exe` directly
  in your chess GUI.

## Microsoft Defender / SmartScreen False Positives

Newly built unsigned open-source command-line binaries can occasionally receive
heuristic warnings (`SmartScreen` or `Wacatac`/`Bearfoos` ML detections) until
Windows Defender reputation builds. Before running any binary:

1. Verify its `SHA-256` checksum and `gh attestation verify` proof above.
2. Optionally inspect it on [VirusTotal](https://www.virustotal.com/) or build
   directly from source (see [README.md](README.md)).
3. Report confirmed false positives at
   [Microsoft Security Intelligence](https://www.microsoft.com/en-us/wdsi/filesubmission).

## Reporting Vulnerabilities

If you discover a security or memory-safety bug (for example in FEN/UCI parsing
or tablebase/network container loading), please report it privately via GitHub
Security Advisories or open an issue without publishing exploit payloads first.
