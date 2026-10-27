/*---------------------------------------------------------*\
| DebugTrace.cpp                                            |
|                                                           |
|   Always-on diagnostic trace written to its own file,     |
|   independent of the log level and verbosity settings.    |
|                                                           |
|   This file is part of the OpenRGB project                |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>
#include <algorithm>
#include "DebugTrace.h"
#include "filesystem.h"

#ifdef __linux__
#include <dirent.h>
#include <pwd.h>
#include <grp.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
#endif

/*---------------------------------------------------------*\
| Limits                                                    |
|   Rotate an existing file larger than ROTATE_BYTES when a |
|   process opens it, and stop writing once one process has |
|   written MAX_BYTES_PER_PROCESS so a runaway loop cannot  |
|   fill the disk.                                          |
\*---------------------------------------------------------*/
static const long           DEBUGTRACE_ROTATE_BYTES         = 10L * 1024L * 1024L;
static const long           DEBUGTRACE_MAX_BYTES_PER_PROCESS= 20L * 1024L * 1024L;

static std::recursive_mutex debugtrace_mutex;
static FILE*                debugtrace_file                 = nullptr;
static bool                 debugtrace_open_attempted       = false;
static bool                 debugtrace_capped               = false;
static long                 debugtrace_written              = 0;

static const char* debugtrace_level_names[] =
{
    "FATAL",
    "ERROR",
    "WARNING",
    "INFO",
    "VERBOSE",
    "DEBUG",
    "TRACE",
    "DIALOG"
};

static std::string DebugTraceFilePath()
{
    const char* xdg  = getenv("XDG_CONFIG_HOME");
    const char* home = getenv("HOME");

    std::string dir;

    if(xdg != nullptr && xdg[0] != '\0')
    {
        dir = std::string(xdg) + "/OpenRGB";
    }
    else if(home != nullptr && home[0] != '\0')
    {
        dir = std::string(home) + "/.config/OpenRGB";
    }
    else
    {
        dir = "/tmp";
    }

    std::error_code ec;
    filesystem::create_directories(filesystem::path(dir), ec);

    return(dir + "/OpenRGB-debug.log");
}

static FILE* DebugTraceOpen()
{
    if(debugtrace_open_attempted)
    {
        return(debugtrace_file);
    }

    debugtrace_open_attempted = true;

    std::string path = DebugTraceFilePath();

    /*-----------------------------------------------------*\
    | Rotate a large existing file to .old                  |
    \*-----------------------------------------------------*/
    std::error_code ec;
    std::uintmax_t  size = filesystem::file_size(filesystem::path(path), ec);

    if(!ec && size > (std::uintmax_t)DEBUGTRACE_ROTATE_BYTES)
    {
        filesystem::rename(filesystem::path(path), filesystem::path(path + ".old"), ec);
    }

    debugtrace_file = fopen(path.c_str(), "a");

    if(debugtrace_file == nullptr)
    {
        /*-------------------------------------------------*\
        | Fall back to /tmp so the trace is never lost      |
        \*-------------------------------------------------*/
        debugtrace_file = fopen("/tmp/OpenRGB-debug.log", "a");
    }

    return(debugtrace_file);
}

static std::string DebugTracePrefix()
{
    char buf[128];

    /*-----------------------------------------------------*\
    | Wall clock time                                       |
    \*-----------------------------------------------------*/
    struct timespec rt;
    clock_gettime(CLOCK_REALTIME, &rt);

    struct tm local_tm;
    localtime_r(&rt.tv_sec, &local_tm);

    char wall[32];
    strftime(wall, sizeof(wall), "%Y-%m-%d %H:%M:%S", &local_tm);

    /*-----------------------------------------------------*\
    | Seconds since boot, comparable with the kernel log    |
    | (journalctl -o short-monotonic / dmesg)               |
    \*-----------------------------------------------------*/
    double since_boot = 0.0;

#ifdef __linux__
    struct timespec bt;
    clock_gettime(CLOCK_BOOTTIME, &bt);
    since_boot = (double)bt.tv_sec + ((double)bt.tv_nsec / 1e9);

    long pid = (long)getpid();
    long tid = (long)syscall(SYS_gettid);
#else
    long pid = 0;
    long tid = 0;
#endif

    snprintf(buf, sizeof(buf), "%s.%03ld [boot+%10.3f] [pid %ld tid %ld] ", wall, (long)(rt.tv_nsec / 1000000), since_boot, pid, tid);

    return(std::string(buf));
}

