#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <VSHelper.h>
#include <Windows.h>
#include "ipc/ipc_client.h"
#include "ipc/ipc_commands.h"
#include "ipc/video_types.h"
#include "ipc/win32util.h"
#include "libp2p/p2p_api.h"
#include "vsxx/vsxx_pluginmain.h"

using namespace vsxx;

namespace {

constexpr char PLUGIN_ID[] = "xxx.abc.avsproxy";

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

	try {
		std::string ret;

		ret.resize(ipc::deserialize_str(nullptr, ptr, -1));
		ipc::deserialize_str(&ret[0], ptr, -1);

		client->deallocate(ptr);
		return ret;
	} catch (...) {
		client->deallocate(ptr);
		throw;
	}
}

uint32_t local_to_heap_str(ipc_client::IPCClient *client, const char *str, size_t len)
{
	size_t size = ipc::serialize_str(nullptr, str, len);
	void *ptr = client->allocate(size);
	ipc::serialize_str(ptr, str, len);
	return client->pointer_to_offset(ptr);
}

::VSVideoInfo deserialize_video_info(const ipc::VideoInfo &ipc_vi, const VapourCore &core)
{
	::VSVideoInfo vi{};

	vi.fpsNum = ipc_vi.fps_num;
	vi.fpsDen = ipc_vi.fps_den;
	vi.width = ipc_vi.width;
	vi.height = ipc_vi.height;
	vi.numFrames = ipc_vi.num_frames;

	muldivRational(&vi.fpsNum, &vi.fpsDen, 1, 1);

	switch (ipc_vi.color_family) {
	case ipc::VideoInfo::RGB:
		vi.format = core.register_format(cmRGB, ::stInteger, 8, ipc_vi.subsample_w, ipc_vi.subsample_h);
		break;
	case ipc::VideoInfo::YUV:
		vi.format = core.register_format(cmYUV, ::stInteger, 8, ipc_vi.subsample_w, ipc_vi.subsample_h);
		break;
	case ipc::VideoInfo::GRAY:
		vi.format = core.format_preset(::pfGray8);
		break;
	case ipc::VideoInfo::RGB24:
	case ipc::VideoInfo::RGB32:
		vi.format = core.format_preset(::pfRGB24);
		break;
	case ipc::VideoInfo::YUY2:
		vi.format = core.format_preset(::pfYUV422P8);
		break;
	default:
		break;
	}

	if (!vi.format)
		throw std::runtime_error{ "color format not supported" };

	return vi;
}

ipc::VideoInfo serialize_video_info(const ::VSVideoInfo &vi)
{
	if (!isConstantFormat(&vi))
		throw std::runtime_error{ "constant format required" };
	if (vi.format->bitsPerSample != 8)
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

	if (vi.format->id == ::pfCompatBGR32) {
		ipc_vi.color_family = ipc::VideoInfo::RGB32;
	} else if (vi.format->id == ::pfCompatYUY2) {
		ipc_vi.color_family = ipc::VideoInfo::YUY2;
	} else {
		switch (vi.format->colorFamily) {
		case ::cmRGB:
			ipc_vi.color_family = ipc::VideoInfo::RGB32;
			break;
		case ::cmYUV:
		case ::cmYCoCg:
			ipc_vi.color_family = ipc::VideoInfo::YUV;
			break;
		case ::cmGray:
			ipc_vi.color_family = ipc::VideoInfo::GRAY;
			break;
		default:
			throw std::runtime_error{ "color format not supported" };
		}

		ipc_vi.subsample_w = vi.format->subSamplingW;
		ipc_vi.subsample_h = vi.format->subSamplingH;
	}

	return ipc_vi;
}

