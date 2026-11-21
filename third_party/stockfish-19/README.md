# Stockfish 19 (external opponent binary location)

Koi does not compile, link, or vendor Stockfish C++ source code. This folder
serves as the conventional location for a local, untracked external Stockfish 19
executable used by optional match, training-data, and Elo tools:

- Windows x64 path:
  `third_party/stockfish-19/stockfish-windows-x86-64-universal/stockfish/stockfish-windows-x86-64-universal.exe`

Binaries (`*.exe`) are ignored by `.gitignore`; download or build Stockfish
upstream and place the executable locally without committing binaries or
upstream source trees.

Stockfish is licensed under the GNU General Public License, version 3. See
[https://stockfishchess.org/](https://stockfishchess.org/) for upstream releases
and licensing terms.
