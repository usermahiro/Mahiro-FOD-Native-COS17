// fodinject

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <dirent.h>
#include <elf.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <android/log.h>

#define LOG_TAG "fodinject"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static const char* TARGET = "com.android.systemui";
static const char* SOPATH = "/system/lib64/libfodhook.so";
static const char* SOPATH_EXT = "/system_ext/lib64/libfodhook.so";

struct regs64 { uint64_t regs[31]; uint64_t sp; uint64_t pc; uint64_t pstate; };

static int findPid(const char* name) {
    DIR* d = opendir("/proc");
    if (!d) return -1;
    struct dirent* e; int pid = -1;
    while ((e = readdir(d))) {
        int p = atoi(e->d_name);
        if (p <= 0) continue;
        char path[64], buf[256];
        snprintf(path, sizeof(path), "/proc/%d/cmdline", p);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) continue;
        buf[n] = 0;
        if (strcmp(buf, name) == 0) { pid = p; break; }
    }
    closedir(d);
    return pid;
}

// lowest mapped address of a library (soname substring) in a process's maps
static uintptr_t libBase(int pid, const char* soname) {
    char path[64];
    if (pid == 0) snprintf(path, sizeof(path), "/proc/self/maps");
    else snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    char line[512]; uintptr_t base = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, soname)) { base = (uintptr_t)strtoull(line, nullptr, 16); break; }
    }
    fclose(f);
    return base;
}

static bool alreadyInjected(int pid) {
    char path[64]; snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    FILE* f = fopen(path, "r"); if (!f) return false;
    char line[512]; bool yes = false;
    while (fgets(line, sizeof(line), f)) { if (strstr(line, "libfodhook.so")) { yes = true; break; } }
    fclose(f);
    return yes;
}

static bool getRegs(int pid, regs64* r) {
    struct iovec iov = { r, sizeof(*r) };
    return ptrace(PTRACE_GETREGSET, pid, (void*)NT_PRSTATUS, &iov) == 0;
}
static bool setRegs(int pid, regs64* r) {
    struct iovec iov = { r, sizeof(*r) };
    return ptrace(PTRACE_SETREGSET, pid, (void*)NT_PRSTATUS, &iov) == 0;
}

static bool writeMem(int pid, uintptr_t dst, const void* src, size_t len) {
    struct iovec liov = { (void*)src, len };
    struct iovec riov = { (void*)dst, len };
    return process_vm_writev(pid, &liov, 1, &riov, 1, 0) == (ssize_t)len;
}

static bool readMem(int pid, uintptr_t src, void* dst, size_t len) {
    struct iovec liov = { dst, len };
    struct iovec riov = { (void*)src, len };
    return process_vm_readv(pid, &liov, 1, &riov, 1, 0) == (ssize_t)len;
}

// remote-call a function: set x0..x2, pc=func, lr=0 (return-trap -> SIGSEGV), run.
static uint64_t remoteCall(int pid, uintptr_t func, uint64_t a0, uint64_t a1, uint64_t a2, regs64 saved) {
    regs64 r = saved;
    r.regs[0] = a0;
    r.regs[1] = a1;
    r.regs[2] = a2;
    r.regs[30] = 0;                     // lr = 0 -> fault on return, we catch it
    r.pc = func;
    r.sp = (saved.sp - 0x800) & ~0xFUL; // scratch + 16-byte aligned
    if (!setRegs(pid, &r)) { LOGE("setregs(call) failed"); return 0; }
    ptrace(PTRACE_CONT, pid, 0, 0);
    int st = 0;
    while (true) {
        if (waitpid(pid, &st, 0) <= 0) break;
        if (WIFSTOPPED(st)) {
            int sig = WSTOPSIG(st);
            if (sig == SIGSEGV) break;
            ptrace(PTRACE_CONT, pid, 0, (sig == SIGSTOP || sig == SIGTRAP) ? 0 : sig);
        } else {
            break;
        }
    }
    // expect stop by SIGSEGV (returned to 0). Read x0.
    regs64 out; getRegs(pid, &out);
    return out.regs[0];
}