VideoFrame heap_to_local_frame(ipc_client::IPCClient *client, const ::VSVideoInfo &vi, int32_t color_family, const ipc::VideoFrame &ipc_frame, const VapourCore &core)
{
	unsigned char *heap_ptr = static_cast<unsigned char *>(client->offset_to_pointer(ipc_frame.heap_offset));
	int src_planes = vi.format->numPlanes;

	if (color_family == ipc::VideoInfo::RGB24 || color_family == ipc::VideoInfo::RGB32 || color_family == ipc::VideoInfo::YUY2)
		src_planes = 1;

	try {
		for (int p = 0; p < src_planes; ++p) {
			int row_size = (vi.width >> (p ? vi.format->subSamplingW : 0)) * vi.format->bytesPerSample;

			if (color_family == ipc::VideoInfo::RGB24)
				row_size *= 3;
			else if (color_family == ipc::VideoInfo::RGB32)
				row_size *= 4;
			else if (color_family == ipc::VideoInfo::YUY2)
				row_size *= 2;

			if (ipc_frame.stride[p] < row_size)
				throw std::runtime_error{ "wrong width" };
			if (ipc_frame.height[p] != (vi.height >> (p ? vi.format->subSamplingH : 0)))
				throw std::runtime_error{ "wrong height" };
		}

		VideoFrame frame = core.new_video_frame(*vi.format, vi.width, vi.height);
		VideoFrame alpha;

		if (color_family == ipc::VideoInfo::RGB32)
			alpha = core.new_video_frame(*core.format_preset(::pfGray8), vi.width, vi.height);

		if (color_family == ipc::VideoInfo::RGB24 || color_family == ipc::VideoInfo::RGB32 || color_family == ipc::VideoInfo::YUY2) {
			p2p_buffer_param param{};

			param.src[0] = heap_ptr;
			param.src_stride[0] = ipc_frame.stride[0];

			for (int p = 0; p < vi.format->numPlanes; ++p) {
				if (color_family == ipc::VideoInfo::RGB24 || color_family == ipc::VideoInfo::RGB32) {
					param.dst[p] = frame.write_ptr(p) + (frame.height(p) - 1) * frame.stride(p);
					param.dst_stride[p] = -frame.stride(p);
				} else {
					param.dst[p] = frame.write_ptr(p);
					param.dst_stride[p] = frame.stride(p);
				}
			}

			if (color_family == ipc::VideoInfo::RGB32) {
				alpha = core.new_video_frame(*core.format_preset(::pfGray8), vi.width, vi.height);
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

			for (int p = 0; p < vi.format->numPlanes; ++p) {
				int row_size = (vi.width >> (p ? vi.format->subSamplingW : 0)) * vi.format->bytesPerSample;

				vs_bitblt(frame.write_ptr(p), frame.stride(p), src_ptr, ipc_frame.stride[p], row_size, ipc_frame.height[p]);
				src_ptr += ipc_frame.stride[p] * ipc_frame.height[p];
			}
		}

		if (alpha)
			frame.frame_props().set_prop("_Alpha", alpha);

		client->deallocate(heap_ptr);
		return frame;
	} catch (...) {
		client->deallocate(heap_ptr);
		throw;
	}
}

ipc::VideoFrame local_to_heap_frame(ipc_client::IPCClient *client, uint32_t clip_id, int32_t n, const ::VSVideoInfo &vi, const ConstVideoFrame &frame)
{
	ipc::VideoFrame ipc_frame{ { clip_id, n } };
	size_t size = 0;

	if (vi.format->colorFamily == ::cmRGB || vi.format->id == ::pfCompatBGR32 || vi.format->id == ::pfCompatYUY2) {
		int rowsize = vi.width * (vi.format->id == ::pfCompatYUY2 ? 2 : 4);
		ipc_frame.stride[0] = rowsize % 64 ? rowsize + 64 - rowsize % 64 : rowsize;
		ipc_frame.height[0] = vi.height;
		size = ipc_frame.stride[0] * vi.height;
	} else {
		for (int p = 0; p < vi.format->numPlanes; ++p) {
			int rowsize = frame.width(p);
			ipc_frame.stride[p] = rowsize % 64 ? rowsize + 64 - rowsize % 64 : rowsize;
			ipc_frame.height[p] = frame.height(p);
			size += ipc_frame.stride[p] * ipc_frame.height[p];
		}
	}

	unsigned char *dst_ptr = static_cast<unsigned char *>(client->allocate(size));
	ipc_frame.heap_offset = client->pointer_to_offset(dst_ptr);

	if (vi.format->colorFamily == ::cmRGB) {
		ConstVideoFrame alpha = frame.frame_props_ro().get_prop<ConstVideoFrame>("_Alpha", map::Ignore{});
		p2p_buffer_param param{};

		param.src[0] = frame.read_ptr(0);
		param.src[1] = frame.read_ptr(1);
		param.src[2] = frame.read_ptr(2);
		param.src[3] = alpha ? alpha.read_ptr(0) : nullptr;

		param.src_stride[0] = frame.stride(0);
		param.src_stride[1] = frame.stride(1);
		param.src_stride[2] = frame.stride(2);
		param.src_stride[3] = alpha ? alpha.stride(0) : 0;

		param.dst[0] = dst_ptr;
		param.dst_stride[0] = ipc_frame.stride[0];

		param.width = vi.width;
		param.height = vi.height;
		param.packing = p2p_argb32_le;

		p2p_pack_frame(&param, P2P_ALPHA_SET_ONE);
	} else {
		for (int p = 0; p < vi.format->numPlanes; ++p) {
			int rowsize = frame.width(p);

			if (vi.format->id == ::pfCompatBGR32)
				rowsize *= 4;
			else if (vi.format->id == ::pfCompatYUY2)
				rowsize *= 2;

			vs_bitblt(dst_ptr, ipc_frame.stride[p], frame.read_ptr(p), frame.stride(p), rowsize, frame.height(p));
			dst_ptr += rowsize * frame.height(p);
		}
	}

	return ipc_frame;
}

} // namespace


