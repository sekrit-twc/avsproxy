#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <new>
#include <tuple>
#include <utility>
#include <Windows.h>
#include "ipc/ipc_client.h"
#include "ipc/logging.h"
#include "ipc/video_types.h"

#ifdef AVISYNTH_PLUS
  #include <avisynth.h>
#else
  #include "avisynth_2.6.h"
#endif

#include "avshost.h"

const AVS_Linkage *AVS_linkage;
bool g_avisynth_plus;

namespace avs {

namespace {

bool is_avisynth_plus()
{
	::VideoInfo vi{};
	vi.pixel_type = -536805376; // CS_Y16
	return vi.BitsPerPixel() == 16;
}

char *save_string(::IScriptEnvironment *env, const std::string &s)
{
	return env->SaveString(s.c_str(), static_cast<int>(s.size()));
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

::VideoInfo deserialize_video_info(const ipc::VideoInfo &ipc_vi)
{
	::VideoInfo vi{};

	vi.width = ipc_vi.width;
	vi.height = ipc_vi.height;
	vi.fps_numerator = ipc_vi.fps_num;
	vi.fps_denominator = ipc_vi.fps_den;

	switch (ipc_vi.color_family) {
	case ipc::VideoInfo::YUV:
		if (ipc_vi.subsample_w == 0 && ipc_vi.subsample_h == 0)
			vi.pixel_type = ::VideoInfo::CS_YV24;
		else if (ipc_vi.subsample_w == 1 && ipc_vi.subsample_h == 0)
			vi.pixel_type = ::VideoInfo::CS_YV16;
		else if (ipc_vi.subsample_w == 1 && ipc_vi.subsample_h == 1)
			vi.pixel_type = ::VideoInfo::CS_YV12;
		else if (ipc_vi.subsample_w == 2 && ipc_vi.subsample_h == 0)
			vi.pixel_type = ::VideoInfo::CS_YV411;
		else
			throw AvisynthError_{ "color format not supported" };
		break;
	case ipc::VideoInfo::GRAY:
		vi.pixel_type = ::VideoInfo::CS_Y8;
		break;
	case ipc::VideoInfo::RGB24:
		vi.pixel_type = ::VideoInfo::CS_BGR24;
		break;
	case ipc::VideoInfo::RGB32:
		vi.pixel_type = ::VideoInfo::CS_BGR32;
		break;
	case ipc::VideoInfo::YUY2:
		vi.pixel_type = ::VideoInfo::CS_YUY2;
		break;
	case ipc::VideoInfo::RGB:
	default:
		throw AvisynthError_{ "color format not supported" };
	}

	vi.num_frames = ipc_vi.num_frames;
	return vi;
}

ipc::VideoInfo serialize_video_info(const ::VideoInfo &vi)
{
	ipc::VideoInfo ipc_vi{};

	ipc_vi.width = vi.width;
	ipc_vi.height = vi.height;
	ipc_vi.fps_num = vi.fps_numerator;
	ipc_vi.fps_den = vi.fps_denominator;
	ipc_vi.num_frames = vi.num_frames;

	if (vi.IsRGB24()) {
		ipc_vi.color_family = ipc::VideoInfo::RGB24;
	} else if (vi.IsRGB32()) {
		ipc_vi.color_family = ipc::VideoInfo::RGB32;
	} else if (vi.IsYUY2()) {
		ipc_vi.color_family = ipc::VideoInfo::YUY2;
	} else if (vi.IsY8()) {
		ipc_vi.color_family = ipc::VideoInfo::GRAY;
	} else if (vi.IsPlanar()) {
		ipc_vi.color_family = ipc::VideoInfo::YUV;

		if (vi.IsYV24()) {
			ipc_vi.subsample_w = 0;
			ipc_vi.subsample_h = 0;
		} else if (vi.IsYV16()) {
			ipc_vi.subsample_w = 1;
			ipc_vi.subsample_h = 0;
		} else if (vi.IsYV12()) {
			ipc_vi.subsample_w = 1;
			ipc_vi.subsample_h = 1;
		} else if (vi.IsYV411()) {
			ipc_vi.subsample_w = 2;
			ipc_vi.subsample_h = 0;
		} else {
			throw AvisynthError_{ "color format not supported" };
		}
	} else {
		throw AvisynthError_{ "color format not supported" };
	}

	return ipc_vi;
}

::PVideoFrame heap_to_local_frame(ipc_client::IPCClient *client, const ::VideoInfo &vi, const ipc::VideoFrame &ipc_frame, ::IScriptEnvironment *env)
{
	constexpr int plane_order[3] = { PLANAR_Y, PLANAR_U, PLANAR_V };

	void *heap_ptr = client->offset_to_pointer(ipc_frame.heap_offset);
	int num_planes = vi.IsPlanar() && !vi.IsY8() ? 3 : 1;

	try {
		for (int p = 0; p < num_planes; ++p) {
			if (vi.RowSize(plane_order[p]) > ipc_frame.stride[p])
				throw AvisynthError_{ "wrong width" };
			if (ipc_frame.height[p] != vi.height >> (p ? vi.GetPlaneHeightSubsampling(plane_order[p]) : 0))
				throw AvisynthError_{ "wrong height" };
		}

		const unsigned char *src_ptr = static_cast<const unsigned char *>(heap_ptr);
		::PVideoFrame frame = env->NewVideoFrame(vi);

		for (int p = 0; p < num_planes; ++p) {
			int avs_plane = plane_order[p];

			env->BitBlt(frame->GetWritePtr(avs_plane), frame->GetPitch(avs_plane), src_ptr, ipc_frame.stride[p], frame->GetRowSize(avs_plane), frame->GetHeight(avs_plane));
			src_ptr += ipc_frame.stride[p] * ipc_frame.height[p];
		}

		client->deallocate(heap_ptr);
		return frame;
	} catch (...) {
		client->deallocate(heap_ptr);
		throw;
	}
}

ipc::VideoFrame local_to_heap_frame(ipc_client::IPCClient *client, uint32_t clip_id, int32_t n, const ::VideoInfo &vi, const ::PVideoFrame &frame, ::IScriptEnvironment *env)
{
	constexpr int plane_order[3] = { PLANAR_Y, PLANAR_U, PLANAR_V };

	ipc::VideoFrame ipc_frame{ { clip_id, n } };
	size_t size = 0;

	int num_planes = vi.IsPlanar() && !vi.IsY8() ? 3 : 1;

	for (int p = 0; p < num_planes; ++p) {
		int rowsize = vi.RowSize(plane_order[p]);
		ipc_frame.stride[p] = rowsize % 64 ? rowsize + 64 - rowsize % 64 : rowsize;
		ipc_frame.height[p] = frame->GetHeight(plane_order[p]);

		size += ipc_frame.stride[p] * ipc_frame.height[p];
	}

	unsigned char *dst_ptr = static_cast<unsigned char *>(client->allocate(size));
	ipc_frame.heap_offset = client->pointer_to_offset(dst_ptr);

	for (int p = 0; p < num_planes; ++p) {
		int avs_plane = plane_order[p];
		env->BitBlt(dst_ptr, ipc_frame.stride[p], frame->GetReadPtr(avs_plane), frame->GetPitch(avs_plane), frame->GetRowSize(avs_plane), frame->GetHeight(avs_plane));
		dst_ptr += ipc_frame.stride[p] * ipc_frame.height[p];
	}

	return ipc_frame;
}

} // namespace


#define AVS_EX_BEGIN try {

#define AVS_EX_END \
  } catch (const ::AvisynthError &e) { \
    throw AvisynthError_{ e.msg }; \
  } catch (const ::IScriptEnvironment::NotFound &) { \
    throw AvisynthError_{ "function or variable not defined"}; \
  }


class Cache {
	static constexpr size_t MEMORY_MAX = 8 * (1 << 20UL);

