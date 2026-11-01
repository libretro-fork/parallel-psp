
#include "libretro/LibretroGraphicsContext.h"
#include "libretro/LibretroGLContext.h"
#include "libretro/LibretroGLCoreContext.h"
#include "libretro/LibretroVulkanContext.h"
#ifdef _WIN32
#include "libretro/LibretroD3D11Context.h"
#include "libretro/LibretroD3D9Context.h"
#endif

#include "Common/Log.h"
#include "Core/Config.h"
#include "Core/System.h"
#include "GPU/GPUCommon.h"
#include "GPU/Software/SoftGpu.h"

retro_video_refresh_t LibretroGraphicsContext::video_cb;

extern "C" {
   retro_hw_get_proc_address_t libretro_get_proc_address;
};

void retro_set_video_refresh(retro_video_refresh_t cb) { LibretroGraphicsContext::video_cb = cb; }
static void context_reset() { ((LibretroHWRenderContext *)Libretro::ctx)->ContextReset(); }
static void context_destroy() { ((LibretroHWRenderContext *)Libretro::ctx)->ContextDestroy(); }

bool LibretroHWRenderContext::InitHW(bool cache_context) {
	hw_render_.cache_context = cache_context;
	if (!Libretro::environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render_))
		return false;
	libretro_get_proc_address = hw_render_.get_proc_address;
	return true;
}

LibretroHWRenderContext::LibretroHWRenderContext(retro_hw_context_type context_type, unsigned version_major, unsigned version_minor) {
	hw_render_.context_type = context_type;
	hw_render_.version_major = version_major;
	hw_render_.version_minor = version_minor;
	hw_render_.context_reset = context_reset;
	hw_render_.context_destroy = context_destroy;
	hw_render_.depth = true;
}

void LibretroHWRenderContext::ContextReset() {
	INFO_LOG(Log::G3D, "Context reset");

	if (gpu && Libretro::useEmuThread) {
		Libretro::EmuThreadPause();
	}

	if (gpu) {
		gpu->DeviceLost();
	}

	if (!draw_) {
		CreateDrawContext();
		bool success = draw_->CreatePresets();
		_assert_(success);
	}

	GotBackbuffer();

	if (gpu) {
		gpu->DeviceRestore(draw_);
	}

	if (gpu && Libretro::useEmuThread) {
		Libretro::EmuThreadStart();
	}
}

void LibretroHWRenderContext::ContextDestroy() {
	INFO_LOG(Log::G3D, "Context destroy");

	if (Libretro::useEmuThread) {
		Libretro::EmuThreadStop();
	}

	if (gpu) {
		gpu->DeviceLost();
	}

	if (!hw_render_.cache_context && Libretro::useEmuThread && draw_ && Libretro::EmuThreadStateGet() != Libretro::EmuThreadState::PAUSED) {
		DestroyDrawContext();
	}

	if (!hw_render_.cache_context && !Libretro::useEmuThread) {
		ShutdownAPI();
	}
}

void LibretroGraphicsContext::GotBackbuffer() { draw_->HandleEvent(Draw::Event::GOT_BACKBUFFER, PSP_CoreParameter().pixelWidth, PSP_CoreParameter().pixelHeight); }

void LibretroGraphicsContext::LostBackbuffer() { draw_->HandleEvent(Draw::Event::LOST_BACKBUFFER, -1, -1); }

