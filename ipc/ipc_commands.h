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

class IPCError;


constexpr uint32_t INVALID_TRANSACTION = ~static_cast<uint32_t>(0);

enum class CommandType : int32_t;
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

	virtual size_t size_internal() const { return 0; }
	virtual void serialize_internal(void *buf) const {}
public:
	virtual ~Command() = default;

	void set_transaction_id(uint32_t transaction_id) { m_transaction_id = transaction_id; }
	void set_response_id(uint32_t response_id) { m_response_id = response_id; }

	size_t serialized_size() const;
	void serialize(void *buf) const;

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
	std::string m_arg;
protected:
	static std::unique_ptr<Command_Args1_str> deserialize_internal(const void *buf, size_t size)
	{
		size_t len = ipc::deserialize_str(nullptr, buf, size);
		if (len == ~static_cast<size_t>(0))
			throw_deserialization_error("buffer overrun");

		std::string str(len, '\0');
		ipc::deserialize_str(&str[0], buf, size);
		return std::make_unique<Command_Args1_str>(str);
	}

	size_t size_internal() const override { return ipc::serialize_str(nullptr, m_arg.c_str(), m_arg.size()); }
	void serialize_internal(void *buf) const override { ipc::serialize_str(buf, m_arg.c_str(), m_arg.size()); }
public:
	explicit Command_Args1_str(const std::string &arg) : Command{ Type }, m_arg{ arg } {}

	const std::string &arg() const { return m_arg; }

	friend std::unique_ptr<Command>(::ipc_client::deserialize_command)(const ipc::Command *command);
};

template <CommandType Type>
class Command_Args1_wstr : public Command {
	std::wstring m_arg;
protected:
	static std::unique_ptr<Command_Args1_wstr> deserialize_internal(const void *buf, size_t size)
	{
		size_t len = ipc::deserialize_wstr(nullptr, buf, size);
		if (len == ~static_cast<size_t>(0))
			throw_deserialization_error("buffer overrun");

		std::wstring str(len, '\0');
		ipc::deserialize_wstr(&str[0], buf, size);
		return std::make_unique<Command_Args1_wstr>(str);
	}

	size_t size_internal() const override { return ipc::serialize_wstr(nullptr, m_arg.c_str(), m_arg.size()); }
	void serialize_internal(void *buf) const override { ipc::serialize_wstr(buf, m_arg.c_str(), m_arg.size()); }
public:
	explicit Command_Args1_wstr(const std::wstring &arg) : Command{ Type }, m_arg{ arg } {}

	const std::wstring &arg() const { return m_arg; }

	friend std::unique_ptr<Command> (::ipc_client::deserialize_command)(const ipc::Command *command);
};

template <CommandType Type, class T>
class Commands_Args1_pod : public Command {
	T m_arg;
protected:
	static std::unique_ptr<Commands_Args1_pod> deserialize_internal(const void *buf, size_t size)
	{
		if (size < sizeof(T))
			throw_deserialization_error("buffer overrun");
		return std::make_unique<Commands_Args1_pod>(*static_cast<const T *>(buf));
	}

	size_t size_internal() const override { return sizeof(T); }
	void serialize_internal(void *buf) const override { *static_cast<T *>(buf) = m_arg; }
public:
	explicit Commands_Args1_pod(const T &arg) : Command{ Type }, m_arg(arg) {}

	const T &arg() const { return m_arg; }

	friend std::unique_ptr<Command> (::ipc_client::deserialize_command)(const ipc::Command *command);
};

} // namespace detail


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

typedef detail::Command_Args0<CommandType::ACK> CommandAck;
typedef detail::Command_Args0<CommandType::ERR> CommandErr;
typedef detail::Command_Args1_wstr<CommandType::SET_LOG_FILE> CommandSetLogFile;
typedef detail::Command_Args1_wstr<CommandType::LOAD_AVISYNTH> CommandLoadAvisynth;
typedef detail::Command_Args0<CommandType::NEW_SCRIPT_ENV> CommandNewScriptEnv;
typedef detail::Command_Args1_str<CommandType::GET_SCRIPT_VAR> CommandGetScriptVar;

class CommandSetScriptVar : public Command {
	std::string m_name;
	ipc::Value m_value;
protected:
	static std::unique_ptr<CommandSetScriptVar> deserialize_internal(const void *buf, size_t size);

	size_t size_internal() const override;
	void serialize_internal(void *buf) const override;
public:
	CommandSetScriptVar(const std::string &name, const ipc::Value &value) :
		Command{ CommandType::SET_SCRIPT_VAR },
		m_name{ name },
		m_value(value)
	{}

	const std::string &name() const { return m_name; }
	const ipc::Value &value() const { return m_value; }

	friend std::unique_ptr<Command> deserialize_command(const ipc::Command *command);
};

typedef detail::Commands_Args1_pod<CommandType::EVAL_SCRIPT, uint32_t> CommandEvalScript;
typedef detail::Commands_Args1_pod<CommandType::GET_FRAME, ipc::VideoFrameRequest> CommandGetFrame;
typedef detail::Commands_Args1_pod<CommandType::SET_FRAME, ipc::VideoFrame> CommandSetFrame;

std::unique_ptr<Command> deserialize_command(const ipc::Command *command);


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