static void DebugTraceWriteLine(const std::string& text)
{
    std::lock_guard<std::recursive_mutex> guard(debugtrace_mutex);

    FILE* f = DebugTraceOpen();

    if(f == nullptr || debugtrace_capped)
    {
        return;
    }

    std::string line = DebugTracePrefix() + text + "\n";

    if(debugtrace_written + (long)line.size() > DEBUGTRACE_MAX_BYTES_PER_PROCESS)
    {
        std::string cap = DebugTracePrefix() + "[DebugTrace] Per-process size limit reached, further trace suppressed\n";
        fwrite(cap.data(), 1, cap.size(), f);
        fflush(f);
        debugtrace_capped = true;
        return;
    }

    fwrite(line.data(), 1, line.size(), f);
    fflush(f);

    debugtrace_written += (long)line.size();
}

void DebugTrace(const char* fmt, ...)
{
    va_list va;
    va_start(va, fmt);

    va_list va2;
    va_copy(va2, va);
    int len = vsnprintf(nullptr, 0, fmt, va);
    va_end(va);

    if(len < 0)
    {
        va_end(va2);
        return;
    }

    std::string text;
    text.resize((std::size_t)len);
    vsnprintf(&text[0], (std::size_t)len + 1, fmt, va2);
    va_end(va2);

    DebugTraceWriteLine(text);
}

void DebugTraceLogMessage(unsigned int level, const char* filename, int line, const std::string& text)
{
    const char* level_name = "?";

    if(level < (sizeof(debugtrace_level_names) / sizeof(debugtrace_level_names[0])))
    {
        level_name = debugtrace_level_names[level];
    }

    const char* base = filename;

    if(filename != nullptr)
    {
        const char* slash = strrchr(filename, '/');

        if(slash != nullptr)
        {
            base = slash + 1;
        }
    }

    DebugTrace("[LOG %s] %s (%s:%d)", level_name, text.c_str(), base ? base : "?", line);
}

std::string DebugHex(const unsigned char* buf, std::size_t len, std::size_t max_len)
{
    std::string out;
    char        tmp[4];

    if(buf == nullptr)
    {
        return("(null)");
    }

    std::size_t count = std::min(len, max_len);

    for(std::size_t i = 0; i < count; i++)
    {
        snprintf(tmp, sizeof(tmp), "%02X", buf[i]);

        if(i > 0)
        {
            out += ' ';
        }

        out += tmp;
    }

    if(len > max_len)
    {
        out += " ...";
    }

    return(out);
}

#ifdef __linux__
static std::string DebugTraceReadFile(const std::string& path)
{
    std::ifstream in(path);

    if(!in.is_open())
    {
        return("");
    }

    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    return(content);
}

static std::string DebugTraceOneLine(std::string text)
{
    std::replace(text.begin(), text.end(), '\n', ' ');

    while(!text.empty() && text.back() == ' ')
    {
        text.pop_back();
    }

    return(text);
}

static std::string DebugTraceUeventValue(const std::string& uevent, const std::string& key)
{
    std::size_t pos = 0;

    while(pos < uevent.size())
    {
        std::size_t end  = uevent.find('\n', pos);
        std::string line = uevent.substr(pos, (end == std::string::npos) ? std::string::npos : end - pos);

        if(line.compare(0, key.size() + 1, key + "=") == 0)
        {
            return(line.substr(key.size() + 1));
        }

        if(end == std::string::npos)
        {
            break;
        }

        pos = end + 1;
    }

    return("");
}

