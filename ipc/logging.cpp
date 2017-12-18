#pragma once

#include <atomic>
#include <cstdio>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <system_error>
#include "ipc_client.h"
#include "logging.h"

#undef ipc_filename
#undef ipc_log
#undef ipc_wlog
#undef ipc_log_current_exception

namespace {

#ifdef _DEBUG
std::atomic<void (*)(const char *, va_list)> g_log_handler{ ipc_log_stderr };
std::atomic<void (*)(const wchar_t *, va_list)> g_wlog_handler{ ipc_wlog_stderr };
#else
std::atomic<void (*)(const char *, va_list)> g_log_handler{};
std::atomic<void (*)(const wchar_t *, va_list)> g_wlog_handler{};
#endif

} // namespace


const char *ipc_filename(const char *path)
{
	if (auto s = std::strrchr(path, '/'))
		return s + 1;
	else if (auto s = std::strrchr(path, '\\'))
		return s + 1;
	else
		return path;
}

void ipc_set_log_handler(void (*func)(const char *, va_list), void (*wfunc)(const wchar_t *, va_list))
{
	g_log_handler = func;
	g_wlog_handler = wfunc;
}

void ipc_log_stderr(const char *fmt, va_list va)
{
	std::vfprintf(stderr, fmt, va);
}

void ipc_wlog_stderr(const wchar_t *fmt, va_list va)
{
	std::vfwprintf(stderr, fmt, va);
}

void ipc_log(const char *fmt, ...)
{
	va_list va;
	va_start(va, fmt);
	auto func = g_log_handler.load();
	if (func)
		func(fmt, va);
	va_end(va);
}

void ipc_wlog(const wchar_t *fmt, ...)
{
	va_list va;
	va_start(va, fmt);
	auto func = g_wlog_handler.load();
	if (func)
		func(fmt, va);
	va_end(va);
}

void ipc_log_current_exception()
{
	try {
		throw;
	} catch (const ipc_client::IPCError &e) {
		ipc_log("IPC error: %s\n", e.what());

		if (e.cause()) {
			ipc_log("cause: ");

			try {
				std::rethrow_exception(e.cause());
			} catch (...) {
				ipc_log_current_exception();
			}
		}
	} catch (const std::system_error &e) {
		ipc_log("system error %d: %s\n", e.code().value(), e.what());
	} catch (const std::exception &e) {
		ipc_log("%s: %s\n", typeid(e).name(), e.what());
	} catch (...) {
		ipc_log("unknown exception\n");
	}
}
