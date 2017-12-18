#include <cassert>
#include <system_error>
#include <type_traits>
#include <Windows.h>
#include "win32util.h"

namespace win32 {

static_assert(std::is_same<detail::DWORD, ::DWORD>::value, "type mismatch");
static_assert(std::is_same<detail::HANDLE, ::HANDLE>::value, "type mismatch");
static_assert(std::is_same<detail::HMODULE, ::HMODULE>::value, "type mismatch");

static_assert(detail::INFINITE_ == INFINITE, "constant mismatch");


namespace {
const int unused_ = []() { assert(detail::invalid_handle() == INVALID_HANDLE_VALUE); return 0; }();
} // namespace


namespace detail {

void CloseHandleDeleter::operator()(handle h) { ::CloseHandle(h.h); }
void UnmapViewOfFileDeleter::operator()(void *ptr) { ::UnmapViewOfFile(ptr); }
void FreeLibraryDeleter::operator()(::HMODULE ptr) { ::FreeLibrary(ptr); }

void TerminateProcessDeleter::operator()(handle h)
{
	::TerminateProcess(h.h, 0);
	::CloseHandle(h.h);
}

} // namespace detail


void trap_error(const char *msg)
{
	std::error_code code{ static_cast<int>(::GetLastError()), std::system_category() };
	throw std::system_error{ code, msg };
}


MutexGuard::MutexGuard(::HANDLE handle, ::DWORD timeout) : m_handle{ handle }
{
	DWORD result = ::WaitForSingleObject(handle, timeout);
	switch (result) {
	case WAIT_OBJECT_0:
		return;
	case WAIT_ABANDONED:
		::SetLastError(ERROR_ABANDONED_WAIT_0);
		trap_error("remote process abandoned mutex");
		break;
	case WAIT_TIMEOUT:
		::SetLastError(ERROR_TIMEOUT);
		trap_error();
		break;
	case WAIT_FAILED:
		trap_error("failed to acquire mutex");
		break;
	default:
		::SetLastError(ERROR_UNIDENTIFIED_ERROR);
		trap_error("unknown error while waiting on mutex");
		break;
	}
}

MutexGuard::~MutexGuard() { ::ReleaseMutex(m_handle); }

} // namespace win32
