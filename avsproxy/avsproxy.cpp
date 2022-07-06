#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <Windows.h>
#include "ipc/ipc_client.h"
#include "ipc/ipc_commands.h"
#include "ipc/ipc_types.h"
#include "ipc/video_types.h"
#include "ipc/win32util.h"
#include "p2p_api.h"
#include "VSHelper4.h"
#include "vsxx4_pluginmain.h"

using namespace vsxx4;

namespace {

constexpr char PLUGIN_ID[] = "xxx.abc.avsproxy";

constexpr size_t MAX_STR_LEN = 1UL << 20;


std::wstring utf8_to_utf16(const std::string &s)
{
	if (s.empty())
		return L"";

	int required = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
	if (required <= 0)
		win32::trap_error("UTF-8 decoding error");

	std::wstring ws(required, L'\0');
	::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &ws[0], required);
	return ws;
}

std::string heap_to_local_str(ipc_client::IPCClient *client, uint32_t offset)
{
	void *ptr = client->offset_to_pointer(offset);
	if (!ptr)
		return "";

	size_t len = ipc::deserialize_str(nullptr, ptr, -1);
	if (len > MAX_STR_LEN)
		throw std::runtime_error{ "string too long" };

	std::string ret(len, '\0');
	ipc::deserialize_str(&ret[0], ptr, -1);
	return ret;
}

uint32_t local_to_heap_str(ipc_client::IPCClient *client, const char *str, size_t len)
{
	if (len > MAX_STR_LEN)
		throw std::runtime_error{ "string too long" };

	size_t size = ipc::serialize_str(nullptr, str, len);
	void *ptr = client->allocate(size);
	ipc::serialize_str(ptr, str, len);
	return client->pointer_to_offset(ptr);
}

::VSVideoInfo deserialize_video_info(const ipc::VideoInfo &ipc_vi, const Core &core)
{
	::VSVideoInfo vi{};

	vi.fpsNum = ipc_vi.fps_num;
	vi.fpsDen = ipc_vi.fps_den;
	vi.width = ipc_vi.width;
	vi.height = ipc_vi.height;
	vi.numFrames = ipc_vi.num_frames;

	vsh::reduceRational(&vi.fpsNum, &vi.fpsDen);

	switch (ipc_vi.color_family) {
	case ipc::VideoInfo::RGB:
		vi.format = core.query_video_format(::cfRGB, ::stInteger, 8, ipc_vi.subsample_w, ipc_vi.subsample_h);
		break;
	case ipc::VideoInfo::YUV:
		vi.format = core.query_video_format(::cfYUV, ::stInteger, 8, ipc_vi.subsample_w, ipc_vi.subsample_h);
		break;
	case ipc::VideoInfo::GRAY:
		vi.format = core.get_video_format_by_id(::pfGray8);
		break;
	case ipc::VideoInfo::RGB24:
	case ipc::VideoInfo::RGB32:
		vi.format = core.get_video_format_by_id(::pfRGB24);
		break;
	case ipc::VideoInfo::YUY2:
		vi.format = core.get_video_format_by_id(::pfYUV422P8);
		break;
	default:
		break;
	}

	if (vi.format.colorFamily == ::cfUndefined)
		throw std::runtime_error{ "color format not supported" };

	return vi;
}

ipc::VideoInfo serialize_video_info(const ::VSVideoInfo &vi)
{
	if (!vsh::isConstantVideoFormat(&vi))
		throw std::runtime_error{ "constant format required" };
	if (vi.format.bitsPerSample != 8)
		throw std::runtime_error{ "high bit-depth not supported" };

	ipc::VideoInfo ipc_vi{};

	int64_t fps_num = vi.fpsNum;
	int64_t fps_den = vi.fpsDen;

	if (fps_num > INT32_MAX || fps_den > INT32_MAX) {
		int64_t max_val = std::max(fps_num, fps_den);
		int64_t divisor = max_val / INT32_MAX + (max_val % INT32_MAX ? 1 : 0);

		fps_num /= divisor;
		fps_den /= divisor;
		assert(fps_num <= INT32_MAX);
		assert(fps_den <= INT32_MAX);
	}

	ipc_vi.width = vi.width;
	ipc_vi.height = vi.height;
	ipc_vi.fps_num = static_cast<int32_t>(fps_num);
	ipc_vi.fps_den = static_cast<int32_t>(fps_den);
	ipc_vi.num_frames = vi.numFrames;

	switch (vi.format.colorFamily) {
	case ::cfRGB:
		ipc_vi.color_family = ipc::VideoInfo::RGB32;
		break;
	case ::cfYUV:
		ipc_vi.color_family = ipc::VideoInfo::YUV;
		break;
	case ::cfGray:
		ipc_vi.color_family = ipc::VideoInfo::GRAY;
		break;
	default:
		throw std::runtime_error{ "color format not supported" };
	}

	ipc_vi.subsample_w = vi.format.subSamplingW;
	ipc_vi.subsample_h = vi.format.subSamplingH;

	return ipc_vi;
}

