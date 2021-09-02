#include <cassert>
#include <cstdint>
#include <string>
#include "video_types.h"

namespace ipc {

namespace {

constexpr size_t LEN_UNKNOWN = ~static_cast<size_t>(0);

template <class T>
size_t LEN_MAX = (UINT32_MAX - sizeof(uint32_t) - sizeof(T)) / sizeof(T);

template <class T>
size_t deserialize_chars(T *dst, const void *src, size_t buf_size)
{
	if (buf_size < sizeof(uint32_t) + sizeof(T))
		return LEN_UNKNOWN;

	size_t len = *static_cast<const uint32_t *>(src);
	if (len > LEN_MAX<T>)
		return LEN_UNKNOWN;
	if (buf_size < sizeof(uint32_t) + len * sizeof(T) + sizeof(T))
		return LEN_UNKNOWN;

	if (dst) {
		const T *p = reinterpret_cast<const T *>(static_cast<const unsigned char *>(src) + sizeof(uint32_t));
		std::memcpy(dst, p, len * sizeof(T));
		dst[len] = '\0';
	}

	return len;
}

template <class T>
size_t serialize_chars(void *dst, const T *src, size_t len)
{
	if (len == LEN_UNKNOWN)
		len = std::char_traits<T>::length(src);
	if (len > LEN_MAX<T>)
		len = 0;

	size_t total_size = sizeof(uint32_t) + len * sizeof(T) + sizeof(T);

	if (dst) {
		*static_cast<uint32_t *>(dst) = static_cast<uint32_t>(len);

		T *p = reinterpret_cast<T *>(static_cast<unsigned char *>(dst) + sizeof(uint32_t));
		std::memcpy(p, src, len * sizeof(T));
		p[len] = '\0';
	}

	return total_size;
}

} // namespace


size_t deserialize_str(char *dst, const void *src, size_t buf_size) noexcept
{
	return deserialize_chars(dst, src, buf_size);
}

size_t serialize_str(void *dst, const char *src, size_t len) noexcept
{
	return serialize_chars(dst, src, len);
}

size_t deserialize_wstr(wchar_t *dst, const void *src, size_t buf_size) noexcept
{
	return deserialize_chars(dst, src, buf_size);
}

size_t serialize_wstr(void *dst, const wchar_t *src, size_t len) noexcept
{
	return serialize_chars(dst, src, len);
}

} // namespace ipc