static std::string DebugTraceUserName(uid_t uid)
{
    struct passwd* pw = getpwuid(uid);

    if(pw != nullptr && pw->pw_name != nullptr)
    {
        return(std::string(pw->pw_name));
    }

    return(std::to_string((unsigned long)uid));
}

static std::string DebugTraceGroupName(gid_t gid)
{
    struct group* gr = getgrgid(gid);

    if(gr != nullptr && gr->gr_name != nullptr)
    {
        return(std::string(gr->gr_name));
    }

    return(std::to_string((unsigned long)gid));
}
#endif

void DebugTracePathAccess(const char* tag, const char* path)
{
#ifdef __linux__
    if(path == nullptr)
    {
        DebugTrace("[%s] access check: path is null", tag);
        return;
    }

    struct stat st;

    if(stat(path, &st) != 0)
    {
        int err = errno;
        DebugTrace("[%s] access check %s: stat failed: %s (errno %d)", tag, path, strerror(err), err);
        return;
    }

    int rw_ok  = access(path, R_OK | W_OK);
    int rw_err = (rw_ok == 0) ? 0 : errno;

    DebugTrace("[%s] access check %s: owner=%s group=%s mode=%04o read+write=%s%s%s",
               tag,
               path,
               DebugTraceUserName(st.st_uid).c_str(),
               DebugTraceGroupName(st.st_gid).c_str(),
               (unsigned int)(st.st_mode & 07777),
               (rw_ok == 0) ? "YES" : "NO",
               (rw_ok == 0) ? "" : " - ",
               (rw_ok == 0) ? "" : strerror(rw_err));
#else
    DebugTrace("[%s] access check %s: not implemented on this platform", tag, path ? path : "(null)");
#endif
}

void DebugTraceHidrawScan(const char* reason)
{
#ifdef __linux__
    DebugTrace("[HIDRAW] ---- Permission scan (%s) ----", reason);

    DIR* dir = opendir("/dev");

    if(dir == nullptr)
    {
        int err = errno;
        DebugTrace("[HIDRAW] Cannot open /dev: %s", strerror(err));
        return;
    }

    std::vector<std::string> nodes;
    struct dirent*           entry;

    while((entry = readdir(dir)) != nullptr)
    {
        if(strncmp(entry->d_name, "hidraw", 6) == 0)
        {
            nodes.push_back(entry->d_name);
        }
    }

    closedir(dir);

    std::sort(nodes.begin(), nodes.end(), [](const std::string& a, const std::string& b)
    {
        int na = atoi(a.c_str() + 6);
        int nb = atoi(b.c_str() + 6);
        return(na < nb);
    });

    unsigned int accessible   = 0;
    unsigned int inaccessible = 0;

    for(const std::string& node : nodes)
    {
        std::string dev_path = "/dev/" + node;
        std::string uevent   = DebugTraceReadFile("/sys/class/hidraw/" + node + "/device/uevent");
        std::string hid_id   = DebugTraceUeventValue(uevent, "HID_ID");
        std::string hid_name = DebugTraceUeventValue(uevent, "HID_NAME");

        struct stat st;

        if(stat(dev_path.c_str(), &st) != 0)
        {
            int err = errno;
            DebugTrace("[HIDRAW] %-10s stat failed: %s  (%s %s)", dev_path.c_str(), strerror(err), hid_id.c_str(), hid_name.c_str());
            continue;
        }

        int rw_ok  = access(dev_path.c_str(), R_OK | W_OK);
        int rw_err = (rw_ok == 0) ? 0 : errno;

        if(rw_ok == 0)
        {
            accessible++;
        }
        else
        {
            inaccessible++;
        }

        DebugTrace("[HIDRAW] %-12s owner=%s group=%s mode=%04o read+write=%s%s%s  id=%s name=\"%s\"",
                   dev_path.c_str(),
                   DebugTraceUserName(st.st_uid).c_str(),
                   DebugTraceGroupName(st.st_gid).c_str(),
                   (unsigned int)(st.st_mode & 07777),
                   (rw_ok == 0) ? "YES" : "NO",
                   (rw_ok == 0) ? "" : " - ",
                   (rw_ok == 0) ? "" : strerror(rw_err),
                   hid_id.c_str(),
                   hid_name.c_str());
    }

    DebugTrace("[HIDRAW] ---- %zu nodes, %u accessible, %u NOT accessible ----", nodes.size(), accessible, inaccessible);
#else
    DebugTrace("[HIDRAW] Permission scan (%s) not implemented on this platform", reason);
#endif
}