Frame heap_to_local_frame(ipc_client::IPCClient *client, const ::VSVideoInfo &vi, int32_t color_family, const ipc::VideoFrame &ipc_frame, const Core &core)
{
	unsigned char *heap_ptr = static_cast<unsigned char *>(client->offset_to_pointer(ipc_frame.heap_offset));
	if (!heap_ptr)
		throw std::runtime_error{ "missing frame data" };

	int src_planes = vi.format.numPlanes;

	if (color_family == ipc::VideoInfo::RGB24 || color_family == ipc::VideoInfo::RGB32 || color_family == ipc::VideoInfo::YUY2)
		src_planes = 1;

	for (int p = 0; p < src_planes; ++p) {
		int row_size = (vi.width >> (p ? vi.format.subSamplingW : 0)) * vi.format.bytesPerSample;

		if (color_family == ipc::VideoInfo::RGB24)
			row_size *= 3;
		else if (color_family == ipc::VideoInfo::RGB32)
			row_size *= 4;
		else if (color_family == ipc::VideoInfo::YUY2)
			row_size *= 2;

		if (ipc_frame.stride[p] < row_size)
			throw std::runtime_error{ "wrong width" };
		if (ipc_frame.height[p] != (vi.height >> (p ? vi.format.subSamplingH : 0)))
			throw std::runtime_error{ "wrong height" };
	}

	Frame frame = core.new_video_frame(vi.format, vi.width, vi.height);
	Frame alpha;

	if (color_family == ipc::VideoInfo::RGB32)
		alpha = core.new_video_frame(core.get_video_format_by_id(::pfGray8), vi.width, vi.height);

	if (color_family == ipc::VideoInfo::RGB24 || color_family == ipc::VideoInfo::RGB32 || color_family == ipc::VideoInfo::YUY2) {
		p2p_buffer_param param{};

		if (color_family == ipc::VideoInfo::RGB24 || color_family == ipc::VideoInfo::RGB32) {
			param.src[0] = heap_ptr + (ipc_frame.height[0] - 1) * ipc_frame.stride[0];
			param.src_stride[0] = -ipc_frame.stride[0];
		} else {
			param.src[0] = heap_ptr;
			param.src_stride[0] = ipc_frame.stride[0];
		}

		for (int p = 0; p < vi.format.numPlanes; ++p) {
			param.dst[p] = frame.write_ptr(p);
			param.dst_stride[p] = frame.stride(p);
		}
		if (color_family == ipc::VideoInfo::RGB32) {
			alpha = core.new_video_frame(core.get_video_format_by_id(::pfGray8), vi.width, vi.height);
			param.dst[3] = alpha.write_ptr(0) + (vi.height - 1) * alpha.stride(0);
			param.dst_stride[3] = -alpha.stride(0);
		}

		param.width = vi.width;
		param.height = vi.height;

		if (color_family == ipc::VideoInfo::RGB24)
			param.packing = p2p_rgb24_le;
		else if (color_family == ipc::VideoInfo::RGB32)
			param.packing = p2p_argb32_le;
		else if (color_family == ipc::VideoInfo::YUY2)
			param.packing = p2p_yuy2;

		p2p_unpack_frame(&param, 0);
	} else {
		const unsigned char *src_ptr = static_cast<const unsigned char *>(heap_ptr);

		for (int p = 0; p < vi.format.numPlanes; ++p) {
			unsigned row_size = (vi.width >> (p ? vi.format.subSamplingW : 0)) * vi.format.bytesPerSample;
			vsh::bitblt(frame.write_ptr(p), frame.stride(p), src_ptr, ipc_frame.stride[p], row_size, ipc_frame.height[p]);
			src_ptr += ipc_frame.stride[p] * ipc_frame.height[p];
		}
	}

	if (alpha)
		frame.frame_props_rw().set_prop("_Alpha", alpha);

	return frame;
}

