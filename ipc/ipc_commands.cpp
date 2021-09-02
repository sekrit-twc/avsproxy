#include <cassert>
#include <cstdint>
#include "ipc_client.h"
#include "ipc_commands.h"
#include "ipc_types.h"
#include "logging.h"

namespace ipc_client {

namespace {

template <class T, class U>
std::unique_ptr<T> unique_ptr_cast(std::unique_ptr<U> &&ptr) noexcept
{
	return std::unique_ptr<T>(static_cast<T *>(ptr.release()));
}

} // namespace


namespace detail {

void throw_deserialization_error(const char *msg)
{
	throw IPCError{ msg };
}


template <CommandType Type>
template <class Derived>
std::unique_ptr<Derived> Command_Args1_str<Type>::deserialize_internal(const void *buf, size_t size)
{
	size_t len = ipc::deserialize_str(nullptr, buf, size);
	if (len == ~static_cast<size_t>(0))
		throw_deserialization_error("buffer overrun");

	std::string str(len, '\0');
	ipc::deserialize_str(&str[0], buf, size);
	return std::make_unique<Derived>(str);
}


template <CommandType Type>
template <class Derived>
std::unique_ptr<Derived> Command_Args1_wstr<Type>::deserialize_internal(const void *buf, size_t size)
{
	size_t len = ipc::deserialize_wstr(nullptr, buf, size);
	if (len == ~static_cast<size_t>(0))
		throw_deserialization_error("buffer overrun");

	std::wstring str(len, '\0');
	ipc::deserialize_wstr(&str[0], buf, size);
	return std::make_unique<Derived>(str);
}


template <CommandType Type, class T>
template <class Derived>
std::unique_ptr<Derived> Command_Args1_pod<Type, T>::deserialize_internal(const void *buf, size_t size)
{
	if (size < sizeof(T))
		throw_deserialization_error("buffer overrun");
	return std::make_unique<Derived>(*static_cast<const T *>(buf));
}


std::unique_ptr<CommandSetScriptVar> CommandSetScriptVar::deserialize_internal(const void *buf, size_t size)
{
	size_t len = ipc::deserialize_str(nullptr, buf, size);
	if (len == ~static_cast<size_t>(0))
		detail::throw_deserialization_error("buffer overrun");

	std::string name(len, '\0');
	ipc::deserialize_str(&name[0], buf, size);

	size_t consumed = ipc::serialize_str(nullptr, name.data(), name.size());
	if (consumed % alignof(ipc::Value)) {
		size_t pad = alignof(ipc::Value) - consumed % alignof(ipc::Value);
		if (pad > size - consumed)
			detail::throw_deserialization_error("buffer overrun");
		consumed += pad;
	}

	buf = static_cast<const unsigned char *>(buf) + consumed;
	size -= consumed;

	ipc::Value value = *static_cast<const ipc::Value *>(buf);
	return std::make_unique<CommandSetScriptVar>(name, value);
}

CommandSetScriptVar::~CommandSetScriptVar()
{
	if (m_value.type == ipc::Value::STRING && m_value.s != ipc::NULL_OFFSET)
		ipc_log("leaking heap allocation at %u", m_value.s);
}

size_t CommandSetScriptVar::size_internal() const noexcept
{
	size_t size = ipc::serialize_str(nullptr, m_name.data(), m_name.size());
	assert(size <= UINT32_MAX - sizeof(ipc::Value));
	if (size % alignof(ipc::Value))
		size += alignof(ipc::Value) - size % alignof(ipc::Value);
	return size + sizeof(ipc::Value);
}

void CommandSetScriptVar::serialize_internal(void *buf) const noexcept
{
	size_t size = ipc::serialize_str(buf, m_name.data(), m_name.size());
	if (size % alignof(ipc::Value))
		size += alignof(ipc::Value) - size % alignof(ipc::Value);
	*reinterpret_cast<ipc::Value *>(static_cast<unsigned char *>(buf) + size) = m_value;
}

void CommandSetScriptVar::deallocate_heap_resources(IPCClient *client)
{
	if (m_value.type == ipc::Value::STRING) {
		client->deallocate(client->offset_to_pointer(m_value.s));
		m_value.s = ipc::NULL_OFFSET;
	}
}

void CommandSetScriptVar::relinquish_heap_resources() noexcept
{
	if (m_value.type == ipc::Value::STRING)
		m_value.s = ipc::NULL_OFFSET;
}


CommandEvalScript::~CommandEvalScript()
{
	if (m_arg != ipc::NULL_OFFSET)
		ipc_log("leaking heap allocation at %u", m_arg);
}

void CommandEvalScript::deallocate_heap_resources(IPCClient *client)
{
	client->deallocate(client->offset_to_pointer(m_arg));
	m_arg = ipc::NULL_OFFSET;
}

void CommandEvalScript::relinquish_heap_resources() noexcept
{
	m_arg = ipc::NULL_OFFSET;
}


CommandSetFrame::~CommandSetFrame()
{
	if (m_arg.heap_offset != ipc::NULL_OFFSET)
		ipc_log("leaking heap allocation at %u", m_arg.heap_offset);
}

void CommandSetFrame::deallocate_heap_resources(IPCClient *client)
{
	client->deallocate(client->offset_to_pointer(m_arg.heap_offset));
	m_arg.heap_offset = ipc::NULL_OFFSET;
}

void CommandSetFrame::relinquish_heap_resources() noexcept
{
	m_arg.heap_offset = ipc::NULL_OFFSET;
}

} // namespace detail


void Command::deserialize_common(const ipc::Command *command)
{
	m_transaction_id = command->transaction_id;
	m_response_id = command->response_id;
}

size_t Command::serialized_size() const noexcept
{
	size_t internal_size = size_internal();
	assert(internal_size <= UINT32_MAX);
	size_t size = sizeof(ipc::Command) + internal_size;
	assert(size <= UINT32_MAX);
	return size;
}

void Command::serialize(void *buf) const noexcept
{
	ipc::Command *command = new (buf) ipc::Command{};
	command->size = static_cast<uint32_t>(serialized_size());
	command->transaction_id = transaction_id();
	command->response_id = response_id();
	command->type = static_cast<int32_t>(type());

	void *payload = ipc::offset_to_pointer<void>(command, sizeof(ipc::Command));
	serialize_internal(payload);
}


std::unique_ptr<Command> deserialize_command(const ipc::Command *command)
{
	assert(ipc::check_fourcc(command->magic, "cmdx"));

	if (command->size < sizeof(ipc::Command))
		detail::throw_deserialization_error("buffer overrun");

	void *payload = ipc::offset_to_pointer<void>(command, sizeof(ipc::Command));
	size_t payload_size = command->size - sizeof(ipc::Command);

	std::unique_ptr<Command> deserialized;

	switch (static_cast<CommandType>(command->type)) {
	case CommandType::ACK:
		deserialized = std::make_unique<CommandAck>();
		break;
	case CommandType::ERR:
		deserialized = std::make_unique<CommandErr>();
		break;
	case CommandType::SET_LOG_FILE:
		deserialized = CommandSetLogFile::deserialize_internal(payload, payload_size);
		break;
	case CommandType::LOAD_AVISYNTH:
		deserialized = CommandLoadAvisynth::deserialize_internal(payload, payload_size);
		break;
	case CommandType::NEW_SCRIPT_ENV:
		deserialized = std::make_unique<CommandNewScriptEnv>();
		break;
	case CommandType::GET_SCRIPT_VAR:
		deserialized = CommandGetScriptVar::deserialize_internal(payload, payload_size);
		break;
	case CommandType::SET_SCRIPT_VAR:
		deserialized = CommandSetScriptVar::deserialize_internal(payload, payload_size);
		break;
	case CommandType::EVAL_SCRIPT:
		deserialized = CommandEvalScript::deserialize_internal(payload, payload_size);
		break;
	case CommandType::GET_FRAME:
		deserialized = CommandGetFrame::deserialize_internal(payload, payload_size);
		break;
	case CommandType::SET_FRAME:
		deserialized = CommandSetFrame::deserialize_internal(payload, payload_size);
		break;
	default:
		break;
	}

	if (deserialized)
		deserialized->deserialize_common(command);

	return deserialized;
}


int CommandObserver::dispatch(std::unique_ptr<Command> c)
{
	switch (c->type()) {
	case CommandType::ACK:
		return observe(unique_ptr_cast<CommandAck>(std::move(c)));
	case CommandType::ERR:
		return observe(unique_ptr_cast<CommandErr>(std::move(c)));
	case CommandType::SET_LOG_FILE:
		return observe(unique_ptr_cast<CommandSetLogFile>(std::move(c)));
	case CommandType::LOAD_AVISYNTH:
		return observe(unique_ptr_cast<CommandLoadAvisynth>(std::move(c)));
	case CommandType::NEW_SCRIPT_ENV:
		return observe(unique_ptr_cast<CommandNewScriptEnv>(std::move(c)));
	case CommandType::GET_SCRIPT_VAR:
		return observe(unique_ptr_cast<CommandGetScriptVar>(std::move(c)));
	case CommandType::SET_SCRIPT_VAR:
		return observe(unique_ptr_cast<CommandSetScriptVar>(std::move(c)));
	case CommandType::EVAL_SCRIPT:
		return observe(unique_ptr_cast<CommandEvalScript>(std::move(c)));
	case CommandType::GET_FRAME:
		return observe(unique_ptr_cast<CommandGetFrame>(std::move(c)));
	case CommandType::SET_FRAME:
		return observe(unique_ptr_cast<CommandSetFrame>(std::move(c)));
	default:
		return 0;
	}
}

} // namespace ipc_client
