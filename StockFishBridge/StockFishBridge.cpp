#include "pch.h"
#include <windows.h>
#include <string>
#include <fstream>

static std::string g_runtimePath; // set by SetStockfishPath

extern "C" __declspec(dllexport) void __cdecl SetStockfishPath(const char* path)
{
    if (path && path[0]) g_runtimePath = path;
    else g_runtimePath.clear();
}

static std::string GetModuleFolder()
{
    char buf[MAX_PATH] = { 0 };
    if (GetModuleFileNameA(NULL, buf, MAX_PATH) > 0) {
        std::string full(buf);
        size_t p = full.find_last_of("\\/");
        if (p != std::string::npos) return full.substr(0, p);
    }
    return std::string();
}

static std::string ReadSidecarFile()
{
    std::string folder = GetModuleFolder();
    if (folder.empty()) return std::string();
    std::string fname = folder + "\\stockfish_path.txt";
    std::ifstream ifs(fname);
    if (!ifs.is_open()) return std::string();
    std::string line;
    if (std::getline(ifs, line)) {
        size_t start = line.find_first_not_of(" \t\r\n");
        size_t end = line.find_last_not_of(" \t\r\n");
        if (start != std::string::npos && end != std::string::npos) return line.substr(start, end - start + 1);
    }
    return std::string();
}

static std::string ResolveStockfishPath()
{
    if (!g_runtimePath.empty()) return g_runtimePath;

    char envbuf[MAX_PATH] = { 0 };
    if (GetEnvironmentVariableA("STOCKFISH_PATH", envbuf, MAX_PATH) > 0) {
        std::string s(envbuf);
        if (!s.empty()) return s;
    }

    std::string file = ReadSidecarFile();
    if (!file.empty()) return file;

    return std::string("C:\\ChessEngines\\Stockfish\\stockfish.exe");
}
extern "C" __declspec(dllexport) const char* __cdecl GetBestMove(const char* fen)
{
    thread_local static char result[64];
    strcpy_s(result, "error");
    const char* stockfishPath = "C:\\ChessEngines\\Stockfish\\stockfish.exe";

    // Quick path check
    DWORD attrs = GetFileAttributesA(stockfishPath);
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        strcpy_s(result, "bad_path");
        return result;
    }

    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
    HANDLE childStdInRead = NULL, childStdInWrite = NULL;
    HANDLE childStdOutRead = NULL, childStdOutWrite = NULL;
    PROCESS_INFORMATION pi = { 0 };

    auto cleanup = [&]() {
        if (childStdInWrite) { CloseHandle(childStdInWrite);  childStdInWrite = NULL; }
        if (childStdInRead) { CloseHandle(childStdInRead);   childStdInRead = NULL; }
        if (childStdOutWrite) { CloseHandle(childStdOutWrite); childStdOutWrite = NULL; }
        if (childStdOutRead) { CloseHandle(childStdOutRead);  childStdOutRead = NULL; }
        if (pi.hProcess) { CloseHandle(pi.hProcess);      pi.hProcess = NULL; }
        if (pi.hThread) { CloseHandle(pi.hThread);       pi.hThread = NULL; }
        };

    if (!CreatePipe(&childStdInRead, &childStdInWrite, &sa, 0)) { strcpy_s(result, "pipe_fail"); cleanup(); return result; }
    if (!CreatePipe(&childStdOutRead, &childStdOutWrite, &sa, 0)) { strcpy_s(result, "pipe_fail"); cleanup(); return result; }

    // Parent keeps childStdInWrite and childStdOutRead: mark those as NON-INHERITABLE
    SetHandleInformation(childStdInWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(childStdOutRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = { 0 };
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = childStdInRead;    // child's stdin
    si.hStdOutput = childStdOutWrite;  // child's stdout
    si.hStdError = childStdOutWrite;  // child's stderr

    // Prepare writable, quoted command line buffer
    char cmdline[MAX_PATH + 8] = { 0 };
    _snprintf_s(cmdline, sizeof(cmdline), "\"%s\"", stockfishPath);

    BOOL ok = CreateProcessA(
        NULL,           // lpApplicationName
        cmdline,        // lpCommandLine (writable)
        NULL,
        NULL,
        TRUE,           // inherit handles
        CREATE_NO_WINDOW,
        NULL,
        NULL,
        &si,
        &pi
    );

    if (!ok) {
        strcpy_s(result, "launch_failed");
        cleanup();
        return result;
    }

    // Parent doesn't need the child's ends
    CloseHandle(childStdInRead); childStdInRead = NULL;
    CloseHandle(childStdOutWrite); childStdOutWrite = NULL;

    auto writeCommand = [&](const std::string& s) -> bool {
        DWORD written = 0;
        return WriteFile(childStdInWrite, s.c_str(), (DWORD)s.size(), &written, NULL) && written == s.size();
        };

    auto readAvailableAppend = [&](std::string& acc) -> bool {
        DWORD avail = 0;
        if (!PeekNamedPipe(childStdOutRead, NULL, 0, NULL, &avail, NULL)) return false;
        if (avail == 0) return true;
        char buf[2048];
        DWORD r = 0;
        if (!ReadFile(childStdOutRead, buf, (DWORD)sizeof(buf) - 1, &r, NULL)) return false;
        if (r == 0) return true;
        buf[r] = '\0';
        acc += buf;
        return true;
        };

    // UCI handshake
    if (!writeCommand("uci\n")) { strcpy_s(result, "write_fail"); goto FINAL; }
    {
        std::string acc;
        ULONGLONG start = GetTickCount64();
        while ((GetTickCount64() - start) < 3000) {
            if (!readAvailableAppend(acc)) break;
            if (acc.find("uciok") != std::string::npos) break;
            Sleep(10);
        }
    }

    if (!writeCommand("isready\n")) { strcpy_s(result, "write_fail"); goto FINAL; }
    {
        std::string acc;
        ULONGLONG start = GetTickCount64();
        while ((GetTickCount64() - start) < 3000) {
            if (!readAvailableAppend(acc)) break;
            if (acc.find("readyok") != std::string::npos) break;
            Sleep(10);
        }
    }

    // Send position and go
    {
        std::string cmd = "position fen ";
        cmd += fen;
        cmd += "\n";
        cmd += "go movetime 3000\n";
        if (!writeCommand(cmd)) { strcpy_s(result, "write_fail"); goto FINAL; }
    }

    // Read until bestmove or timeout
    {
        std::string acc;
        ULONGLONG start = GetTickCount64();
        bool found = false;
        while ((GetTickCount64() - start) < 10000) {
            if (!readAvailableAppend(acc)) break;
            size_t pos = acc.find("bestmove ");
            if (pos != std::string::npos) {
                size_t startMove = pos + 9;
                size_t endMove = acc.find_first_of(" \r\n", startMove);
                std::string move = (endMove == std::string::npos) ? acc.substr(startMove) : acc.substr(startMove, endMove - startMove);
                if (move.size() >= 2 && move.size() < (sizeof(result) - 1)) {
                    strcpy_s(result, move.c_str());
                }
                else {
                    strcpy_s(result, "bad_move");
                }
                found = true;
                break;
            }
            Sleep(10);
        }
        if (!found) strcpy_s(result, "no_move");
    }

    // Ask engine to quit cleanly
    writeCommand("quit\n");
    WaitForSingleObject(pi.hProcess, 500);

FINAL:
    cleanup();
    return result;
}
