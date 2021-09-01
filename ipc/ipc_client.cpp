#include <cassert>
#include <cstdint>
#include <cwchar>
#include <condition_variable>
#include <new>
#include <string>
#include <utility>
#include <vector>
#include <Windows.h>
#include "ipc_client.h"
#include "ipc_commands.h"
#include "ipc_types.h"
#include "logging.h"

namespace ipc_client {

namespace {

constexpr uint32_t QUEUE_SIZE = 4096;
constexpr uint32_t SHMEM_SIZE = 256 * (1UL << 20);

std::wstring create_slave_command(const std::wstring &slave_path, ::HANDLE shmem_handle, uint32_t shmem_size)
{
#define FORMAT L"\"%s\" %u %u %u", slave_path.c_str(), ::GetCurrentProcessId(), HandleToULong(shmem_handle), shmem_size
	if (slave_path.empty() || slave_path.find(L'"') != std::wstring::npos || slave_path.back() == L'\\')
		throw IPCError{ "invalid characters in path" };

	std::wstring cmd(MAX_PATH, L'\0');

	while (std::swprintf(&cmd[0], cmd.size(), FORMAT) < 0) {
		cmd.resize(cmd.size() * 2);
	}

	return cmd;
#undef FORMAT
}

void wait_remote_process_write(::HANDLE event, ::HANDLE process)
{
	::HANDLE handles[2] = { event, process };
	::DWORD result = ::WaitForMultipleObjects(sizeof(handles) / sizeof(::HANDLE), handles, FALSE, INFINITE);

	switch (result) {
	case WAIT_OBJECT_0:
		return;
	case WAIT_OBJECT_0 + 1:
		throw IPCError{ "remote process terminated unexpectedly" };
	case WAIT_ABANDONED_0:
	case WAIT_ABANDONED_0 + 1:
		::SetLastError(ERROR_ABANDONED_WAIT_0);
		win32::trap_error("remote process abandoned event");
		break;
	case WAIT_TIMEOUT:
		::SetLastError(ERROR_TIMEOUT);
		win32::trap_error();
		break;
	case WAIT_FAILED:
		win32::trap_error("failed to wait for event");
		break;
	default:
		::SetLastError(ERROR_UNIDENTIFIED_ERROR);
		win32::trap_error("unknown error while waiting on event");
		break;
	}
}

void print_heap(const ipc::Heap *heap)
{
	const ipc::HeapNode *base = ipc::offset_to_pointer<const ipc::HeapNode>(heap, heap->buffer_offset);
	const ipc::HeapNode *node = base;

	while (true) {
		ipc_log("0x%08x - 0x%08x (%u): %s\n",
			ipc::pointer_to_offset(base, node),
			node->next_node_offset,
			node->next_node_offset - ipc::pointer_to_offset(base, node),
			(node->flags & ipc::HEAP_FLAG_ALLOCATED) ? "allocated" : "free");
		if (node->next_node_offset == ipc::NULL_OFFSET)
			break;
		node = ipc::offset_to_pointer<const ipc::HeapNode>(base, node->next_node_offset);
	};
}

} // namespace


IPCClient::IPCClient(bool master) :
	m_master_queue{},
	m_slave_queue{},
	m_heap{},
	m_remote_process{},
	m_master{ master },
	m_transaction_id{},
	m_kill_flag{}
{}

IPCClient::IPCClient(master_tag, const wchar_t *slave_path) : IPCClient{ true }
{
	::SECURITY_ATTRIBUTES inheritable_attributes{ sizeof(::SECURITY_ATTRIBUTES), nullptr, TRUE };

	// Allocate and map shared memory.
	ipc_log0("allocate shared memory\n");

	m_shmem_handle.reset(::CreateFileMappingW(INVALID_HANDLE_VALUE, &inheritable_attributes, PAGE_READWRITE, 0, SHMEM_SIZE, nullptr));
	if (!m_shmem_handle)
		win32::trap_error("error allocating IPC shared memory");

	m_shmem.reset(::MapViewOfFile(m_shmem_handle.get().h, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, SHMEM_SIZE));
	if (!m_shmem)
		win32::trap_error("error mapping shared memory");

	// Create synchronization events.
	ipc_log0("initialize Win32 objects\n");

	m_master_event.reset(::CreateEventW(&inheritable_attributes, FALSE, FALSE, nullptr));
	if (!m_master_event)
		win32::trap_error("error creating synchronization object");
	m_master_mutex.reset(::CreateMutexW(&inheritable_attributes, FALSE, nullptr));
	if (!m_master_mutex)
		win32::trap_error("error creating synchronization object");

	m_slave_event.reset(::CreateEventW(&inheritable_attributes, FALSE, FALSE, nullptr));
	if (!m_slave_event)
		win32::trap_error("error creating synchronization object");
	m_slave_mutex.reset(::CreateMutexW(&inheritable_attributes, FALSE, nullptr));
	if (!m_slave_mutex)
		win32::trap_error("error creating synchronization object");

	m_heap_mutex.reset(::CreateMutexW(&inheritable_attributes, FALSE, nullptr));
	if (!m_heap_mutex)
		win32::trap_error("error creating synchronization object");

	// Initialize IPC structures.
	ipc::SharedMemoryHeader *header = new (m_shmem.get()) ipc::SharedMemoryHeader{};
	header->size = SHMEM_SIZE;

	m_master_queue = new (ipc::offset_to_pointer<void>(header, sizeof(ipc::SharedMemoryHeader))) ipc::Queue{};
	m_master_queue->size = QUEUE_SIZE;
	m_master_queue->event_handle = HandleToULong(m_master_event.get().h);
	m_master_queue->mutex_handle = HandleToULong(m_master_mutex.get().h);

	m_slave_queue = new (ipc::offset_to_pointer<void>(m_master_queue, QUEUE_SIZE)) ipc::Queue{};
	m_slave_queue->size = QUEUE_SIZE;
	m_slave_queue->event_handle = HandleToULong(m_slave_event.get().h);
	m_slave_queue->mutex_handle = HandleToULong(m_slave_mutex.get().h);

	m_heap = new (ipc::offset_to_pointer<void>(m_slave_queue, QUEUE_SIZE)) ipc::Heap{};
	m_heap->size = SHMEM_SIZE - ipc::pointer_to_offset(header, m_heap);
	m_heap->mutex_handle = HandleToULong(m_heap_mutex.get().h);

	new (ipc::offset_to_pointer<void>(m_heap, m_heap->buffer_offset)) ipc::HeapNode{};

	header->master_queue_offset = ipc::pointer_to_offset(header, m_master_queue);
	header->slave_queue_offset = ipc::pointer_to_offset(header, m_slave_queue);
	header->heap_offset = ipc::pointer_to_offset(header, m_heap);

	// Start slave process.
	std::wstring slave_command = create_slave_command(slave_path, m_shmem_handle.get().h, SHMEM_SIZE);
	ipc_wlog(L"start slave process: %s\n", slave_command.c_str());

	::STARTUPINFO startup_info{ sizeof(::STARTUPINFO) };
	::PROCESS_INFORMATION process_info{};

#ifdef _DEBUG
  #define FLAGS CREATE_NEW_CONSOLE
#else
  #define FLAGS CREATE_NO_WINDOW
#endif
	if (!::CreateProcessW(nullptr, &slave_command[0], nullptr, nullptr, TRUE, FLAGS, nullptr, nullptr, &startup_info, &process_info))
		win32::trap_error("error starting slave process");
#undef FLAGS

	ipc_log("slave process pid: %u\n", process_info.dwProcessId);
	::CloseHandle(process_info.hThread);
	m_remote_process = process_info.hProcess;
}

IPCClient::IPCClient(slave_tag, ::HANDLE master_process, ::HANDLE shmem_handle, size_t shmem_size) : IPCClient{ false }
{
	ipc_log("open shared memory\n");

	if (shmem_size < sizeof(ipc::SharedMemoryHeader))
		throw IPCError{ "wrong shared memory size" };

	m_shmem_handle.reset(shmem_handle);
	m_shmem.reset(::MapViewOfFile(m_shmem_handle.get().h, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, shmem_size));
	if (!m_shmem)
		win32::trap_error("error mapping shared memory");

	ipc::SharedMemoryHeader *header = static_cast<ipc::SharedMemoryHeader *>(m_shmem.get());
	if (!ipc::check_fourcc(header->magic, "avsw"))
		throw IPCError{ "bad header in shared memory" };
	if (header->size != shmem_size)
		throw IPCError{ "wrong shared memory size" };
	if (header->version != ipc::VERSION)
		throw IPCError{ "IPC version mismatch" };
	if (header->slave_queue_offset > header->size - sizeof(ipc::Queue) ||
	    header->master_queue_offset > header->size - sizeof(ipc::Queue) ||
	    header->heap_offset > header->size - sizeof(ipc::Heap))
	{
		throw IPCError{ "pointer out of bounds" };
	}

	m_master_queue = ipc::offset_to_pointer<ipc::Queue>(header, header->master_queue_offset);
	if (!ipc::check_fourcc(m_master_queue->magic, "cmdq"))
		throw IPCError{ "bad queue header" };
	if (m_master_queue->size > header->size - header->master_queue_offset ||
	    m_master_queue->buffer_offset > m_master_queue->size - sizeof(ipc::Command))
	{
		throw IPCError{ "pointer out of bounds" };
	}

	m_slave_queue = ipc::offset_to_pointer<ipc::Queue>(header, header->slave_queue_offset);
	if (!ipc::check_fourcc(m_slave_queue->magic, "cmdq"))
		throw IPCError{ "bad queue header" };
	if (m_slave_queue->size > header->size - header->slave_queue_offset ||
	    m_slave_queue->buffer_offset > m_slave_queue->size - sizeof(ipc::Command))
	{
		throw IPCError{ "pointer out of bounds" };
	}

	m_heap = ipc::offset_to_pointer<ipc::Heap>(header, header->heap_offset);
	if (!ipc::check_fourcc(m_heap->magic, "heap"))
		throw IPCError{ "bad heap header" };
	if (m_heap->size > header->size - header->heap_offset ||
	    m_heap->buffer_offset > m_heap->size - sizeof(ipc::HeapNode))
	{
		throw IPCError{ "pointer out of bounds" };
	}

	m_master_event.reset(ULongToHandle(m_master_queue->event_handle));
	m_master_mutex.reset(ULongToHandle(m_master_queue->mutex_handle));
	m_slave_event.reset(ULongToHandle(m_slave_queue->event_handle));
	m_slave_mutex.reset(ULongToHandle(m_slave_queue->mutex_handle));
	m_heap_mutex.reset(ULongToHandle(m_heap->mutex_handle));

	m_remote_process = master_process;
}

IPCClient::~IPCClient()
{
	try {
		stop();
	} catch (...) {
		ipc_log_current_exception();
	}

	if (m_master) {
		ipc_log("terminate slave process\n");
		::Sleep(100);
		::TerminateProcess(m_remote_process, 0);
		::CloseHandle(m_remote_process);
	}
}

uint32_t IPCClient::next_transaction_id()
{
	uint32_t transaction_id;

	do {
		transaction_id = m_transaction_id++;
	} while (transaction_id == INVALID_TRANSACTION);

	return transaction_id;
}

void IPCClient::recv_thread_func()
{
	try {
		std::vector<unsigned char> command_buf;

		while (true) {
			if (m_kill_flag) {
				ipc_log0("exit receiver thread after kill flag\n");
				break;
			}

			wait_remote_process_write(recv_event(), m_remote_process);

			{
				win32::MutexGuard lock{ recv_mutex() };
				command_buf.resize(recv_queue()->buffer_usage);
				ipc::queue_read(recv_queue(), command_buf.data());
			}

			size_t pos = 0;
			while (pos < command_buf.size()) {
				if (command_buf.size() - pos < sizeof(ipc::Command))
					throw IPCError{ "pointer out of bounds" };

				const ipc::Command *raw_command = reinterpret_cast<const ipc::Command *>(command_buf.data() + pos);
				if (!ipc::check_fourcc(raw_command->magic, "cmdx"))
					throw IPCError{ "bad command header" };
				if (raw_command->size > command_buf.size() - pos)
					throw IPCError{ "pointer out of bounds" };

				ipc_log("received command type %d: %u => %u\n", raw_command->type, raw_command->response_id, raw_command->transaction_id);

				std::unique_ptr<Command> command = deserialize_command(raw_command);
				pos += raw_command->size;

				if (!command) {
					ipc_log0("failed to deserialize command type\n");
					continue;
				}

				callback_type callback;

				if (command->response_id() != INVALID_TRANSACTION) {
					std::lock_guard<std::mutex> lock{ m_worker_mutex };
					auto it = m_callbacks.find(command->response_id());

					if (it != m_callbacks.end()) {
						callback = std::move(it->second);
						m_callbacks.erase(it);
					}
				}

				if (callback) {
					ipc_log("invoke callback for original transaction %u\n", command->response_id());
					callback(std::move(command));
				} else if (m_default_cb) {
					m_default_cb(std::move(command));
				}
			}
		}
	} catch (...) {
		ipc_log0("exit receiver thread after exception\n");
		m_recv_exception = std::current_exception();
	}

	std::lock_guard<std::mutex> lock{ m_worker_mutex };

	for (const auto &cb : m_callbacks) {
		cb.second(nullptr);
	}
	if (m_default_cb)
		m_default_cb(nullptr);

	m_kill_flag = true;
}

void IPCClient::start(callback_type default_cb)
{
	assert(!m_recv_thread);
	assert(!m_kill_flag);

	::DWORD exit_code = STILL_ACTIVE;
	if (!::GetExitCodeProcess(m_remote_process, &exit_code))
		win32::trap_error("error polling remote process");
	if (exit_code != STILL_ACTIVE)
		throw IPCError{ "remote process exited" };

	m_default_cb = std::move(default_cb);

	ipc_log0("start IPC receiver thread\n");
	m_recv_thread = std::make_unique<std::thread>(&IPCClient::recv_thread_func, this);
}

void IPCClient::stop()
{
	if (!m_recv_thread)
		return;

	ipc_log0("stop IPC receiver thread\n");
	m_kill_flag = true;

	if (!::SetEvent(recv_event())) {
		try {
			win32::trap_error("error interrupting IPC receiver thread");
		} catch (...) {
			ipc_log_current_exception();
			std::terminate();
		}
	}

	m_recv_thread->join();
	m_recv_thread.reset();
	m_callbacks.clear();

	if (m_recv_exception) {
		ipc_log("rethrow exception from receiver thread\n");
		std::exception_ptr eptr = m_recv_exception;
		m_recv_exception = nullptr;
		std::rethrow_exception(eptr);
	}
}

uint32_t IPCClient::pointer_to_offset(void *ptr) const
{
	if (!ptr)
		return ipc::NULL_OFFSET;

	return ipc::pointer_to_offset(ipc::offset_to_pointer<void>(m_heap, m_heap->buffer_offset), ptr);
}

void *IPCClient::offset_to_pointer(uint32_t off) const
{
	if (off == ipc::NULL_OFFSET)
		return nullptr;

	if (off > m_heap->size - m_heap->buffer_offset)
		throw IPCError{ "pointer out of bounds" };

	return ipc::offset_to_pointer<void>(ipc::offset_to_pointer<void>(m_heap, m_heap->buffer_offset), off);
}

void *IPCClient::allocate(size_t size)
{
	if (size > static_cast<uint32_t>(INT32_MAX))
		throw IPCError{ "cannot allocate more than 2 GB" };

	win32::MutexGuard lock{ m_heap_mutex.get().h };
	ipc::HeapNode *node = ipc::heap_alloc(m_heap, static_cast<uint32_t>(size));
	if (!node) {
		ipc_log("heap full, could not allocate %zu bytes\n", size);
		print_heap(m_heap);
		throw IPCHeapFull{ (m_heap->size - m_heap->buffer_offset) - m_heap->buffer_usage, size };
	}

	return ipc::offset_to_pointer<void>(node, sizeof(ipc::HeapNode));
}

void IPCClient::deallocate(void *ptr)
{
	if (!ptr)
		return;

	ipc::HeapNode *node = reinterpret_cast<ipc::HeapNode *>(static_cast<unsigned char *>(ptr) - sizeof(ipc::HeapNode));
	if (!ipc::check_fourcc(node->magic, "memz"))
		throw IPCError{ "pointer not a heap block" };

	win32::MutexGuard lock{ m_heap_mutex.get().h };
	ipc::heap_free(m_heap, node);
}

void IPCClient::send_async(std::unique_ptr<Command> command, callback_type cb)
{
	uint32_t transaction_id = INVALID_TRANSACTION;

	if (cb) {
		transaction_id = next_transaction_id();
		command->set_transaction_id(transaction_id);
	}

	{
		std::lock_guard<std::mutex> lock{ m_worker_mutex };

		if (m_recv_exception) {
			stop(); // Will throw.
			return; // Not reached.
		}
		if (m_kill_flag) {
			if (cb)
				cb(nullptr);
			return;
		}

		if (cb)
			m_callbacks[transaction_id] = std::move(cb);
	}

	std::vector<unsigned char> data(command->serialized_size());
	command->serialize(data.data());

	try {
		ipc_log("async send command type %d: %u\n", command->type(), transaction_id);
		{
			win32::MutexGuard lock_guard{ send_mutex() };
			ipc::queue_write(send_queue(), data.data(), static_cast<uint32_t>(data.size()));
		}
		if (!::SetEvent(send_event()))
			win32::trap_error("error setting event");
	} catch (...) {
		if (transaction_id != INVALID_TRANSACTION) {
			try {
				std::lock_guard<std::mutex> lock{ m_worker_mutex };
				m_callbacks.erase(transaction_id);
			} catch (...) {
				std::terminate();
			}
		}

		throw IPCError{ std::current_exception(), "error sending command" };
	}
}

std::unique_ptr<Command> IPCClient::send_sync(std::unique_ptr<Command> command)
{
	std::condition_variable cond;
	std::mutex mutex;

	assert(std::this_thread::get_id() != m_recv_thread->get_id());
	ipc_log("sync send command type: %d\n", command->type());

	std::unique_ptr<Command> result;
	bool called = false;

	callback_type func = [&](std::unique_ptr<Command> c)
	{
		{
			std::lock_guard<std::mutex> lock{ mutex };
			result = std::move(c);
		}
		called = true;
		cond.notify_all();
	};

	std::unique_lock<std::mutex> lock{ mutex };
	send_async(std::move(command), std::move(func));
	cond.wait(lock, [&]() { return called; });

	return result;
}

} // namespace ipc_client
