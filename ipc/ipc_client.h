#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include "win32util.h"

namespace ipc {

struct Queue;
struct Heap;

} // namespace ipc


namespace ipc_client {

class Command;

class IPCError : public std::runtime_error {
	std::exception_ptr m_cause;
public:
	using std::runtime_error::runtime_error;

	IPCError(std::exception_ptr cause, const std::string &what_arg) :
		std::runtime_error{ what_arg },
		m_cause{ cause }
	{}

	IPCError(std::exception_ptr cause, const char *what_arg) :
		std::runtime_error{ what_arg },
		m_cause{ cause }
	{}

	std::exception_ptr cause() const noexcept { return m_cause; }
};

class IPCHeapFull : public IPCError {
	size_t m_alloc;
	size_t m_free;
public:
	IPCHeapFull(size_t alloc, size_t free) : IPCError{ "heap full" }, m_alloc { alloc }, m_free{ free } {}

	size_t alloc() const { return m_alloc; }
	size_t free() const { return m_free; }
};


class IPCClient {
public:
	typedef std::function<void(std::unique_ptr<Command>)> callback_type;
private:
	struct master_tag {};
	struct slave_tag {};

	// IPC control structures.
	win32::unique_handle m_shmem_handle;
	win32::unique_file_view m_shmem;

	ipc::Queue *m_master_queue;
	win32::unique_handle m_master_event;
	win32::unique_handle m_master_mutex;

	ipc::Queue *m_slave_queue;
	win32::unique_handle m_slave_event;
	win32::unique_handle m_slave_mutex;

	ipc::Heap *m_heap;
	win32::unique_handle m_heap_mutex;

	win32::detail::HANDLE m_remote_process;
	bool m_master;

	// Transaction state.
	std::unordered_map<uint32_t, callback_type> m_callbacks;
	callback_type m_default_cb;
	std::mutex m_worker_mutex;
	std::atomic_uint32_t m_transaction_id;
	std::atomic_bool m_kill_flag;

	std::unique_ptr<std::thread> m_recv_thread;
	std::exception_ptr m_recv_exception;

	ipc::Queue *send_queue() const { return m_master ? m_master_queue : m_slave_queue; }
	win32::detail::HANDLE send_event() const { return m_master ? m_master_event.get().h : m_slave_event.get().h; }
	win32::detail::HANDLE send_mutex() const { return m_master ? m_master_mutex.get().h : m_slave_mutex.get().h; }

	ipc::Queue *recv_queue() const { return m_master ? m_slave_queue : m_master_queue; }
	win32::detail::HANDLE recv_event() const { return m_master ? m_slave_event.get().h : m_master_event.get().h; }
	win32::detail::HANDLE recv_mutex() const { return m_master ? m_slave_mutex.get().h : m_master_mutex.get().h; }

	explicit IPCClient(bool master);

	uint32_t next_transaction_id();

	void recv_thread_func();
public:
	static master_tag master() { return{}; }
	static slave_tag slave() { return{}; }

	// Allocate IPC context and start child process.
	IPCClient(master_tag, const wchar_t *slave_path);

	// Connect to master process.
	IPCClient(slave_tag, win32::detail::HANDLE master_process, win32::detail::HANDLE shmem_handle, size_t shmem_size);

	IPCClient(const IPCClient &) = delete;
	IPCClient(IPCClient &&) = delete;

	~IPCClient();

	IPCClient &operator=(const IPCClient &) = delete;
	IPCClient &operator=(IPCClient &&) = delete;

	// Begin receiving commands. The client can only be started once.
	void start(callback_type default_cb);

	// Stop receiving commands. If a fatal communication error had previously
	// occurred, any exception generated will be raised here.
	void stop();

	// Heap interface.
	uint32_t pointer_to_offset(void *ptr) const;
	void *offset_to_pointer(uint32_t off) const;

	void *allocate(size_t size);
	void deallocate(void *ptr);

	// Send a command with an optional callback. The callback will be invoked
	// from the command receiver thread. Raises any prior exceptions.
	void send_async(std::unique_ptr<Command> command, callback_type cb = nullptr);

	// Send a command and wait for the result. Synchronous commands can not be
	// sent from the command receiver thread. Raises any prior exceptions.
	std::unique_ptr<Command> send_sync(std::unique_ptr<Command> command);
};

} // namespace ipc_client