	std::deque<std::tuple<uint32_t, int, ::PVideoFrame>> m_cache;
	size_t m_memory_usage;
public:
	Cache() : m_memory_usage{} {}

	void insert(uint32_t clip_id, int n, ::PVideoFrame frame)
	{
		size_t size = frame->GetFrameBuffer()->GetDataSize();

		if (size > MEMORY_MAX)
			return;

		while (MEMORY_MAX - m_memory_usage < size) {
			::PVideoFrame frame = std::get<2>(m_cache.back());
			m_cache.pop_back();
			m_memory_usage -= frame->GetFrameBuffer()->GetDataSize();
		}

		m_cache.emplace_back(clip_id, n, frame);
		m_memory_usage += size;
	}

	::PVideoFrame find(uint32_t clip_id, int n)
	{
		auto it = std::find_if(m_cache.begin(), m_cache.end(), [=](const std::tuple<uint32_t, int, ::PVideoFrame> &x)
		{
			return std::get<0>(x) == clip_id && std::get<1>(x) == n;
		});

		if (it == m_cache.end())
			return nullptr;

		auto val = *it;
		m_cache.erase(it);
		m_cache.emplace_front(val);
		return std::get<2>(val);
	}
};


class VirtualClip : public ::IClip {
	ipc_client::IPCClient *m_client;
	Cache *m_cache;
	uint32_t m_clip_id;
	::VideoInfo m_vi;
public:
	VirtualClip(ipc_client::IPCClient *client, Cache *cache, uint32_t clip_id, const ::VideoInfo &vi) :
		m_client{ client },
		m_cache{ cache },
		m_clip_id{ clip_id },
		m_vi(vi)
	{}

