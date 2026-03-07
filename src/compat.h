#pragma once

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
    #include <io.h>
    #include <direct.h>
    #include <process.h>

    #define popen _popen
    #define pclose _pclose
    #define getcwd _getcwd

    #include <sys/stat.h>
    #ifndef S_ISDIR
        #define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
    #endif
    #ifndef S_ISREG
        #define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
    #endif
#else
    #include <sys/mman.h>
    #include <sys/wait.h>
    #include <unistd.h>
    #include <signal.h>
    #include <glob.h>
#endif

#include <cstdlib>
#include <cstdint>

inline void* jitAlloc(size_t size) {
#ifdef _WIN32
    return VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    return mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
}

inline void jitProtect(void* ptr, size_t size) {
#ifdef _WIN32
    DWORD old;
    VirtualProtect(ptr, size, PAGE_EXECUTE_READ, &old);
#else
    mprotect(ptr, size, PROT_READ | PROT_EXEC);
#endif
}

inline void jitFree(void* ptr, size_t size) {
#ifdef _WIN32
    (void)size;
    VirtualFree(ptr, 0, MEM_RELEASE);
#else
    munmap(ptr, size);
#endif
}

inline const char* devNull() {
#ifdef _WIN32
    return "NUL";
#else
    return "/dev/null";
#endif
}
