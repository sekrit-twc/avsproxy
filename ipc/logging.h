#pragma once

#ifndef IPC_LOGGING_H_
#define IPC_LOGGING_H_

#include <stdarg.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

const char *ipc_filename(const char *path);

void ipc_set_log_handler(void (*func)(const char *, va_list), void (*wfunc)(const wchar_t *, va_list));

void ipc_log_stderr(const char *fmt, va_list va);
void ipc_wlog_stderr(const wchar_t *fmt, va_list va);

void ipc_log(const char *fmt, ...);
void ipc_wlog(const wchar_t *fmt, ...);

void ipc_log_current_exception();

#ifndef _DEBUG
  #define ipc_filename(x) ""
#endif

#define ipc_log(x, ...) (ipc_log)("[%s @ %s:%d] " x, __func__, ipc_filename(__FILE__), __LINE__, __VA_ARGS__)
#define ipc_log0(x) (ipc_log)("[%s @ %s:%d] " x, __func__, ipc_filename(__FILE__), __LINE__)

#define ipc_wlog(x, ...) (ipc_wlog)(L"[%S @ %S:%d] " x, __func__, ipc_filename(__FILE__), __LINE__, __VA_ARGS__)
#define ipc_wlog0(x) (ipc_wlog)("L[%S @ %S:%d] " x, __func__, ipc_filename(__FILE__), __LINE__)

#define ipc_log_current_exception() do { ipc_log0(""); (ipc_log_current_exception)(); } while (0)

#ifdef __cplusplus
} // extern "C"
#endif

#endif /* IPC_LOGGING_H_ */
