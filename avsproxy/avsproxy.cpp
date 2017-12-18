#include <algorithm>
#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
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

ipc::VideoFrame local_to_heap_frame(ipc_client::IPCClient *client, uint32_t clip_id, int32_t n, const ::VSVideoInfo &vi, int32_t color_family, const ConstVideoFrame &frame)
{
	throw std::runtime_error{ "not implemented" };
}

} // namespace


class AVSProxy : public vsxx::FilterBase {
	std::unique_ptr<ipc_client::IPCClient> m_client;
	ipc::Value m_script_result;
	::VSVideoInfo m_vi;

	void throw_on_error(ipc_client::Command *c, ipc_client::CommandType expected_type = ipc_client::CommandType::ACK) const
	{
		if (!c)
			throw std::runtime_error{ "no response received for command" };
		if (c->type() == ipc_client::CommandType::ERR)
			throw std::runtime_error{ "command failed" };
		if (c->type() != expected_type)
			throw std::runtime_error{ "unexpected resposne received for command" };
	}

	void recv_callback(std::unique_ptr<ipc_client::Command> c) {}
public:
	explicit AVSProxy(void *) : m_script_result{}, m_vi{} {}

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

	ConstVideoFrame get_frame_initial(int n, const VapourCore &core, ::VSFrameContext *frame_ctx) override
	{
		auto result = m_client->send_sync(
			std::make_unique<ipc_client::CommandGetFrame>(ipc::VideoFrameRequest{ m_script_result.c.clip_id, n }));
		throw_on_error(result.get(), ipc_client::CommandType::SET_FRAME);

		ipc_client::CommandSetFrame *set_frame = static_cast<ipc_client::CommandSetFrame *>(result.get());
		if (set_frame->arg().request.clip_id != m_script_result.c.clip_id || set_frame->arg().request.frame_number != n)
			throw std::runtime_error{ "remote get frame returned wrong frame" };

		return heap_to_local_frame(m_client.get(), m_vi, m_script_result.c.vi.color_family, set_frame->arg(), core);
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
			"avisynth:data:opt;"
			"slave:data:opt;"
		}
	}
};
