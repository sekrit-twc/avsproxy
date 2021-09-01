#pragma once

#ifndef IPC_WIN32UTIL_H_
#define IPC_WIN32UTIL_H_

#include <cstddef>
#include <cstdint>
#include <memory>

struct HINSTANCE__;

namespace win32 {
namespace detail {
typedef unsigned long DWORD;

typedef void *HANDLE;
typedef HINSTANCE__ *HMODULE;

constexpr DWORD INFINITE_ = ~static_cast<DWORD>(0);
constexpr uintptr_t INVALID_HANDLE_VALUE_ = ~static_cast<uintptr_t>(0);

inline HANDLE invalid_handle() { return reinterpret_cast<HANDLE>(INVALID_HANDLE_VALUE_); }

// Pointer class handling the two invalid states of HANDLE.
struct handle {
	HANDLE h;

	handle(HANDLE h = nullptr) : h{ h } {}

	explicit operator bool() const { return h && h != invalid_handle(); }

	friend bool operator==(handle lhs, handle rhs) { return lhs.h == rhs.h || (!lhs && !rhs); }
	friend bool operator==(handle lhs, std::nullptr_t rhs) { return !lhs; }
	friend bool operator==(std::nullptr_t lhs, handle rhs) { return !rhs; }

	friend bool operator!=(handle lhs, handle rhs) { return !(lhs == rhs); }
	friend bool operator!=(handle lhs, std::nullptr_t rhs) { return !!lhs; }
	friend bool operator!=(std::nullptr_t lhs, handle rhs) { return !!rhs; }
};

struct CloseHandleDeleter {
	typedef handle pointer;
	void operator()(handle h);
};

struct UnmapViewOfFileDeleter {
	void operator()(void *ptr);
};

struct FreeLibraryDeleter {
	void operator()(HMODULE ptr);
};

class TerminateProcessDeleter {
	typedef handle pointer;
	void operator()(handle h);
};

} // namespace detail


// Call GetLastError and throw a std::system_error.
[[noreturn]] inline void trap_error(const char *msg = "");

// Handle-based smart pointers.
typedef std::unique_ptr<void, detail::CloseHandleDeleter> unique_handle;
typedef std::unique_ptr<void, detail::UnmapViewOfFileDeleter> unique_file_view;
typedef std::unique_ptr<HINSTANCE__, detail::FreeLibraryDeleter> unique_module;

// Automatically terminates process.
typedef std::unique_ptr<void, detail::TerminateProcessDeleter> unique_process;

// std::lock_guard analogue for Win32 mutexes.
class MutexGuard {
	detail::HANDLE m_handle;
public:
	MutexGuard(detail::HANDLE h, detail::DWORD timeout = detail::INFINITE_);

	~MutexGuard();

	MutexGuard(const MutexGuard &) = delete;
	MutexGuard(MutexGuard &&) = delete;

	MutexGuard &operator=(const MutexGuard &) = delete;
	MutexGuard &operator=(MutexGuard &&) = delete;
};

} // namespace win32

#endif // IPC_WIN32UTIL_H_