ipc::VideoFrame local_to_heap_frame(ipc_client::IPCClient *client, uint32_t clip_id, int32_t n, const ::VSVideoInfo &vi, const ConstFrame &frame)
{
	ipc::VideoFrame ipc_frame{ { clip_id, n } };
	size_t size = 0;

	if (vi.format.colorFamily == ::cfRGB) {
		int rowsize = vi.width * 4;
		ipc_frame.stride[0] = rowsize % 64 ? rowsize + 64 - rowsize % 64 : rowsize;
		ipc_frame.height[0] = vi.height;
		size = ipc_frame.stride[0] * vi.height;
	} else {
		for (int p = 0; p < vi.format.numPlanes; ++p) {
			int rowsize = frame.width(p);
			ipc_frame.stride[p] = rowsize % 64 ? rowsize + 64 - rowsize % 64 : rowsize;
			ipc_frame.height[p] = frame.height(p);
			size += ipc_frame.stride[p] * ipc_frame.height[p];
		}
	}

	unsigned char *dst_ptr = static_cast<unsigned char *>(client->allocate(size));
	ipc_frame.heap_offset = client->pointer_to_offset(dst_ptr);

	if (vi.format.colorFamily == ::cfRGB) {
		ConstFrame alpha = frame.frame_props_ro().get_prop<ConstFrame>("_Alpha", map::Ignore{});
		p2p_buffer_param param{};

		param.src[0] = frame.read_ptr(0);
		param.src[1] = frame.read_ptr(1);
		param.src[2] = frame.read_ptr(2);
		param.src[3] = alpha ? alpha.read_ptr(0) : nullptr;

		param.src_stride[0] = frame.stride(0);
		param.src_stride[1] = frame.stride(1);
		param.src_stride[2] = frame.stride(2);
		param.src_stride[3] = alpha ? alpha.stride(0) : 0;

		param.dst[0] = dst_ptr + (ipc_frame.height[0] - 1) * ipc_frame.stride[0];
		param.dst_stride[0] = -ipc_frame.stride[0];

		param.width = vi.width;
		param.height = vi.height;
		param.packing = p2p_argb32_le;

		p2p_pack_frame(&param, P2P_ALPHA_SET_ONE);
	} else {
		for (int p = 0; p < vi.format.numPlanes; ++p) {
			unsigned rowsize = frame.width(p);
			vsh::bitblt(dst_ptr, ipc_frame.stride[p], frame.read_ptr(p), frame.stride(p), rowsize, frame.height(p));
			dst_ptr += ipc_frame.stride[p] * static_cast<int>(frame.height(p));
		}
	}

	return ipc_frame;
}

} // namespace


class AVSProxy : public FilterBase {
	std::unique_ptr<ipc_client::IPCClient> m_client;
	std::unordered_map<uint32_t, FilterNode> m_clips;
	ipc::Value m_script_result;
	::VSVideoInfo m_vi;

	std::deque<std::unique_ptr<ipc_client::Command>> m_command_queue;
	std::unique_ptr<ipc_client::Command> m_runloop_response;
	std::mutex m_mutex;
	std::condition_variable m_cond;
	std::atomic_uint32_t m_active_request;
	std::atomic_bool m_runloop_response_received;
	std::atomic_bool m_remote_exit;

	void fatal()
	{
		m_client->stop();
		m_remote_exit = true;
	}

	std::unique_ptr<ipc_client::Command> expect_response(std::unique_ptr<ipc_client::Command> c, ipc_client::CommandType expected_type)
	{
		auto throw_ = [&](const char *msg)
		{
			if (c)
				reject(std::move(c));
			throw std::runtime_error{ msg };
		};

		if (!c)
			throw_("no response received for command");
		if (c->type() == ipc_client::CommandType::ERR)
			throw_("command failed");
		if (c->type() != expected_type)
			throw_("unexpected resposne received for command");

		return c;
	}

	void recv_callback(std::unique_ptr<ipc_client::Command> c)
	{
		std::unique_lock<std::mutex> lock{ m_mutex };

		if (c)
			m_command_queue.emplace_back(std::move(c));
		else
			m_remote_exit = true;

		lock.unlock();
		m_cond.notify_all();
	}