	::PVideoFrame __stdcall GetFrame(int n, ::IScriptEnvironment *env) override
	{
		::PVideoFrame frame = m_cache->find(m_clip_id, n);

		if (!frame) {
			ipc_log("clip %u frame %d not prefetched\n", m_clip_id, n);

			auto response = m_client->send_sync(std::make_unique<ipc_client::CommandGetFrame>(ipc::VideoFrameRequest{ m_clip_id, n }));
			if (!response || response->type() != ipc_client::CommandType::SET_FRAME)
				env->ThrowError("remote get frame failed");

			ipc_client::CommandSetFrame *set_frame = static_cast<ipc_client::CommandSetFrame *>(response.get());
			if (set_frame->arg().request.clip_id != m_clip_id || set_frame->arg().request.frame_number != n)
				env->ThrowError("remote get frame returned wrong frame");

			frame = heap_to_local_frame(m_client, m_vi, set_frame->arg(), env);
			m_cache->insert(m_clip_id, n, frame);
		}

		return frame;
	}

	bool __stdcall GetParity(int n) override { return false; }
	void __stdcall GetAudio(void *, __int64, __int64, ::IScriptEnvironment *) override {}
	int __stdcall SetCacheHints(int, int) override { return 0; }
	const ::VideoInfo & __stdcall GetVideoInfo() override { return m_vi; }
};


void AvisynthHost::IScriptEnvironmentDeleter::operator()(::IScriptEnvironment *env)
{
	env->DeleteScriptEnvironment();
}


AvisynthHost::PClip_::PClip_() : m_val{}
{
	static_assert(sizeof(PClip_) == sizeof(PClip), "wrong size");
	static_assert(alignof(PClip_) == alignof(PClip), "wrong alignment");
	new (m_val.x) ::PClip{};
}

AvisynthHost::PClip_::PClip_(const ::PClip &p) : m_val{}
{
	new (m_val.x) ::PClip{ p };
}

AvisynthHost::PClip_::PClip_(const PClip_ &other) : m_val{}
{
	new (m_val.x) ::PClip{ other.get() };
}

AvisynthHost::PClip_::~PClip_() { get().~PClip(); }

AvisynthHost::PClip_ &AvisynthHost::PClip_::operator=(const AvisynthHost::PClip_ &other)
{
	get().~PClip();
	new (m_val.x) ::PClip{ other.get() };
	return *this;
}


AvisynthHost::AvisynthHost(ipc_client::IPCClient *client) :
	m_client{ client },
	m_create_script_env{},
	m_local_clip_id{}
{}

AvisynthHost::~AvisynthHost() = default;

int AvisynthHost::check_avs_loaded(ipc_client::Command *c)
{
	if (m_library)
		return 0;

	ipc_log("received command type %d before Avisynth loaded\n", c->type());

	if (c->transaction_id())
		send_err(c->transaction_id());
	return 1;
}

int AvisynthHost::observe(std::unique_ptr<ipc_client::CommandLoadAvisynth> c)
{
	if (m_library) {
		ipc_log0("Avisynth already loaded\n");

		if (c->transaction_id())
			send_err(c->transaction_id());

		return 1;
	}

	ipc_wlog("load avisynth DLL from '%s'\n", c->arg().c_str());

	m_library.reset(::LoadLibraryW(c->arg().empty() ? L"avisynth" : c->arg().c_str()));
	if (!m_library)
		win32::trap_error("error loading avisynth library");

	FARPROC proc = ::GetProcAddress(m_library.get(), "CreateScriptEnvironment");
	if (!proc) {
		m_library.reset();
		win32::trap_error("entry point not found");
	}

	m_create_script_env = reinterpret_cast<create_script_env>(proc);

	try {
		AVS_EX_BEGIN
		m_env.reset(m_create_script_env(AVISYNTH_INTERFACE_VERSION));
		AVS_linkage = m_env->GetAVSLinkage();
		g_avisynth_plus = is_avisynth_plus();
		AVS_EX_END

		if (!m_env)
			throw AvisynthError_{ "avisynth library has incompatible interface version" };

		m_cache = std::make_unique<Cache>();
	} catch (...) {
		m_library.reset();
		m_create_script_env = nullptr;
		throw;
	}

	return 0;
}

int AvisynthHost::observe(std::unique_ptr<ipc_client::CommandNewScriptEnv> c)
{
	if (int ret = check_avs_loaded(c.get()))
		return ret;

	ipc_log0("new script env\n");

	AVS_EX_BEGIN
	std::unique_ptr<::IScriptEnvironment, IScriptEnvironmentDeleter> env{ m_create_script_env(6) };
	if (!env)
		throw AvisynthError_{ "avisynth library has incompatible interface version" };

	m_local_clips.clear();
	m_remote_clips.clear();
	m_cache.reset();

	m_env = std::move(env);
	AVS_linkage = m_env->GetAVSLinkage();
	g_avisynth_plus = is_avisynth_plus();
	m_cache = std::make_unique<Cache>();
	AVS_EX_END

	return 0;
}

int AvisynthHost::observe(std::unique_ptr<ipc_client::CommandGetScriptVar> c)
{
	if (int ret = check_avs_loaded(c.get()))
		return ret;

	ipc_log("get script var '%s'\n", c->arg());

	AVS_EX_BEGIN
	::AVSValue result = m_env->GetVar(c->arg().c_str());
	if (!result.Defined())
		throw IScriptEnvironment::NotFound{};
	send_avsvalue(c->transaction_id(), result);
	AVS_EX_END

	return 1;
}

int AvisynthHost::observe(std::unique_ptr<ipc_client::CommandSetScriptVar> c)
{
	if (int ret = check_avs_loaded(c.get()))
		return ret;

	ipc_log("set script var '%s'\n", c->name().c_str());

	AVS_EX_BEGIN
	switch (c->value().type) {
	case ipc::Value::CLIP:
	{
		const ipc::VideoInfo &vi = c->value().c.vi;

		ipc_log("remote clip %u: %dx%d %d/%d/%d\n",
		        c->value().c.clip_id, vi.width, vi.height, vi.color_family, vi.subsample_w, vi.subsample_h);

		PClip clip = new VirtualClip{ m_client, m_cache.get(), c->value().c.clip_id, deserialize_video_info(vi) };
		m_env->SetVar(save_string(m_env.get(), c->name().c_str()), clip);
		m_remote_clips[c->value().c.clip_id] = clip;
		break;
	}
	case ipc::Value::BOOL_:
		m_env->SetVar(save_string(m_env.get(), c->name().c_str()), c->value().b);
		break;
	case ipc::Value::INT:
		m_env->SetVar(save_string(m_env.get(), c->name().c_str()), static_cast<int>(c->value().i));
		break;
	case ipc::Value::FLOAT:
		m_env->SetVar(save_string(m_env.get(), c->name().c_str()), static_cast<float>(c->value().f));
		break;
	case ipc::Value::STRING:
		m_env->SetVar(save_string(m_env.get(), c->name()), heap_to_local_str(m_client, c->value().s).c_str());
		break;
	default:
		throw AvisynthError_{ "unsupported variable type" };
	}
	AVS_EX_END

	return 0;
}

int AvisynthHost::observe(std::unique_ptr<ipc_client::CommandEvalScript> c)
{
	if (int ret = check_avs_loaded(c.get()))
		return ret;

	AVS_EX_BEGIN
	std::string script = heap_to_local_str(m_client, c->arg());

	ipc_log0("begin eval script\n");
	(ipc_log)("%s", script.c_str());
	ipc_log0("end eval script\n");

	::AVSValue result = m_env->Invoke("Eval", save_string(m_env.get(), script));
	send_avsvalue(c->transaction_id(), result);
	AVS_EX_END

	return 1;
}

int AvisynthHost::observe(std::unique_ptr<ipc_client::CommandGetFrame> c)
{
	if (int ret = check_avs_loaded(c.get()))
		return ret;

	ipc_log("GetFrame clip %u frame %u\n", c->arg().clip_id, c->arg().frame_number);

	auto it = m_local_clips.find(c->arg().clip_id);
	if (it == m_local_clips.end()) {
		ipc_log0("invalid local clip id\n");
		send_err(c->transaction_id());
		return 1;
	}

	AVS_EX_BEGIN
	const ::PClip &clip = it->second.get();
	::PVideoFrame frame = clip->GetFrame(c->arg().frame_number, m_env.get());
	ipc::VideoFrame ipc_frame = local_to_heap_frame(m_client, c->arg().clip_id, c->arg().frame_number, clip->GetVideoInfo(), frame, m_env.get());

	auto result = std::make_unique<ipc_client::CommandSetFrame>(ipc_frame);
	if (c->transaction_id() != ipc_client::INVALID_TRANSACTION)
		result->set_response_id(c->transaction_id());

	m_client->send_async(std::move(result));
	AVS_EX_END

	return 1;
}

int AvisynthHost::observe(std::unique_ptr<ipc_client::CommandSetFrame> c)
{
	if (int ret = check_avs_loaded(c.get()))
		return ret;

	ipc_log("SetFrame clip %u frame %u\n", c->arg().request.clip_id, c->arg().request.frame_number);

	auto it = m_remote_clips.find(c->arg().request.clip_id);
	if (it == m_remote_clips.end()) {
		ipc_log0("invalid remote clip id\n");
		send_err(c->transaction_id());
		return 1;
	}

	AVS_EX_BEGIN
	VirtualClip *clip = static_cast<VirtualClip *>(it->second.get().operator void *());
	::PVideoFrame frame = heap_to_local_frame(m_client, clip->GetVideoInfo(), c->arg(), m_env.get());
	m_cache->insert(c->arg().request.clip_id, c->arg().request.frame_number, frame);
	AVS_EX_END

	return 0;
}

void AvisynthHost::send_avsvalue(uint32_t response_id, const ::AVSValue &avs_value)
{
	if (response_id == ipc_client::INVALID_TRANSACTION)
		return;

	ipc::Value value{};

	if (avs_value.IsClip()) {
		::PClip clip = avs_value.AsClip();
		const ::VideoInfo &vi = clip->GetVideoInfo();
		uint32_t clip_id = m_local_clip_id++;
		ipc_log("local clip %u: %dx%d %d\n", clip_id, vi.width, vi.height, vi.pixel_type);

		value.type = ipc::Value::CLIP;
		value.c.clip_id = clip_id;
		value.c.vi = serialize_video_info(vi);
		m_local_clips[clip_id] = clip;
	} else if (avs_value.IsBool()) {
		value.type = ipc::Value::BOOL_;
		value.b = avs_value.AsBool();
	} else if (avs_value.IsInt()) {
		value.type = ipc::Value::INT;
		value.i = avs_value.AsInt();
	} else if (avs_value.IsFloat()) {
		value.type = ipc::Value::FLOAT;
		value.f = avs_value.AsFloat();
	} else if (avs_value.IsString()) {
		const char *str = avs_value.AsString();
		value.type = ipc::Value::STRING;
		value.s = local_to_heap_str(m_client, str, std::strlen(str));
	}

	auto response = std::make_unique<ipc_client::CommandSetScriptVar>("", value);
	response->set_response_id(response_id);
	m_client->send_async(std::move(response));
}

void AvisynthHost::send_err(uint32_t response_id)
{
	if (response_id == ipc_client::INVALID_TRANSACTION)
		return;

	auto response = std::make_unique<ipc_client::CommandErr>();
	response->set_response_id(response_id);
	m_client->send_async(std::move(response));
}

} // namespace avs
