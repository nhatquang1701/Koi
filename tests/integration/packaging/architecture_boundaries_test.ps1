#!/usr/bin/env pwsh
# Architecture-boundary regression for the 1.0.0 baseline.
#
# This test freezes the architecture that should stay valid until 2.0.0:
#  * SearchService's public header must not expose transposition-table internals;
#  * public headers may only include an allow-listed set of detail/ utilities;
#  * only the UCI controller (plus main.cpp) may include the controller header;
#  * engine core sources must not contain tooling/test data or dead modules;
#  * reliability fuzzers live in the test tree, not artifacts;
#  * the architecture contract is documented.
$ErrorActionPreference = 'Stop'

$scriptDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $scriptDirectory '../../..'))
$failures = [System.Collections.Generic.List[string]]::new()

function Fail([string]$Message) {
    $script:failures.Add($Message)
}

function Read-RepositoryFile([string]$RelativePath) {
    return Get-Content -LiteralPath (Join-Path $repositoryRoot $RelativePath) -Raw
}

# 0. The embedding service header must not pull private detail headers.
$serviceHeader = Read-RepositoryFile 'src/koi/search_service.hpp'
if ($serviceHeader -match '#include\s+"koi/detail/') {
    Fail 'search_service.hpp must not include private detail headers'
}

# 1. SearchService must not leak the transposition-table definition.
if ((Read-RepositoryFile 'src/koi/search_service.hpp') -match '#include\s+"koi/transposition_table\.hpp"') {
    Fail 'search_service.hpp must not include transposition_table.hpp (use hash_types.hpp)'
}

