#pragma once

#ifndef IPC_COMMANDS_H_
#define IPC_COMMANDS_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include "video_types.h"

namespace ipc {
struct Command;
} // namespace ipc


namespace ipc_client {

constexpr uint32_t INVALID_TRANSACTION = ~static_cast<uint32_t>(0);

class IPCClient;
class IPCError;

enum class CommandType : int32_t {
	ACK,
	ERR,
	SET_LOG_FILE,
	LOAD_AVISYNTH,
	NEW_SCRIPT_ENV,
	GET_SCRIPT_VAR,
	SET_SCRIPT_VAR,
	EVAL_SCRIPT,
	GET_FRAME,
	SET_FRAME,
};

class Command {
protected:
	uint32_t m_transaction_id;
	uint32_t m_response_id;
	CommandType m_type;

	explicit Command(CommandType type) :
		m_transaction_id{ INVALID_TRANSACTION },
		m_response_id{ INVALID_TRANSACTION },
		m_type{ type }
	{}

	void deserialize_common(const ipc::Command *command);

	virtual size_t size_internal() const noexcept { return 0; }
	virtual void serialize_internal(void *buf) const noexcept {}
public:
	virtual ~Command() = default;

	// Deallocate resources held on the IPC heap. Any heap resources must be deallocated
	// or relinquished before the destructor is called.
	virtual void deallocate_heap_resources(IPCClient *client) {}

	// Relinquish heap resources after command is successfully written to IPC queue.
	virtual void relinquish_heap_resources() noexcept {}

	void set_transaction_id(uint32_t transaction_id) { m_transaction_id = transaction_id; }
	void set_response_id(uint32_t response_id) { m_response_id = response_id; }

	size_t serialized_size() const noexcept;
	void serialize(void *buf) const noexcept;

	uint32_t transaction_id() const { return m_transaction_id; }
	uint32_t response_id() const { return m_response_id; }
	CommandType type() const { return m_type; }

	friend std::unique_ptr<Command> deserialize_command(const ipc::Command *command);
};

std::unique_ptr<Command> deserialize_command(const ipc::Command *command);


namespace detail {

[[noreturn]] void throw_deserialization_error(const char *msg);

template <CommandType Type>
class Command_Args0 : public Command {
public:
	Command_Args0() : Command{ Type } {}
};

template <CommandType Type>
class Command_Args1_str : public Command {
protected:
	std::string m_arg;

	template <class Derived = Command_Args1_str>
	static std::unique_ptr<Derived> deserialize_internal(const void *buf, size_t size);

	size_t size_internal() const noexcept override { return ipc::serialize_str(nullptr, m_arg.c_str(), m_arg.size()); }
	void serialize_internal(void *buf) const noexcept override { ipc::serialize_str(buf, m_arg.c_str(), m_arg.size()); }
public:
	explicit Command_Args1_str(const std::string &arg) : Command{ Type }, m_arg{ arg } {}

	const std::string &arg() const { return m_arg; }

	friend std::unique_ptr<Command> (::ipc_client::deserialize_command)(const ipc::Command *command);
};

template <CommandType Type>
class Command_Args1_wstr : public Command {
protected:
	std::wstring m_arg;

	template <class Derived = Command_Args1_wstr>
	static std::unique_ptr<Derived> deserialize_internal(const void *buf, size_t size);

	size_t size_internal() const noexcept override { return ipc::serialize_wstr(nullptr, m_arg.c_str(), m_arg.size()); }
	void serialize_internal(void *buf) const noexcept override { ipc::serialize_wstr(buf, m_arg.c_str(), m_arg.size()); }
public:
	explicit Command_Args1_wstr(const std::wstring &arg) : Command{ Type }, m_arg{ arg } {}

	const std::wstring &arg() const { return m_arg; }

	friend std::unique_ptr<Command> (::ipc_client::deserialize_command)(const ipc::Command *command);
};

template <CommandType Type, class T>
class Command_Args1_pod : public Command {
protected:
	T m_arg;

	template <class Derived = Command_Args1_pod>
	static std::unique_ptr<Derived> deserialize_internal(const void *buf, size_t size);

	size_t size_internal() const noexcept override { return sizeof(T); }
	void serialize_internal(void *buf) const noexcept override { *static_cast<T *>(buf) = m_arg; }
public:
	explicit Command_Args1_pod(const T &arg) : Command{ Type }, m_arg(arg) {}