	void runloop_callback(uint32_t request, std::unique_ptr<ipc_client::Command> c)
	{
		if (request != m_active_request) {
			c->deallocate_heap_resources(m_client.get());
			send_err(c->transaction_id());
			return;
		}

		std::unique_lock<std::mutex> lock{ m_mutex };

		m_runloop_response = std::move(c);
		m_runloop_response_received = true;

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

	void expect_ack(std::unique_ptr<ipc_client::Command> c)
	{
		c = expect_response(std::move(c), ipc_client::CommandType::ACK);
		c->deallocate_heap_resources(m_client.get());
	}

	void reject(std::unique_ptr<ipc_client::Command> c)
	{
		c->deallocate_heap_resources(m_client.get());
		send_err(c->transaction_id());
	}

	void reject_commands()
	{
		// Caller must acquire mutex.
		assert(!m_mutex.try_lock());

		// Reject any slave activity from a previous frame.
		while (!m_command_queue.empty()) {
			std::unique_ptr<ipc_client::Command> c{ std::move(m_command_queue.front()) };
			m_command_queue.pop_front();
			reject(std::move(c));
		}
	}

	void service_remote_getframe(std::unique_ptr<ipc_client::CommandGetFrame> c)
	{
		auto it = m_clips.find(c->arg().clip_id);
		if (it == m_clips.end()) {
			c->deallocate_heap_resources(m_client.get());
			send_err(c->transaction_id());
			return;
		}

		ConstFrame frame;

		try {
			frame = it->second.get_frame(c->arg().frame_number);
		} catch (...) {
			c->deallocate_heap_resources(m_client.get());
			send_err(c->transaction_id());
			return;
		}

		ipc::VideoFrame ipc_frame = local_to_heap_frame(m_client.get(), c->arg().clip_id, c->arg().frame_number, it->second.video_info(), frame);
		std::unique_ptr<ipc_client::Command> response;

		try {
			response = std::make_unique<ipc_client::CommandSetFrame>(ipc_frame);
		} catch (...) {
			m_client->deallocate(m_client->offset_to_pointer(ipc_frame.heap_offset));
			throw;
		}

		response->set_response_id(c->transaction_id());
		m_client->send_async(std::move(response));
	}

	std::unique_ptr<ipc_client::Command> runloop(std::unique_ptr<ipc_client::Command> c)
	{
		if (m_remote_exit) {
			c->deallocate_heap_resources(m_client.get());
			throw std::runtime_error{ "remote process exited" };
		}

		std::unique_lock<std::mutex> lock{ m_mutex };
		reject_commands();

		m_runloop_response.reset();
		m_runloop_response_received = false;
		m_client->send_async(std::move(c), std::bind(&AVSProxy::runloop_callback, this, ++m_active_request, std::placeholders::_1));

		while (true) {
			m_cond.wait(lock, [&]() { return m_remote_exit || m_runloop_response_received || !m_command_queue.empty(); });

			if (m_remote_exit)
				throw std::runtime_error{ "remote process exited" };

			if (m_runloop_response_received)
				break;

			while (!m_command_queue.empty()) {
				std::unique_ptr<ipc_client::Command> c{ std::move(m_command_queue.front()) };
				m_command_queue.pop_front();
				lock.unlock();

				if (c->type() != ipc_client::CommandType::GET_FRAME) {
					reject(std::move(c));
					lock.lock();
					continue;
				}

				std::unique_ptr<ipc_client::CommandGetFrame> get{ static_cast<ipc_client::CommandGetFrame *>(c.release()) };
				service_remote_getframe(std::move(get));
				lock.lock();
			}
		}

		reject_commands();
		if (m_runloop_response)
			send_ack(m_runloop_response->transaction_id());

		return std::move(m_runloop_response);
	}
public:
	AVSProxy(void * = nullptr) :
		m_script_result{},
		m_vi{},
		m_active_request{},
		m_runloop_response_received{},
		m_remote_exit{}
	{}

	const char *get_name(void *) noexcept override { return "Avisynth 32-bit proxy"; }

	void init(const ConstMap &in, const Map &out, const Core &core) override
	{
		Plugin this_plugin = core.get_plugin_by_id(PLUGIN_ID);

		std::string script = in.get_prop<std::string>("script");
		std::wstring avisynth_path = utf8_to_utf16(in.get_prop<std::string>("avisynth", map::Ignore{}));
		std::wstring slave_path = utf8_to_utf16(in.get_prop<std::string>("slave", map::Ignore{}));

		if (slave_path.empty()) {
			std::string plugin_path = this_plugin.path();
			slave_path = utf8_to_utf16(plugin_path.substr(0, plugin_path.find_last_of('/')) + "/avshost_native.exe");
		}

		m_client = std::make_unique<ipc_client::IPCClient>(ipc_client::IPCClient::master(), slave_path.c_str());
		m_client->start(std::bind(&AVSProxy::recv_callback, this, std::placeholders::_1));

		if (in.contains("slave_log")) {
			std::wstring log_path = utf8_to_utf16(in.get_prop<std::string>("slave_log"));
			m_client->send_async(std::make_unique<ipc_client::CommandSetLogFile>(log_path));
		}

		std::unique_ptr<ipc_client::Command> response;

		response = m_client->send_sync(std::make_unique<ipc_client::CommandLoadAvisynth>(avisynth_path.c_str()));
		expect_ack(std::move(response));

		if (in.contains("clips")) {
			size_t num_clips = in.num_elements("clips");

			if (!in.contains("clip_names") || in.num_elements("clip_names") != num_clips)
				throw std::runtime_error{ "clips and clip_names must have same number of elements" };

			for (size_t i = 0; i < num_clips; ++i) {
				FilterNode node = in.get_prop<FilterNode>("clips", static_cast<int>(i));
				std::string name = in.get_prop<std::string>("clip_names", static_cast<int>(i));

				ipc::Value value{ ipc::Value::CLIP };
				value.c.clip_id = static_cast<int>(i);
				value.c.vi = serialize_video_info(node.video_info());

				response = m_client->send_sync(std::make_unique<ipc_client::CommandSetScriptVar>(name, value));
				expect_ack(std::move(response));

				m_clips[static_cast<int>(i)] = std::move(node);
			}
		}

		uint32_t heap_script = local_to_heap_str(m_client.get(), script.c_str(), script.size());
		std::unique_ptr<ipc_client::Command> eval_command;

		try {
			eval_command = std::make_unique<ipc_client::CommandEvalScript>(heap_script);
		} catch (...) {
			m_client->deallocate(m_client->offset_to_pointer(heap_script));
			throw;
		}

		response = runloop(std::move(eval_command));
		response = expect_response(std::move(response), ipc_client::CommandType::SET_SCRIPT_VAR);

		m_script_result = static_cast<ipc_client::CommandSetScriptVar *>(response.get())->value();
		response->relinquish_heap_resources();

		switch (m_script_result.type) {
		// Create a filter if the result was a clip.
		case ipc::Value::CLIP: {
			m_vi = deserialize_video_info(m_script_result.c.vi, core);

			FilterDependencyBuilder deps = make_deps();
			for (const auto &entry : m_clips) {
				deps.add_dep(entry.second);
			}
			create_video_filter(out, m_vi, fmFrameState, deps, core);
			break;
		}
		// Otherwise return the result directly.
		case ipc::Value::BOOL_:
			out.set_prop("result", m_script_result.b); break;
		case ipc::Value::INT:
			out.set_prop("result", m_script_result.i); break;
		case ipc::Value::FLOAT:
			out.set_prop("result", m_script_result.f); break;
		case ipc::Value::STRING:
			// Strings need to be deallocated.
			try {
				out.set_prop("result", heap_to_local_str(m_client.get(), m_script_result.s));
			} catch (...) {
				m_client->deallocate(m_client->offset_to_pointer(m_script_result.s));
				throw;
			}
			m_client->deallocate(m_client->offset_to_pointer(m_script_result.s));
			break;
		default:
			break;
		}
	}

	ConstFrame get_frame_initial(int n, const Core &core, const FrameContext &, void *) override
	{
		try {
			std::unique_ptr<ipc_client::Command> response = runloop(
				std::make_unique<ipc_client::CommandGetFrame>(ipc::VideoFrameRequest{ m_script_result.c.clip_id, n }));
			response = expect_response(std::move(response), ipc_client::CommandType::SET_FRAME);

			ipc_client::CommandSetFrame *set_frame = static_cast<ipc_client::CommandSetFrame *>(response.get());
			ConstFrame result;

			try {
				result = heap_to_local_frame(m_client.get(), m_vi, m_script_result.c.vi.color_family, set_frame->arg(), core);
			} catch (...) {
				response->deallocate_heap_resources(m_client.get());
				throw;
			}

			response->deallocate_heap_resources(m_client.get());
			return result;
		} catch (const ipc_client::IPCError &) {
			fatal();
			throw;
		}
	}

	ConstFrame get_frame(int n, const Core &core, const FrameContext &, void *) override
	{
		return nullptr;
	}
};

const PluginInfo4 g_plugin_info4{
	PLUGIN_ID, "avsw", "avsproxy", 0, {
		{ &FilterBase::filter_create<AVSProxy>, "Eval", "script:data;clips:vnode[]:opt;clip_names:data[]:opt;avisynth:data:opt;slave:data:opt;slave_log:data:opt;", "any" }
	}
};
