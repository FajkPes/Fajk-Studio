#ifndef FAJK_DECODE_H
#define FAJK_DECODE_H
#include <stdint.h>
#include <stddef.h>
#if defined(_WIN32)
#if defined(FAJK_DECODE_EXPORTS)
#define FAJK_API __declspec(dllexport)
#else
#define FAJK_API __declspec(dllimport)
#endif
#else
#define FAJK_API
#endif
#ifdef __cplusplus
extern "C" {
#endif
typedef struct fajk_decoder fajk_decoder;
typedef struct fajk_sequence_info {
    uint32_t struct_size; /* Set to sizeof(fajk_sequence_info) before get_info. */
    uint32_t width,height,fps_num,fps_den,source_step;
    uint64_t frame_count;
    int64_t duration_100ns;
} fajk_sequence_info;
/* ABI 1, Windows x64. UTF-8 path. Returns NULL on failure.
   error is optional and receives a NUL-terminated UTF-8 message. */
FAJK_API uint32_t fajk_decode_abi_version(void);
FAJK_API fajk_decoder* fajk_open(const char* utf8_path,char* error,size_t error_capacity);
FAJK_API void fajk_close(fajk_decoder* decoder);
FAJK_API int fajk_get_info(fajk_decoder* decoder,fajk_sequence_info* info);
/* Calls on one handle must be serialized; decoding itself uses multiple cores.
   For concurrent handles, assign each a share with fajk_set_threads.
   RGBA8 / sRGB / top-down / straight alpha. Pixels owned by caller.
   Reads the requested packet and its independent key reference when needed.
   Contiguous rows decode directly into the supplied buffer.
   Padded rows reuse a private contiguous scratch surface up to 64 MiB per
   handle, released by fajk_close; larger surfaces use temporary storage.
   Returns 0 on success, -1 on failure; inspect fajk_last_error.
   Buffer contents are unspecified on failure; do not present them. */
FAJK_API int fajk_decode_frame(fajk_decoder* decoder,uint64_t index,uint8_t* rgba,size_t buffer_size,size_t stride,int64_t* pts_100ns,int64_t* duration_100ns);
/* 0 = automatic (logical CPU count minus two); capped to available processors. */
FAJK_API int fajk_set_threads(fajk_decoder* decoder,uint32_t threads);
/* 0 = R500 CPU (default), 1 = R500 GPU. Execution choice only; file format
   stays compatible. Unsupported operations/devices fall back to CPU. */
FAJK_API int fajk_set_backend(fajk_decoder* decoder,uint32_t backend);
/* Base wavelet reconstruction strength, 100 is the default. Does not change
   packet bytes. This is independent of R441 cleanup stored in the packet;
   setting it to zero does not disable that saved cleanup. Lossless/palette
   modes are unaffected. */
FAJK_API int fajk_set_deblocking(fajk_decoder* decoder,uint32_t percent);
// Optional final display sharpening after reconstruction, default 0 (same as Studio).
// Saved R441 contour contrast remains a separate reconstruction control.
FAJK_API int fajk_set_sharpening(fajk_decoder* decoder,uint32_t percent);
FAJK_API int fajk_frame_at_time(fajk_decoder* decoder,int64_t pts_100ns,uint64_t* index);
FAJK_API const char* fajk_last_error(fajk_decoder* decoder);
#ifdef __cplusplus
}
#endif
#endif