LibretroGraphicsContext *LibretroGraphicsContext::CreateGraphicsContext() {
	LibretroGraphicsContext *ctx;

	retro_hw_context_type preferred;
	if (!Libretro::environ_cb(RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER, &preferred))
		preferred = RETRO_HW_CONTEXT_DUMMY;

	if (Libretro::backend != RETRO_HW_CONTEXT_DUMMY)
		preferred = Libretro::backend;

   std::string errorMessage;
#ifndef USING_GLES2
	if (preferred == RETRO_HW_CONTEXT_DUMMY || preferred == RETRO_HW_CONTEXT_OPENGL_CORE) {
		ctx = new LibretroGLCoreContext();

		if (ctx->InitAPI(nullptr, nullptr, &errorMessage)) {
			return ctx;
		}
      // Failed? Try next.
		delete ctx;
	}
#endif

	if (preferred == RETRO_HW_CONTEXT_DUMMY || preferred == RETRO_HW_CONTEXT_OPENGL || preferred == RETRO_HW_CONTEXT_OPENGLES3) {
		ctx = new LibretroGLContext();

		if (ctx->InitAPI(nullptr, nullptr, &errorMessage)) {
			return ctx;
		}

      // Failed? Try next.
      delete ctx;
	}

#ifndef HAVE_LIBNX
	if (preferred == RETRO_HW_CONTEXT_DUMMY || preferred == RETRO_HW_CONTEXT_VULKAN) {
		ctx = new LibretroVulkanContext();

		if (ctx->InitAPI(nullptr, nullptr, &errorMessage)) {
			return ctx;
		}

      // Failed? Try next.
      delete ctx;
	}
#endif

#ifdef _WIN32
	if (preferred == RETRO_HW_CONTEXT_D3D9) {
		ctx = new LibretroD3D9Context();
		if (ctx->InitAPI(nullptr, nullptr, &errorMessage))
			return ctx;
		delete ctx;
	}
	if (preferred == RETRO_HW_CONTEXT_DUMMY || preferred == RETRO_HW_CONTEXT_D3D11) {
		ctx = new LibretroD3D11Context();

		if (ctx->InitAPI(nullptr, nullptr, &errorMessage)) {
			return ctx;
		}

      // Failed? Try next.
		delete ctx;
	}
#endif

   // End of the line.
	ctx = new LibretroSoftwareContext();
	ctx->InitAPI(nullptr, nullptr, &errorMessage);
	return ctx;
}

// One pass from PSP display rows to XRGB8888 rows.
static void ConvertDisplayRows(const u8 *src, int srcStride, GEBufferFormat fmt, u32 *dst, size_t dstPitch, u32 w, u32 h) {
	const size_t srcPitch = (size_t)srcStride * (fmt == GE_FORMAT_8888 ? 4 : 2);
	for (u32 y = 0; y < h; y++, src += srcPitch, dst += dstPitch) {
		switch (fmt) {
		case GE_FORMAT_8888: ConvertBGRA8888ToRGBA8888(dst, (const u32 *)src, w); break;
		case GE_FORMAT_565: ConvertRGB565ToBGRA8888(dst, (const u16 *)src, w); break;
		case GE_FORMAT_5551: ConvertRGBA5551ToBGRA8888(dst, (const u16 *)src, w); break;
		case GE_FORMAT_4444: ConvertRGBA4444ToBGRA8888(dst, (const u16 *)src, w); break;
		default: memset(dst, 0, w * sizeof(u32)); break;
		}
	}
}

