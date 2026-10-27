/*---------------------------------------------------------*\
| DebugTrace.h                                              |
|                                                           |
|   Always-on diagnostic trace written to its own file,     |
|   independent of the log level and verbosity settings.    |
|                                                           |
|   File: $XDG_CONFIG_HOME/OpenRGB/OpenRGB-debug.log        |
|         (or $HOME/.config/OpenRGB/OpenRGB-debug.log)      |
|                                                           |
|   This file is part of the OpenRGB project                |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

#include <cstddef>
#include <string>

#if defined(__GNUC__) || defined(__clang__)
#define DEBUGTRACE_PRINTF_FORMAT(fmt_idx, arg_idx) __attribute__((format(printf, fmt_idx, arg_idx)))
#else
#define DEBUGTRACE_PRINTF_FORMAT(fmt_idx, arg_idx)
#endif

/*---------------------------------------------------------*\
| Append one line to the debug trace file                   |
\*---------------------------------------------------------*/
void        DebugTrace(const char* fmt, ...) DEBUGTRACE_PRINTF_FORMAT(1, 2);

/*---------------------------------------------------------*\
| Write the process header (pid, user, session, arguments)  |
| and a permission scan of all HID nodes                    |
\*---------------------------------------------------------*/
void        DebugTraceStartup(int argc, char* argv[]);

/*---------------------------------------------------------*\
| Log owner, mode, ACL-effective access and device name of  |
| every /dev/hidraw* node                                   |
\*---------------------------------------------------------*/
void        DebugTraceHidrawScan(const char* reason);

/*---------------------------------------------------------*\
| Log whether the current process can read/write a path     |
\*---------------------------------------------------------*/
void        DebugTracePathAccess(const char* tag, const char* path);

/*---------------------------------------------------------*\
| Hex dump helper (first max_len bytes)                     |
\*---------------------------------------------------------*/
std::string DebugHex(const unsigned char* buf, std::size_t len, std::size_t max_len = 24);

/*---------------------------------------------------------*\
| Name of the current log level, used by the LogManager     |
| mirror                                                    |
\*---------------------------------------------------------*/
void        DebugTraceLogMessage(unsigned int level, const char* filename, int line, const std::string& text);