class AVSProxy : public vsxx::FilterBase {
	struct clip_info {
		FilterNode node;
		std::deque<int32_t> requested;
	};

	std::unique_ptr<ipc_client::IPCClient> m_client;
	std::unordered_map<uint32_t, clip_info> m_clips;
	ipc::Value m_script_result;
	::VSVideoInfo m_vi;

	std::deque<std::unique_ptr<ipc_client::Command>> m_command_queue;
	std::unique_ptr<ipc_client::Command> m_getframe_response;
	std::mutex m_mutex;
	std::condition_variable m_cond;
	std::atomic_uint32_t m_active_request;
	std::atomic_bool m_getframe_response_received;
	std::atomic_bool m_remote_exit;

	void fatal()
	{
		m_client->stop();
		m_remote_exit = true;
	}

	void throw_on_error(ipc_client::Command *c, ipc_client::CommandType expected_type = ipc_client::CommandType::ACK) const
	{
		if (!c)
			throw std::runtime_error{ "no response received for command" };
		if (c->type() == ipc_client::CommandType::ERR)
			throw std::runtime_error{ "command failed" };
		if (c->type() != expected_type)
			throw std::runtime_error{ "unexpected resposne received for command" };
	}

	void recv_callback(std::unique_ptr<ipc_client::Command> c)
	{
		if (!c) {
			m_remote_exit = true;
			m_cond.notify_all();
			return;
		}

		{
			std::lock_guard<std::mutex> lock{ m_mutex };
			m_command_queue.emplace_back(std::move(c));
		}
		m_cond.notify_all();
		return;
	}

