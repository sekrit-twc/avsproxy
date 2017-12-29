#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <Windows.h>
#include "ipc/ipc_client.h"
#include "ipc/ipc_commands.h"
#include "ipc/logging.h"
#include "ipc/win32util.h"
#include "avshost.h"

namespace {

class Session : ipc_client::CommandObserver {
	static std::FILE *s_log_file;

	ipc_client::IPCClient *m_client;
	avs::AvisynthHost m_avs;
	std::mutex m_mutex;
	std::condition_variable m_cond;
	std::deque<std::unique_ptr<ipc_client::Command>> m_queue;
	std::atomic_bool m_exit_flag;

	static void log_to_file(const char *fmt, va_list va)
	{
		std::vfprintf(s_log_file, fmt, va);
	}

	static void log_to_file(const wchar_t *fmt, va_list va)
	{
		std::vfwprintf(s_log_file, fmt, va);
	}

	int observe(std::unique_ptr<ipc_client::CommandSetLogFile> c) override
	{
		if (s_log_file)
			return 0;

		if ((s_log_file = _wfopen(c->arg().c_str(), L"w")))
			ipc_set_log_handler(log_to_file, log_to_file);

		return 0;
	}

#define AVS_OBSERVE(T) int observe(std::unique_ptr<T> c) override { return m_avs.dispatch(std::move(c)); }
	AVS_OBSERVE(ipc_client::CommandLoadAvisynth)
	AVS_OBSERVE(ipc_client::CommandNewScriptEnv)
	AVS_OBSERVE(ipc_client::CommandGetScriptVar)
	AVS_OBSERVE(ipc_client::CommandSetScriptVar)
	AVS_OBSERVE(ipc_client::CommandEvalScript)
	AVS_OBSERVE(ipc_client::CommandGetFrame)
	AVS_OBSERVE(ipc_client::CommandSetFrame)
#undef AVS_OBSERVE

	void queue_command(std::unique_ptr<ipc_client::Command> command)
	{
		std::unique_lock<std::mutex> lock{ m_mutex };

		if (command)
			m_queue.emplace_back(std::move(command));
		else
			m_exit_flag = true;

		lock.unlock();
		m_cond.notify_all();
	}

	void send_ack(uint32_t response_id)
	{
		if (response_id == ipc_client::INVALID_TRANSACTION)
			return;

		auto response = std::make_unique<ipc_client::CommandAck>();
		response->set_response_id(response_id);
		m_client->send_async(std::move(response));
	}

	void send_err(uint32_t response_id)
	{
		if (response_id == ipc_client::INVALID_TRANSACTION)
			return;

		auto response = std::make_unique<ipc_client::CommandErr>();
		response->set_response_id(response_id);
		m_client->send_async(std::move(response));
	}
public:
	explicit Session(ipc_client::IPCClient *client) :
		m_client{ client },
		m_avs{ client },
		m_exit_flag {}
	{}

	Session(const Session &) = delete;
	Session(Session &&) = delete;

	Session &operator=(const Session &) = delete;
	Session &operator=(Session &&) = delete;

	void run_loop()
	{
		m_client->start(std::bind(&Session::queue_command, this, std::placeholders::_1));

		while (true) {
			std::unique_lock<std::mutex> lock{ m_mutex };
			m_cond.wait(lock, [&]() { return m_exit_flag || !m_queue.empty(); });

			if (m_exit_flag) {
				ipc_log0("exit after broken connection\n");
				break;
			}

			std::unique_ptr<ipc_client::Command> command = std::move(m_queue.front());
			m_queue.pop_front();
			lock.unlock();

			uint32_t transaction_id = command->transaction_id();

			try {
				int ret = dispatch(std::move(command));
				if (!ret && transaction_id != ipc_client::INVALID_TRANSACTION)
					send_ack(transaction_id);
			} catch (const ipc_client::IPCError &) {
				throw;
			} catch (...) {
				send_err(transaction_id);
				ipc_log_current_exception();
			}
		}
	}
};

std::FILE *Session::s_log_file{};

} // namespace


int wmain(int argc, wchar_t **argv)
{
	for (int i = 0; i < argc; ++i) {
		ipc_wlog("argv[%d]: %s\n", i, argv[i]);
	}

	if (argc != 4)
		return 1;

	try {
		unsigned long parent_pid = std::stoul(argv[1]);
		::HANDLE shmem_handle = ULongToHandle(std::stoul(argv[2]));
		unsigned long shmem_size = std::stoul(argv[3]);

		win32::unique_handle parent_process{ ::OpenProcess(PROCESS_QUERY_INFORMATION | SYNCHRONIZE, FALSE, parent_pid) };
		if (!parent_process)
			win32::trap_error("error connecting to master process");

		auto client = std::make_unique<ipc_client::IPCClient>(ipc_client::IPCClient::slave(), parent_process.get().h, shmem_handle, shmem_size);
		Session session{ client.get() };
		session.run_loop();
	} catch (...) {
		ipc_log_current_exception();
		throw;
	}

	return 0;
}
