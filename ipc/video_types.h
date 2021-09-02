#pragma once

#ifndef IPC_VIDEO_TYPES_H_
#define IPC_VIDEO_TYPES_H_

#include <cstddef>
#include <cstdint>

namespace ipc {

struct alignas(4) VideoInfo {
	enum {
		RGB = 0,
		YUV = 1,
		GRAY = 2,
		RGB24 = 3,
		RGB32 = 4,
		YUY2 = 5,
	};

	int32_t width;
	int32_t height;
	uint32_t fps_num;
	uint32_t fps_den;
	int32_t num_frames;

	int8_t color_family;
	int8_t subsample_w;
	int8_t subsample_h;
};

struct alignas(4) VideoFrameRequest {
	uint32_t clip_id;
	int32_t frame_number;
};

struct alignas(4) VideoFrame {
	VideoFrameRequest request;
	uint32_t heap_offset;
	int32_t stride[4];
	int32_t height[4];
};

struct alignas(4) Clip {
	uint32_t clip_id;
	VideoInfo vi;
};

struct alignas(8) Value {
	enum {
		CLIP = 'c',
		BOOL_ = 'b',
		INT = 'i',
		FLOAT = 'f',
		STRING = 's',
	};

	int8_t type;

	union {
		Clip c;
		int8_t b;
		int64_t i;
		double f;
		uint32_t s; // Heap pointer.
	};
};


// String functions.
size_t deserialize_str(char *dst, const void *src, size_t buf_size) noexcept;
size_t serialize_str(void *dst, const char *src, size_t len = -1) noexcept;

size_t deserialize_wstr(wchar_t *dst, const void *src, size_t buf_size) noexcept;
size_t serialize_wstr(void *dst, const wchar_t *src, size_t len = -1) noexcept;

} // namespace ipc

#endif // IPC_VIDEO_TYPES_H_
