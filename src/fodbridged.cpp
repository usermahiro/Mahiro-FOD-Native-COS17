// fodbridged - kernel FOD key reader -> @fodhook socket
//
// Watches the Transsion touchscreen for EV_KEY code 195 (finger on display)
// and forwards DOWN/UP ('1'/'0') to libfodhook.so inside com.android.systemui
// through the abstract unix socket @fodhook.
//
// It optionally mirrors the state to /sys/kernel/tran_display/lcm_hbm_state
// for kernels that expose it (the hwcomposer layer remains the primary HBM
// path, exactly like the smali patch this replaces).

#include <android/log.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stddef.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <thread>

#define LOG_TAG "fodbridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

#define FOD_KEYCODE 195
static const char* TOUCH_NAME = "mtk-tpd";
static const char* g_devOverride = nullptr;
static int g_sockFd = -1;

static int connectSocket() {
    if (g_sockFd >= 0) return g_sockFd;
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0) return -1;
    sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    const char* nm = "fodhook";
    a.sun_path[0] = '\0';
    memcpy(a.sun_path + 1, nm, strlen(nm));
    socklen_t len = offsetof(sockaddr_un, sun_path) + 1 + strlen(nm);
    if (connect(s, (sockaddr*)&a, len) != 0) {
        close(s);
        return -1;
    }
    g_sockFd = s;
    LOGI("connected to @fodhook socket");
    return g_sockFd;
}

static void sendSocket(char b) {
    int fd = connectSocket();
    if (fd >= 0) {
        if (write(fd, &b, 1) != 1) {
            close(fd);
            g_sockFd = -1;
        }
    }
}

static void writeHbm(int val) {
    static int hbmFd = -2;
    if (hbmFd == -2) {
        hbmFd = open("/sys/kernel/tran_display/lcm_hbm_state", O_WRONLY | O_CLOEXEC);
    }
    if (hbmFd >= 0) {
        char buf[8];
        int len = snprintf(buf, sizeof(buf), "%d\n", val);
        pwrite(hbmFd, buf, len, 0);
    }
}

static bool supportsFodKey(int fd) {
    uint8_t bits[(KEY_MAX + 8) / 8] = {0};
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) < 0) return false;
    return (bits[FOD_KEYCODE / 8] & (1 << (FOD_KEYCODE % 8))) != 0;
}

static int openTouch() {
    if (g_devOverride) return open(g_devOverride, O_RDONLY | O_CLOEXEC);
    DIR* d = opendir("/dev/input");
    if (!d) return -1;
    int byName = -1;
    int byCap = -1;
    while (dirent* e = readdir(d)) {
        if (strncmp(e->d_name, "event", 5) != 0) continue;
        std::string path = std::string("/dev/input/") + e->d_name;
        int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        char name[128] = {0};
        if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0) name[0] = 0;
        if (strcmp(name, TOUCH_NAME) == 0) {
            LOGI("touch device %s (%s)", path.c_str(), name);
            byName = fd;
            break;
        }
        if (byCap < 0 && supportsFodKey(fd)) {
            byCap = fd;
            LOGI("FOD key capable device %s (%s)", path.c_str(), name);
            continue;
        }
        close(fd);
    }
    closedir(d);
    if (byName >= 0) return byName;
    return byCap;
}

static void onEdge(int st) {
    char c = st ? '1' : '0';
    sendSocket(c);
    writeHbm(st ? 1 : 0);
    LOGI("finger %s (sent to @fodhook)", st ? "DOWN" : "UP");
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--dev") && i + 1 < argc) g_devOverride = argv[++i];
    }
    LOGI("fodbridged started (native FOD bridge)");

    int last = -1;
    for (;;) {
        int fd;
        while ((fd = openTouch()) < 0) { sleep(2); }
        input_event ev;
        for (;;) {
            ssize_t n = read(fd, &ev, sizeof(ev));
            if (n == (ssize_t)sizeof(ev)) {
                if (ev.type == EV_KEY && ev.code == FOD_KEYCODE && ev.value != 2) {
                    int st = ev.value ? 1 : 0;
                    if (st != last) { last = st; onEdge(st); }
                }
            } else if (n < 0 && errno == EINTR) {
                continue;
            } else {
                LOGI("touch read ended (n=%zd errno=%d), reopening", n, errno);
                break;
            }
        }
        close(fd);
        if (last == 1) { last = 0; onEdge(0); }
        sleep(1);
    }
    return 0;
}
