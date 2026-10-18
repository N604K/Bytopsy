#pragma once

// Running console tools (wsl, docker, diskpart, DISM, pnputil, ...) without a window, and as
// administrator when they need it.

#include <atomic>
#include <cstdint>
#include <string>

// <System32>\exe
std::wstring SystemTool(const wchar_t* exe);
// Full path of exe found on PATH, or "".
std::wstring FindOnPath(const wchar_t* exe);
bool RunningElevated();

// Runs a tool without a window and collects what it prints (stdout and stderr). Returns the exit
// code, -1 if it couldn't start, -2 on timeout or cancel (the tool is ended then).
int RunTool(const std::wstring& app, const std::wstring& args, std::string& out, unsigned long timeoutMs,
            std::atomic<bool>* cancel = nullptr);

// Tool output as text: UTF-16 (wsl.exe), UTF-8 (Linux tools, docker) or the console code page
// (DISM, pnputil, diskpart writing to a pipe or file).
std::wstring DecodeToolOutput(const std::string& raw);
// The last non-empty line, e.g. the error a failed tool printed.
std::wstring LastLine(const std::string& raw);

struct AdminRun {
    int code = -1;
    std::string out;
    bool declined = false;  // the UAC prompt was declined
    std::wstring error;     // couldn't start
};
// Runs `cmd.exe /d /s /c <commandLine>` as administrator: directly if this process already is, else
// through a UAC prompt with the output collected in a temp log. Waits until it ends: the tools run
// this way (DISM, diskpart) must never be interrupted.
AdminRun RunAsAdmin(const std::wstring& commandLine);