static bool inject(int pid) {
    const char* targetSo = SOPATH;
    if (access(targetSo, R_OK) != 0 && access(SOPATH_EXT, R_OK) == 0) {
        targetSo = SOPATH_EXT;
    }

    regs64 saved;
    if (ptrace(PTRACE_ATTACH, pid, 0, 0) != 0) { LOGE("attach failed"); return false; }
    int st; waitpid(pid, &st, 0);
    if (!getRegs(pid, &saved)) { LOGE("getregs failed"); ptrace(PTRACE_DETACH, pid, 0, 0); return false; }

    // Prefer __loader_dlopen(path, flags, caller_addr): lets us pass an explicit
    // caller so the remote linker picks a namespace that permits /system/lib64.
    // Caller MUST be in the default/system namespace (e.g. libandroid_runtime.so),
    // NOT libc.so (which is in the isolated APEX runtime namespace).
    void* h = dlopen("libdl.so", RTLD_NOW);
    uintptr_t l_loader = h ? (uintptr_t)dlsym(h, "__loader_dlopen") : 0;
    uintptr_t l_dlerror = h ? (uintptr_t)dlsym(h, "dlerror") : 0;
    uintptr_t r_loader = 0, caller = 0, r_dlerror = 0;
    if (l_loader) {
        uintptr_t l_lk = libBase(0, "linker64"), r_lk = libBase(pid, "linker64");
        uintptr_t r_sys = libBase(pid, "libandroid_runtime.so");
        if (!r_sys) r_sys = libBase(pid, "libnativehelper.so");
        if (!r_sys) r_sys = libBase(pid, "app_process");
        if (!r_sys) r_sys = libBase(pid, "/system/lib64/");
        if (!r_sys) r_sys = libBase(pid, "/libc.so");

        if (l_lk && r_lk && r_sys) {
            r_loader = l_loader - l_lk + r_lk;
            caller = r_sys + 0x20; // address inside default/system namespace
            if (l_dlerror) r_dlerror = l_dlerror - l_lk + r_lk;
        }
    }
    // Fallback: plain dlopen via libdl base-diff.
    uintptr_t l_dlopen = (uintptr_t)&dlopen;
    uintptr_t l_base = libBase(0, "/libdl.so");
    uintptr_t r_base = libBase(pid, "/libdl.so");
    uintptr_t r_dlopen = (l_base && r_base) ? (l_dlopen - l_base + r_base) : 0;
    if (!r_dlerror && l_dlerror && l_base && r_base) {
        r_dlerror = l_dlerror - l_base + r_base;
    }
    LOGI("loader=%p caller=%p dlopen=%p dlerror=%p", (void*)r_loader, (void*)caller, (void*)r_dlopen, (void*)r_dlerror);

    // write the .so path into remote scratch (below saved sp)
    uintptr_t strAddr = (saved.sp - 0x400) & ~0xFUL;
    if (!writeMem(pid, strAddr, targetSo, strlen(targetSo) + 1)) { LOGE("writeMem path failed"); ptrace(PTRACE_DETACH, pid, 0, 0); return false; }

    uint64_t handle = 0;
    if (r_loader && caller) {
        handle = remoteCall(pid, r_loader, strAddr, 2 /*RTLD_NOW*/, caller, saved);
        LOGI("remote __loader_dlopen returned %p", (void*)handle);
    }
    if (!handle && r_dlopen) {
        handle = remoteCall(pid, r_dlopen, strAddr, 2, 0, saved);
        LOGI("remote dlopen returned %p", (void*)handle);
    }

    if (!handle && r_dlerror) {
        uint64_t errPtr = remoteCall(pid, r_dlerror, 0, 0, 0, saved);
        if (errPtr) {
            char errBuf[512] = {0};
            if (readMem(pid, errPtr, errBuf, sizeof(errBuf) - 1)) {
                LOGE("remote dlerror: %s", errBuf);
            }
        }
    }

    // restore original state and let SystemUI continue
    setRegs(pid, &saved);
    ptrace(PTRACE_DETACH, pid, 0, 0);

    return handle != 0;
}

int main(int argc, char** argv) {
    // Test mode: fodinject <pid> <sopath>  -> inject once, exit (for validation).
    if (argc >= 3) {
        int pid = atoi(argv[1]);
        SOPATH = argv[2];
        LOGI("test inject %s into pid %d", SOPATH, pid);
        bool ok = inject(pid);
        LOGI("test inject %s", ok ? "OK" : "FAILED");
        printf("inject %s\n", ok ? "OK" : "FAILED");
        return ok ? 0 : 1;
    }

    // wait for boot + SystemUI + fingerprint HAL
    for (int i = 0; i < 60; i++) {
        char v[16] = {0};
        FILE* p = popen("getprop sys.boot_completed", "r");
        if (p) { if (fgets(v, sizeof(v), p)) {} pclose(p); }
        if (v[0] == '1') break;
        sleep(2);
    }
    sleep(10);

    while (true) {
        int pid = findPid(TARGET);
        if (pid > 0 && !alreadyInjected(pid)) {
            LOGI("injecting into %s (pid %d)", TARGET, pid);
            bool ok = inject(pid);
            LOGI("inject %s", ok ? "OK" : "FAILED");
        }
        sleep(5);   // re-check; re-inject if SystemUI restarted
    }
    return 0;
}