void DebugTraceStartup(int argc, char* argv[])
{
    DebugTrace("==================================================================");
    DebugTrace("[STARTUP] OpenRGB process start (build with DebugTrace)");

    std::string args;

    for(int i = 0; i < argc; i++)
    {
        if(i > 0)
        {
            args += ' ';
        }

        args += argv[i];
    }

    DebugTrace("[STARTUP] Command line: %s", args.c_str());
    DebugTrace("[STARTUP] Trace file: %s", DebugTraceFilePath().c_str());

#ifdef __linux__
    uid_t uid  = getuid();
    uid_t euid = geteuid();

    DebugTrace("[STARTUP] uid=%lu (%s) euid=%lu (%s) gid=%lu (%s)",
               (unsigned long)uid,  DebugTraceUserName(uid).c_str(),
               (unsigned long)euid, DebugTraceUserName(euid).c_str(),
               (unsigned long)getgid(), DebugTraceGroupName(getgid()).c_str());

    /*-----------------------------------------------------*\
    | Supplementary groups                                  |
    \*-----------------------------------------------------*/
    int ngroups = getgroups(0, nullptr);

    if(ngroups > 0)
    {
        std::vector<gid_t> groups((std::size_t)ngroups);
        ngroups = getgroups(ngroups, groups.data());

        std::string group_list;

        for(int i = 0; i < ngroups; i++)
        {
            if(i > 0)
            {
                group_list += ',';
            }

            group_list += DebugTraceGroupName(groups[(std::size_t)i]);
        }

        DebugTrace("[STARTUP] Supplementary groups: %s", group_list.c_str());
    }

    const char* env_names[] = { "HOME", "XDG_CONFIG_HOME", "XDG_RUNTIME_DIR", "XDG_SESSION_ID", "XDG_SESSION_TYPE", "DISPLAY", "WAYLAND_DISPLAY", "INVOCATION_ID" };

    for(const char* name : env_names)
    {
        const char* value = getenv(name);
        DebugTrace("[STARTUP] env %s=%s", name, value ? value : "(unset)");
    }

    /*-----------------------------------------------------*\
    | Login session state from systemd-logind.  uaccess     |
    | udev rules only grant device access to users with an  |
    | active local session.                                 |
    \*-----------------------------------------------------*/
    std::string user_state = DebugTraceReadFile("/run/systemd/users/" + std::to_string((unsigned long)uid));

    if(user_state.empty())
    {
        DebugTrace("[STARTUP] logind: no state file for uid %lu (user has NO login session yet)", (unsigned long)uid);
    }
    else
    {
        DebugTrace("[STARTUP] logind user STATE=%s SESSIONS=%s",
                   DebugTraceUeventValue(user_state, "STATE").c_str(),
                   DebugTraceUeventValue(user_state, "SESSIONS").c_str());
    }

    std::string seat_state = DebugTraceReadFile("/run/systemd/seats/seat0");

    DebugTrace("[STARTUP] logind seat0 ACTIVE=%s ACTIVE_UID=%s",
               DebugTraceUeventValue(seat_state, "ACTIVE").c_str(),
               DebugTraceUeventValue(seat_state, "ACTIVE_UID").c_str());

    std::string uptime = DebugTraceOneLine(DebugTraceReadFile("/proc/uptime"));
    DebugTrace("[STARTUP] /proc/uptime: %s", uptime.c_str());
#endif

    DebugTraceHidrawScan("process startup");
}
