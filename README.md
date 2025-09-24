# StockfishBridge

A tiny Windows DLL that launches a Stockfish chess engine, speaks UCI, and returns the engine’s suggested best move for a given FEN string. Designed as a simple native helper for front‑ends (for example GFA‑Basic 32 demos) that need to query Stockfish without embedding the engine.

## Features

Exports two C ABI functions:

- `SetStockfishPath(const char*)` — set engine executable path at runtime.
- `GetBestMove(const char* fen) -> const char*` — returns Stockfish’s best move for the supplied FEN (or an error code).

Resolves engine path in this order:

1. runtime `SetStockfishPath` call
2. environment variable `STOCKFISH_PATH`
3. sidecar file `stockfish_path.txt` located next to the executable
4. built-in fallback path `C:\ChessEngines\Stockfish\stockfish.exe`

Uses UCI handshake (`uci` / `isready`) and issues `position fen ... + go movetime 3000` to produce a best move.

Simple, single-file native implementation suitable for packaging with a small GUI or script.

## Requirements

- Windows (desktop)
- Visual Studio 2019/2022 (or any build environment capable of producing a Win32 DLL)
- A Stockfish executable (`stockfish.exe`) available on the machine

## Build

1. Create a Win32 DLL project in Visual Studio (or add this source to your existing native project).
2. Add the provided `.cpp` source to the project (it contains `SetStockfishPath` and `GetBestMove`).
3. Build in Release mode and note the output folder containing `StockfishBridge.dll`.
4. Place `StockfishBridge.dll` next to your front-end executable (or on PATH) so the host can load it.

**Hints:**

- Build both x86 and x64 variants to match 32/64-bit hosts.
- If you ship binaries, include checksums and the license/disclaimer.

## Usage (from a host program)

Ensure the host can call Cdecl, `extern "C"` exports:

- Call `SetStockfishPath` once at startup to override other resolution methods (optional).
- Call `GetBestMove` with a FEN string; it returns a pointer to an internal thread-local C string containing either a move (e.g., `e2e4`) or an error code.

**Example exported signatures:**

```cpp
extern "C" __declspec(dllexport) void __cdecl SetStockfishPath(const char* path);
extern "C" __declspec(dllexport) const char* __cdecl GetBestMove(const char* fen);