# 2. Public headers may only include allow-listed detail/ utilities.
$allowedDetailIncludes = @{
    'src/koi/game_state.hpp' = @('koi/detail/no_init_array.hpp')
    'src/koi/search_service.hpp' = @('koi/detail/search_constants.hpp')
}
$publicHeaders = Get-ChildItem -LiteralPath (Join-Path $repositoryRoot 'src/koi') -File -Filter '*.hpp'
foreach ($header in $publicHeaders) {
    $relative = [System.IO.Path]::GetRelativePath($repositoryRoot, $header.FullName).Replace('\', '/')
    $content = Get-Content -LiteralPath $header.FullName -Raw
    foreach ($match in [regex]::Matches($content, '#include\s+"(koi/detail/[^"]+)"')) {
        $include = $match.Groups[1].Value
        $allowed = $allowedDetailIncludes.ContainsKey($relative) -and
            $allowedDetailIncludes[$relative] -contains $include
        if (-not $allowed) {
            Fail "public header $relative includes private detail header $include"
        }
    }
}

# 3. Only the UCI controller itself may include its header within src/.
$controllerIncludes = Get-ChildItem -LiteralPath (Join-Path $repositoryRoot 'src') -Recurse -File |
    Where-Object { $_.Extension -in '.hpp', '.cpp' } |
    Where-Object { (Get-Content -LiteralPath $_.FullName -Raw) -match '#include\s+"koi/uci_controller\.hpp"' }
foreach ($file in $controllerIncludes) {
    $relative = [System.IO.Path]::GetRelativePath($repositoryRoot, $file.FullName).Replace('\', '/')
    if ($relative -notin @('src/koi/uci_controller.cpp', 'src/main.cpp')) {
        Fail "unexpected include of uci_controller.hpp in $relative"
    }
}

# 3b. The UCI controller header is compile-firewalled: debug logging and the
# Syzygy implementation must not be pulled in; both are behind forward
# declarations held by the controller implementation.
$uciHeader = Read-RepositoryFile 'src/koi/uci_controller.hpp'
if ($uciHeader -match '#include\s*<fstream>') {
    Fail 'uci_controller.hpp must not include <fstream> (debug log is detail::DebugLog)'
}
if ($uciHeader -match '#include\s+"koi/syzygy_tablebase\.hpp"') {
    Fail 'uci_controller.hpp must forward-declare SyzygyTablebase instead of including its header'
}
if ($uciHeader -notmatch 'class DebugLog;') {
    Fail 'uci_controller.hpp must forward-declare detail::DebugLog'
}

# 3c. The UCI option catalog is private detail data; the controller consumes it
# instead of defining its own copy.
$optionTablePath = Join-Path $repositoryRoot 'src/koi/detail/uci_option_table.hpp'
if (-not (Test-Path -LiteralPath $optionTablePath)) {
    Fail 'src/koi/detail/uci_option_table.hpp is missing'
} else {
    $optionTable = Get-Content -LiteralPath $optionTablePath -Raw
    if ($optionTable -notmatch 'array<UciOptionDescriptor, 28>') {
        Fail 'the option catalog must declare the 28-entry table'
    }
    $controllerSource = Read-RepositoryFile 'src/koi/uci_controller.cpp'
    if ($controllerSource -match 'enum class UciOptionKind') {
        Fail 'uci_controller.cpp must not redefine the option catalog enums'
    }
    if ($controllerSource -match 'kUciOptions\s*\{\{') {
        Fail 'uci_controller.cpp must not define the option table itself'
    }
}

# 3d. Synthetic NNUE fixtures are test support, not engine code.
$nnueSource = Read-RepositoryFile 'src/koi/nnue.cpp'
if ($nnueSource -match 'NnueNetwork::synthetic') {
    Fail 'src/koi/nnue.cpp must not define synthetic NNUE fixture factories'
}
if (-not (Test-Path -LiteralPath (Join-Path $repositoryRoot 'tests/support/nnue_synthetic.cpp'))) {
    Fail 'tests/support/nnue_synthetic.cpp (synthetic fixtures) is missing'
}

# 3e. The bounded root-fallback policy is its own detail unit; the driver owns
# flow and must not reintroduce the scanners inline.
foreach ($required in @('src/koi/detail/root_fallback.hpp', 'src/koi/detail/root_fallback.cpp')) {
    if (-not (Test-Path -LiteralPath (Join-Path $repositoryRoot $required))) {
        Fail "$required is missing"
    }
}
$runnerSource = Read-RepositoryFile 'src/koi/detail/search_runner.cpp'
if ($runnerSource -match 'int short_fallback_evaluate\(') {
    Fail 'search_runner.cpp must not define the fallback evaluation helpers'
}
if ($runnerSource -notmatch '#include "koi/detail/root_fallback\.hpp"') {
    Fail 'search_runner.cpp must include detail/root_fallback.hpp'
}

# 3f. Stateless FEN syntax validation is its own detail unit.
foreach ($required in @('src/koi/detail/fen_validation.hpp', 'src/koi/detail/fen_validation.cpp')) {
    if (-not (Test-Path -LiteralPath (Join-Path $repositoryRoot $required))) {
        Fail "$required is missing"
    }
}
$gameStateSource = Read-RepositoryFile 'src/koi/game_state.cpp'
if ($gameStateSource -match 'bool valid_piece_placement\(') {
    Fail 'game_state.cpp must not define FEN syntax helpers'
}
if ($gameStateSource -notmatch '#include "koi/detail/fen_validation\.hpp"') {
    Fail 'game_state.cpp must include detail/fen_validation.hpp'
}

# 4. Engine core sources must not contain dead/tooling translation units.
$cmake = Read-RepositoryFile 'CMakeLists.txt'
$coreMatch = [regex]::Match($cmake, '(?s)set\(KOI_CORE_SOURCES(.*?)\n\)')
if (-not $coreMatch.Success) {
    Fail 'could not find KOI_CORE_SOURCES in CMakeLists.txt'
} else {
    $coreSources = $coreMatch.Groups[1].Value
    if ($coreSources -notmatch 'detail/debug_log\.cpp') {
        Fail 'KOI_CORE_SOURCES must contain detail/debug_log.cpp'
    }
    foreach ($forbidden in @('move_chooser.cpp', 'strength_suite.cpp')) {
        if ($coreSources -match [regex]::Escape($forbidden)) {
            Fail "KOI_CORE_SOURCES must not contain $forbidden"
        }
    }
}
if (-not (Test-Path -LiteralPath (Join-Path $repositoryRoot 'src/koi/move_chooser.hpp'))) {
    # Expected: the dead chooser was removed; nothing to do.
} else {
    Fail 'src/koi/move_chooser.hpp should not exist (dead code)'
}

# 5. Reliability fuzzers live in the test tree, not under artifacts.
$fuzzers = @('fen_fuzz.py', 'move_fuzz.py', 'option_fuzz.py', 'go_fuzz.py', 'command_fuzz.py')
foreach ($fuzzer in $fuzzers) {
    if (-not (Test-Path -LiteralPath (Join-Path $repositoryRoot "tests/python/reliability/$fuzzer"))) {
        Fail "missing reliability fuzzer tests/python/reliability/$fuzzer"
    }
}
$artifactFuzzers = Get-ChildItem -LiteralPath (Join-Path $repositoryRoot 'artifacts') -Recurse -File -Filter '*_fuzz.py' -ErrorAction SilentlyContinue
if ($artifactFuzzers.Count -ne 0) {
    Fail "reliability fuzzers must live under tests/, found: $($artifactFuzzers.FullName -join ', ')"
}

# 6. The architecture contract is documented for the 1.0.0 baseline.
$architecture = Read-RepositoryFile 'docs/ARCHITECTURE.md'
foreach ($required in @('Architecture contract (1.0.0)', 'frozen for 1.0.0')) {
    if ($architecture -notmatch [regex]::Escape($required)) {
        Fail "docs/ARCHITECTURE.md is missing '$required'"
    }
}

if ($failures.Count -ne 0) {
    $failures | ForEach-Object { Write-Output "FAIL: $_" }
    throw "architecture boundary violations: $($failures.Count)"
}
Write-Output 'PASS architecture boundaries'
