#pragma once

#ifndef AVSHOST_H_
#define AVSHOST_H_

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include "ipc/ipc_commands.h"
#include "ipc/win32util.h"

class IScriptEnvironment;

class IClip;
class PClip;

class AVSValue;


namespace ipc_client {

class IPCClient;

} // namespace ipc_client


namespace avs {

class Cache;
class VirtualClip;

class AvisynthError_ : public std::runtime_error {
	using std::runtime_error::runtime_error;
};

class AvisynthHost : public ipc_client::CommandObserver {
	typedef ::IScriptEnvironment *(__stdcall *create_script_env)(int);

	struct IScriptEnvironmentDeleter {
		void operator()(::IScriptEnvironment *env);
	};

	class PClip_ {
		union {
			IClip *i;
			unsigned char x[sizeof(IClip *)];
		} m_val;
	public:
		PClip_();
		PClip_(const ::PClip &p);
		PClip_(const PClip_ &other);

		~PClip_();

		PClip_ &operator=(const PClip_ &other);

		PClip &get() { return *reinterpret_cast<::PClip *>(m_val.x); }
		const PClip &get() const { return *reinterpret_cast<const ::PClip *>(m_val.x); }
	};

	ipc_client::IPCClient *m_client;
	win32::unique_module m_library;
	create_script_env m_create_script_env;
	std::unique_ptr<::IScriptEnvironment, IScriptEnvironmentDeleter> m_env;

	std::unique_ptr<Cache> m_cache;
	std::unordered_map<uint32_t, PClip_> m_remote_clips;
	std::unordered_map<uint32_t, PClip_> m_local_clips;
	uint32_t m_local_clip_id;

	int observe(std::unique_ptr<ipc_client::CommandLoadAvisynth> c) override;
	int observe(std::unique_ptr<ipc_client::CommandNewScriptEnv> c) override;
	int observe(std::unique_ptr<ipc_client::CommandSetScriptVar> c) override;
	int observe(std::unique_ptr<ipc_client::CommandGetScriptVar> c) override;
	int observe(std::unique_ptr<ipc_client::CommandEvalScript> c) override;
	int observe(std::unique_ptr<ipc_client::CommandGetFrame> c) override;
	int observe(std::unique_ptr<ipc_client::CommandSetFrame> c) override;

	void send_avsvalue(uint32_t response_id, const ::AVSValue &avs_value);

	void send_err(uint32_t response_id);
public:
	explicit AvisynthHost(ipc_client::IPCClient *client);

	~AvisynthHost();
};

} // namespace avs

#endif // AVSHOST_H_
