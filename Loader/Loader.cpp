// ArcHangel | Alpha - Loader — запускается рядом с HLTas.dll, ждёт hl.exe и подгружает в него DLL.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <string>

static HANDLE g_con = GetStdHandle(STD_OUTPUT_HANDLE);
enum {
    CYAN = FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY,
    MAGENTA = FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_INTENSITY,
    DIM = FOREGROUND_GREEN | FOREGROUND_BLUE,
    WHITE = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY
};

static void Say(WORD color, const wchar_t* fmt, ...)
{
    SetConsoleTextAttribute(g_con, color);
    va_list ap; va_start(ap, fmt); vwprintf(fmt, ap); va_end(ap);
    SetConsoleTextAttribute(g_con, WHITE);
}

static DWORD FindProcess(const wchar_t* exe)
{
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe = { sizeof(pe) };
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (_wcsicmp(pe.szExeFile, exe) == 0) { pid = pe.th32ProcessID; break; }
    CloseHandle(snap);
    return pid;
}

static bool HasModule(DWORD pid, const wchar_t* name)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W me = { sizeof(me) };
    bool found = false;
    for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me))
        if (_wcsicmp(me.szModule, name) == 0) { found = true; break; }
    CloseHandle(snap);
    return found;
}

static bool Inject(DWORD pid, const std::wstring& dll)
{
    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
        PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!proc) { Say(MAGENTA, L"Не удалось открыть процесс (ошибка %lu)\n", GetLastError()); return false; }

    SIZE_T bytes = (dll.size() + 1) * sizeof(wchar_t);
    void* mem = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    bool ok = false;
    if (mem && WriteProcessMemory(proc, mem, dll.c_str(), bytes, nullptr)) {
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        FARPROC fn = k32 ? GetProcAddress(k32, "LoadLibraryW") : nullptr;
        HANDLE th = fn ? CreateRemoteThread(proc, nullptr, 0, (LPTHREAD_START_ROUTINE)fn, mem, 0, nullptr) : nullptr;
        if (th) {
            WaitForSingleObject(th, 10000);
            DWORD code = 0;
            GetExitCodeThread(th, &code);
            ok = code != 0;
            CloseHandle(th);
        }
    }
    if (mem) VirtualFreeEx(proc, mem, 0, MEM_RELEASE);
    CloseHandle(proc);
    return ok;
}

int wmain()
{
    SetConsoleTitleW(L"ArcHangel | Alpha - loader");
    Say(CYAN, L"\n  ========================================\n");
    Say(MAGENTA, L"     ArcHangel | Alpha   -   loader\n");
    Say(CYAN, L"  ========================================\n\n");

    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring dll = self;
    dll = dll.substr(0, dll.find_last_of(L"\\/") + 1) + L"HLTas.dll";

    if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Say(MAGENTA, L"  Не найден HLTas.dll рядом с загрузчиком:\n  %s\n\n", dll.c_str());
        Say(DIM, L"  Нажми Enter, чтобы выйти...");
        (void)getwchar();
        return 1;
    }

    Say(DIM, L"  Жду запуска hl.exe... (запусти Half-Life через Steam)\n");
    DWORD pid = 0;
    while (!(pid = FindProcess(L"hl.exe"))) Sleep(500);
    Say(CYAN, L"  hl.exe найден (PID %lu). Жду загрузки игры...\n", pid);

    // ждём, пока игра подгрузит движок и клиентскую часть
    for (int i = 0; i < 600; ++i) {
        if (HasModule(pid, L"client.dll") && HasModule(pid, L"opengl32.dll")) break;
        Sleep(200);
    }

    if (HasModule(pid, L"HLTas.dll")) {
        Say(MAGENTA, L"  HLTas.dll уже загружена в игру.\n");
    }
    else if (Inject(pid, dll)) {
        Say(CYAN, L"  Готово! В игре нажми Insert, чтобы открыть меню.\n");
    }
    else {
        Say(MAGENTA, L"  Не получилось подгрузить DLL. Проверь, что игра 32-битная,\n  а HLTas.dll собрана в Release x86.\n");
    }

    Say(DIM, L"\n  Нажми Enter, чтобы закрыть это окно...");
    (void)getwchar();
    return 0;
}