void LibretroSoftwareContext::SwapBuffers() {
	u32 w = NATIVEWIDTH;
	u32 h = NATIVEHEIGHT;
	if (!gpu) {
		// Nothing rendered yet, dupe the previous frame.
		video_cb(NULL, w, h, w * sizeof(u32));
		return;
	}
	// 480x272 -> 480x270: skip the first row, drop the last.
	const u32 skip = g_Config.bDisplayCropTo16x9 ? 1 : 0;
	const u32 outH = h - 2 * skip;

	const u8 *src;
	int srcStride;
	GEBufferFormat fmt;
	if (static_cast<SoftGPU *>(gpu)->LendDisplayPixels(&src, &srcStride, &fmt)) {
		src += (size_t)skip * srcStride * (fmt == GE_FORMAT_8888 ? 4 : 2);
		// Convert straight into the frontend's buffer when it lends one.
		retro_framebuffer fb{};
		fb.width = w;
		fb.height = outH;
		fb.access_flags = RETRO_MEMORY_ACCESS_WRITE;
		if (Libretro::environ_cb(RETRO_ENVIRONMENT_GET_CURRENT_SOFTWARE_FRAMEBUFFER, &fb) && fb.data
			&& fb.format == RETRO_PIXEL_FORMAT_XRGB8888 && fb.width == w && fb.height == outH
			&& fb.pitch >= w * sizeof(u32) && fb.pitch % sizeof(u32) == 0) {
			ConvertDisplayRows(src, srcStride, fmt, (u32 *)fb.data, fb.pitch / sizeof(u32), w, outH);
			video_cb(fb.data, w, outH, fb.pitch);
			return;
		}
		pixels_.resize((size_t)w * outH);
		ConvertDisplayRows(src, srcStride, fmt, pixels_.data(), w, w, outH);
		video_cb(pixels_.data(), w, outH, w * sizeof(u32));
		return;
	}

	GPUDebugBuffer buf;
	gpu->GetOutputFramebuffer(buf);
	if (!ConvertFramebufferForLibretro(&buf, w, h, pixels_)) {
		// Unsupported debug buffer format, conversion returned nothing.
		video_cb(NULL, w, h, w * sizeof(u32));
		return;
	}
	video_cb(pixels_.data() + (size_t)skip * w, w, outH, w * sizeof(u32));
}

bool ConvertFramebufferForLibretro(const GPUDebugBuffer *buffer, u32 stride, u32 h, std::vector<u32> &data) {
	// If the output was small, act like everything outside was 0.
	// This can happen depending on viewport parameters.
	u32 safeW = std::min(stride, buffer->GetStride());
	u32 safeH = std::min(h, buffer->GetHeight());

	if (stride == 0 || h == 0)
		return false;
	data.resize((size_t)stride * h);

	const u32 *pixels32 = (const u32 *)buffer->GetData();
	const u16 *pixels16 = (const u16 *)buffer->GetData();
	int outStride = buffer->GetStride();
#if 0
	if (!buffer->GetFlipped()) {
		// Bitmaps are flipped, so we have to compare backwards in this case.
		int toLastRow = outStride * (h > buffer->GetHeight() ? buffer->GetHeight() - 1 : h - 1);
		pixels32 += toLastRow;
		pixels16 += toLastRow;
		outStride = -outStride;
	}
#endif
	// Skip the bottom of the image in the buffer was smaller.  Remember, we're flipped.
	u32 *dst = &data[0];
	if (safeH < h) {
		std::fill(dst, dst + (size_t)(h - safeH) * stride, 0);
		dst += (h - safeH) * stride;
	}

	for (u32 y = 0; y < safeH; ++y) {
		// Every active pixel is overwritten below. Clear only borders left
		// over from the previous frame, not the complete output image.
		if (safeW < stride)
			std::fill(dst + (size_t)y * stride + safeW, dst + (size_t)(y + 1) * stride, 0);
		switch (buffer->GetFormat()) {
		case GPU_DBG_FORMAT_8888:
			ConvertBGRA8888ToRGBA8888(&dst[y * stride], pixels32, safeW);
			break;
		case GPU_DBG_FORMAT_8888_BGRA:
			memcpy(&dst[y * stride], pixels32, safeW * sizeof(u32));
			break;

		case GPU_DBG_FORMAT_565:
			ConvertRGB565ToBGRA8888(&dst[y * stride], pixels16, safeW);
			break;
		case GPU_DBG_FORMAT_5551:
			ConvertRGBA5551ToBGRA8888(&dst[y * stride], pixels16, safeW);
			break;
		case GPU_DBG_FORMAT_4444:
			ConvertRGBA4444ToBGRA8888(&dst[y * stride], pixels16, safeW);
			break;

		default:
			return false;
		}

		pixels32 += outStride;
		pixels16 += outStride;
	}

	return true;
}