	const T &arg() const { return m_arg; }

	friend std::unique_ptr<Command> (::ipc_client::deserialize_command)(const ipc::Command *command);
};

class CommandSetScriptVar : public Command {
	std::string m_name;
	ipc::Value m_value;
protected:
	static std::unique_ptr<CommandSetScriptVar> deserialize_internal(const void *buf, size_t size);

	size_t size_internal() const noexcept override;
	void serialize_internal(void *buf) const noexcept override;
public:
	CommandSetScriptVar(const std::string &name, const ipc::Value &value) :
		Command{ CommandType::SET_SCRIPT_VAR },
		m_name{ name },
		m_value{ value }
	{}

	~CommandSetScriptVar() override;

	void deallocate_heap_resources(IPCClient *client) override;
	void relinquish_heap_resources() noexcept override;

	const std::string &name() const { return m_name; }
	const ipc::Value &value() const { return m_value; }

	friend std::unique_ptr<Command> (::ipc_client::deserialize_command)(const ipc::Command *command);
};

class CommandEvalScript : public Command_Args1_pod<CommandType::EVAL_SCRIPT, uint32_t> {
protected:
	static std::unique_ptr<CommandEvalScript> deserialize_internal(const void *buf, size_t size)
	{
		return Command_Args1_pod::deserialize_internal<CommandEvalScript>(buf, size);
	}
public:
	using Command_Args1_pod::Command_Args1_pod;

	~CommandEvalScript() override;

	void deallocate_heap_resources(IPCClient *client) override;
	void relinquish_heap_resources() noexcept override;

	friend std::unique_ptr<Command>(::ipc_client::deserialize_command)(const ipc::Command *command);
};

class CommandSetFrame : public Command_Args1_pod<CommandType::SET_FRAME, ipc::VideoFrame> {
protected:
	static std::unique_ptr<CommandSetFrame> deserialize_internal(const void *buf, size_t size)
	{
		return Command_Args1_pod::deserialize_internal<CommandSetFrame>(buf, size);
	}
public:
	using Command_Args1_pod::Command_Args1_pod;

	~CommandSetFrame() override;

	void deallocate_heap_resources(IPCClient *client) override;
	void relinquish_heap_resources() noexcept override;

	friend std::unique_ptr<Command>(::ipc_client::deserialize_command)(const ipc::Command *command);
};

} // namespace detail


typedef detail::Command_Args0<CommandType::ACK> CommandAck;
typedef detail::Command_Args0<CommandType::ERR> CommandErr;
typedef detail::Command_Args1_wstr<CommandType::SET_LOG_FILE> CommandSetLogFile;
typedef detail::Command_Args1_wstr<CommandType::LOAD_AVISYNTH> CommandLoadAvisynth;
typedef detail::Command_Args0<CommandType::NEW_SCRIPT_ENV> CommandNewScriptEnv;
typedef detail::Command_Args1_str<CommandType::GET_SCRIPT_VAR> CommandGetScriptVar;
typedef detail::CommandSetScriptVar CommandSetScriptVar;
typedef detail::CommandEvalScript CommandEvalScript;
typedef detail::Command_Args1_pod<CommandType::GET_FRAME, ipc::VideoFrameRequest> CommandGetFrame;
typedef detail::CommandSetFrame CommandSetFrame;

class CommandObserver {
protected:
	virtual int observe(std::unique_ptr<CommandAck> c) { return 0; }
	virtual int observe(std::unique_ptr<CommandErr> c) { return 0; }
	virtual int observe(std::unique_ptr<CommandSetLogFile> c) { return 0; }
	virtual int observe(std::unique_ptr<CommandLoadAvisynth> c) { return 0; }
	virtual int observe(std::unique_ptr<CommandNewScriptEnv> c) { return 0; }
	virtual int observe(std::unique_ptr<CommandGetScriptVar> c) { return 0; }
	virtual int observe(std::unique_ptr<CommandSetScriptVar> c) { return 0; }
	virtual int observe(std::unique_ptr<CommandEvalScript> c) { return 0; }
	virtual int observe(std::unique_ptr<CommandGetFrame> c) { return 0; }
	virtual int observe(std::unique_ptr<CommandSetFrame> c) { return 0; }
public:
	int dispatch(std::unique_ptr<Command> c);
};

} // namespace ipc_client

#endif // IPC_COMMANDS_H_