	void getframe_callback(uint32_t request, std::unique_ptr<ipc_client::Command> c)
	{
		if (m_active_request == request) {
			m_getframe_response_received = true;
			m_getframe_response = std::move(c);

			m_cond.notify_all();
		} else {
			send_err(c->transaction_id());
		}
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

	void reject_commands()
	{
		std::lock_guard<std::mutex> lock{ m_mutex };

		// Reject any slave activity from a previous frame.
		while (!m_command_queue.empty()) {
			std::unique_ptr<ipc_client::Command> c{ std::move(m_command_queue.front()) };
			m_command_queue.pop_front();
			send_err(c->transaction_id());
		}
	}

	void service_remote_getframe(std::unique_ptr<ipc_client::CommandGetFrame> c)
	{
		auto it = m_clips.find(c->arg().clip_id);
		if (it == m_clips.end()) {
			send_err(c->transaction_id());
			return;
		}

		ConstVideoFrame frame;

		try {
			frame = it->second.node.get_frame(c->arg().frame_number);
		} catch (...) {
			send_err(c->transaction_id());
			return;
		}

		ipc::VideoFrame ipc_frame = local_to_heap_frame(m_client.get(), c->arg().clip_id, c->arg().frame_number, it->second.node.video_info(), frame);
		std::unique_ptr<ipc_client::Command> response = std::make_unique<ipc_client::CommandSetFrame>(ipc_frame);
		response->set_response_id(c->transaction_id());
		m_client->send_async(std::move(response));
	}

	std::unique_ptr<ipc_client::CommandSetFrame> get_frame_runloop(int n)
	{
		if (m_remote_exit)
			throw std::runtime_error{ "remote process exited" };

		reject_commands();

		m_getframe_response.reset();
		m_getframe_response_received = false;
		m_client->send_async(
			std::make_unique<ipc_client::CommandGetFrame>(ipc::VideoFrameRequest{ m_script_result.c.clip_id, n }),
			std::bind(&AVSProxy::getframe_callback, this, ++m_active_request, std::placeholders::_1));

		std::unique_lock<std::mutex> lock{ m_mutex };

		while (true) {
			m_cond.wait(lock);

			if (m_remote_exit)
				throw std::runtime_error{ "remote process exited" };

			if (m_getframe_response_received)
				break;

			while (!m_command_queue.empty()) {
				std::unique_ptr<ipc_client::Command> c{ std::move(m_command_queue.front()) };
				m_command_queue.pop_front();

				if (c->type() != ipc_client::CommandType::GET_FRAME) {
					send_err(c->transaction_id());
					continue;
				}

				std::unique_ptr<ipc_client::CommandGetFrame> get{ static_cast<ipc_client::CommandGetFrame *>(c.release()) };
				service_remote_getframe(std::move(get));
			}
		}

		lock.unlock();
		reject_commands();

		if (m_getframe_response)
			send_ack(m_getframe_response->transaction_id());

		throw_on_error(m_getframe_response.get(), ipc_client::CommandType::SET_FRAME);

		return std::unique_ptr<ipc_client::CommandSetFrame>{
			static_cast<ipc_client::CommandSetFrame *>(m_getframe_response.release())
		};
	}
public:
	explicit AVSProxy(void *) :
		m_script_result{},
		m_vi{},
		m_active_request{},
		m_getframe_response_received{},
		m_remote_exit {}
	{}

	const char *get_name(int index) noexcept override { return "Avisynth 32-bit proxy"; }

	std::pair<::VSFilterMode, int> init(const ConstPropertyMap &in, const PropertyMap &out, const VapourCore &core) override
	{
		Plugin this_plugin{ get_vsapi()->getPluginById(PLUGIN_ID, core.get()) };

		std::string script = in.get_prop<std::string>("script");
		std::wstring avisynth_path = utf8_to_utf16(in.get_prop<std::string>("avisynth", map::Ignore{}));
		std::wstring slave_path = utf8_to_utf16(in.get_prop<std::string>("slave", map::Ignore{}));

		if (slave_path.empty()) {
			std::string plugin_path = this_plugin.path();
			slave_path = utf8_to_utf16(plugin_path.substr(0, plugin_path.find_last_of('/')) + "/avshost_native.exe");
		}

		m_client = std::make_unique<ipc_client::IPCClient>(ipc_client::IPCClient::master(), slave_path.c_str());
		m_client->start(std::bind(&AVSProxy::recv_callback, this, std::placeholders::_1));

		std::unique_ptr<ipc_client::Command> response;

		response = m_client->send_sync(std::make_unique<ipc_client::CommandLoadAvisynth>(avisynth_path.c_str()));
		throw_on_error(response.get());

		if (in.contains("clips")) {
			size_t num_clips = in.num_elements("clips");

			if (!in.contains("clip_names") || in.num_elements("clip_names") != num_clips)
				throw std::runtime_error{ "clips and num_clips must have same number of elements" };

			for (size_t i = 0; i < num_clips; ++i) {
				FilterNode node = in.get_prop<FilterNode>("clips", static_cast<int>(i));
				std::string name = in.get_prop<std::string>("clip_names", static_cast<int>(i));

				ipc::Value value{ ipc::Value::CLIP };
				value.c.clip_id = static_cast<int>(i);
				value.c.vi = serialize_video_info(node.video_info());

				response = m_client->send_sync(std::make_unique<ipc_client::CommandSetScriptVar>(name, value));
				throw_on_error(response.get());

				m_clips[static_cast<int>(i)].node = std::move(node);
			}
		}

		uint32_t heap_script = local_to_heap_str(m_client.get(), script.c_str(), script.size());
		response = m_client->send_sync(std::make_unique<ipc_client::CommandEvalScript>(heap_script));
		throw_on_error(response.get(), ipc_client::CommandType::SET_SCRIPT_VAR);

		m_script_result = static_cast<ipc_client::CommandSetScriptVar *>(response.get())->value();
		if (m_script_result.type == ipc::Value::CLIP)
			m_vi = deserialize_video_info(m_script_result.c.vi, core);
		else
			m_vi.numFrames = 1;

		return{ ::fmSerial, 0 };
	}

	void post_init(const ConstPropertyMap &in, const PropertyMap &out, const VapourCore &core) override
	{
		// Delete "clip" if the script did not return a clip.
		if (m_script_result.type == ipc::Value::CLIP)
			return;

		// Increase ref count temporarily.
		FilterNode self = out.get_prop<FilterNode>("clip");
		out.erase("clip");

		switch (m_script_result.type) {
		case ipc::Value::BOOL_:
			out.set_prop("result", m_script_result.b);
			break;
		case ipc::Value::INT:
			out.set_prop("result", m_script_result.i);
			break;
		case ipc::Value::FLOAT:
			out.set_prop("result", m_script_result.f);
			break;
		case ipc::Value::STRING:
			out.set_prop("result", heap_to_local_str(m_client.get(), m_script_result.s));
			break;
		default:
			break;
		}
	}

	std::pair<const ::VSVideoInfo *, size_t> get_video_info() noexcept override
	{
		return{ &m_vi, 1 };
	}

	ConstVideoFrame get_frame_initial(int n, const VapourCore &core, ::VSFrameContext *) override
	{
		try {
			std::unique_ptr<ipc_client::CommandSetFrame> response = get_frame_runloop(n);
			return heap_to_local_frame(m_client.get(), m_vi, m_script_result.c.vi.color_family, response->arg(), core);
		} catch (const ipc_client::IPCError &) {
			fatal();
			throw;
		}
	}

	ConstVideoFrame get_frame(int n, const VapourCore &core, ::VSFrameContext *frame_ctx) override
	{
		return ConstVideoFrame{};
	}
};

const PluginInfo g_plugin_info{
	PLUGIN_ID, "avsw", "avsproxy", {
		{ &vsxx::FilterBase::filter_create<AVSProxy>, "eval",
			"script:data;"
			"clips:clip[]:opt;"
			"clip_names:data[]:opt;"
			"avisynth:data:opt;"
			"slave:data:opt;"
		}
	}
};
