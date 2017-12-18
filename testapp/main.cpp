#include <cstdio>
#include <memory>
#include "ipc/ipc_client.h"
#include "ipc/ipc_commands.h"
#include "ipc/logging.h"
#include "ipc/video_types.h"

namespace {

void wait_for_key()
{
	std::puts("Press enter...");
	std::fgetc(stdin);
}

} // namespace


int wmain(int argc, wchar_t **argv)
{
	if (argc != 2)
		return 1;

	ipc_set_log_handler(ipc_log_stderr, ipc_wlog_stderr);

	try {
		auto client = std::make_unique<ipc_client::IPCClient>(ipc_client::IPCClient::master(), argv[1]);
		client->start(nullptr);

		std::unique_ptr<ipc_client::Command> response;

		ipc_log0("load avisynth\n");
		wait_for_key();
		response = client->send_sync(std::make_unique<ipc_client::CommandLoadAvisynth>(L""));
		if (!response || response->type() != ipc_client::CommandType::ACK) {
			ipc_log0("load avisynth failed\n");
			wait_for_key();
			return 1;
		}

		ipc_log0("eval script\n");
		wait_for_key();
		{
			constexpr char script[] = "BlankClip()\r\n";
			void *script_mem = client->allocate(ipc::serialize_str(nullptr, script, sizeof(script) - 1));
			ipc::serialize_str(script_mem, script, sizeof(script) - 1);
			response = client->send_sync(std::make_unique<ipc_client::CommandEvalScript>(client->pointer_to_offset(script_mem)));
		}
		if (!response || response->type() != ipc_client::CommandType::SET_SCRIPT_VAR) {
			ipc_log0("eval script failed\n");
			wait_for_key();
			return 1;
		}

		uint32_t clip_id = 0;
		{
			ipc_client::CommandSetScriptVar *c = static_cast<ipc_client::CommandSetScriptVar *>(response.get());
			switch (c->value().type) {
			case ipc::Value::CLIP:
				ipc_log("received clip: %dx%d\n", c->value().c.vi.width, c->value().c.vi.height);
				clip_id = c->value().c.clip_id;
				break;
			case ipc::Value::BOOL_:
				ipc_log("received bool: %d\n", c->value().b);
				break;
			case ipc::Value::INT:
				ipc_log("received int: %d\n", c->value().i);
				break;
			case ipc::Value::FLOAT:
				ipc_log("received float: %f\n", c->value().f);
				break;
			case ipc::Value::STRING:
				ipc_log("received string\n");
				client->deallocate(client->offset_to_pointer(c->value().s));
				break;
			default:
				ipc_log("received unknown: %c\n", c->value().type);
				break;
			}

			wait_for_key();
			if (c->value().type != ipc::Value::CLIP)
				return 1;
		}

		ipc_log0("get frame 0\n");
		wait_for_key();
		response = client->send_sync(std::make_unique<ipc_client::CommandGetFrame>(ipc::VideoFrameRequest{ clip_id, 0 }));
		if (!response || response->type() != ipc_client::CommandType::SET_FRAME) {
			ipc_log0("get frame 0 failed\n");
			wait_for_key();
			return 1;
		}

		client->deallocate(client->offset_to_pointer(static_cast<ipc_client::CommandSetFrame *>(response.get())->arg().heap_offset));
		wait_for_key();
	} catch (...) {
		ipc_log_current_exception();
		throw;
	}

	return 0;
}
