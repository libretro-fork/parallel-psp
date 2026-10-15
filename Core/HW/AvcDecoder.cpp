// Copyright (c) 2026- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include "ppsspp_config.h"

#include "Common/Log.h"
#include "Core/HW/AvcDecoder.h"

#include <climits>
#include <formats/rh264.h>
#include <libretro.h>

static const retro_video_decode_interface *frontendDecoder;

void AvcDecoder::SetFrontendDecoder(const retro_video_decode_interface *iface) {
	frontendDecoder = iface;
}

AvcDecoder::AvcDecoder() {
	Open();
}

void AvcDecoder::Open() {
	frontend_ = frontendDecoder;
	if (frontend_)
		frontendDecoder_ = frontend_->open();
	if (frontendDecoder_)
		return;
	frontend_ = nullptr;
	dec_ = rh264_video_open();
	if (!dec_)
		ERROR_LOG(Log::ME, "AvcDecoder: couldn't allocate the H.264 decoder");
}

AvcDecoder::~AvcDecoder() {
	Close();
}

void AvcDecoder::Close() {
	pendingPlanes_ = false;
	if (frontendDecoder_)
		frontend_->close(frontendDecoder_);
	frontendDecoder_ = nullptr;
	if (dec_)
		rh264_video_close(dec_);
	dec_ = nullptr;
}

bool AvcDecoder::IsAvailable() {
	return true;
}

void AvcDecoder::Flush() {
	haveFrame_ = false;
	lastResult_ = 0;
	Close();
	reportedDecoder_ = false;
	Open();
}

bool AvcDecoder::TakeFrontendFrame(const retro_video_decode_frame &frame, bool deferred, bool countFrame) {
	if (!frame.width || !frame.height || frame.width > INT_MAX || frame.height > INT_MAX) {
		haveFrame_ = false;
		pendingPlanes_ = false;
		return false;
	}
	for (int i = 0; i < 3; i++) {
		planes_[i] = frame.planes[i];
		strides_[i] = frame.strides[i];
	}
	width_ = frame.width;
	height_ = frame.height;
	if (!reportedDecoder_) {
		INFO_LOG(Log::ME, "AvcDecoder: frontend %s H.264 decoding active (%dx%d)", frame.hardware ? "hardware" : "software", width_, height_);
		reportedDecoder_ = true;
	}
	if (countFrame) frameCount_++;
	const unsigned chromaWidth = (frame.width + 1) / 2;
	haveFrame_ = deferred || (planes_[0] && planes_[1] && planes_[2]
		&& frame.strides[0] >= frame.width && frame.strides[0] <= INT_MAX
		&& frame.strides[1] >= chromaWidth && frame.strides[1] <= INT_MAX
		&& frame.strides[2] >= chromaWidth && frame.strides[2] <= INT_MAX);
	pendingPlanes_ = haveFrame_ && deferred;
	return haveFrame_;
}

bool AvcDecoder::TakeFrame() {
	int w = 0, h = 0, cw = 0, ch = 0;
	// Everything downstream indexes the three planes as 8-bit 4:2:0, which is all the PSP's
	// encoder produced. Anything else would be read as if it were, so say so and drop the frame
	// rather than converting garbage.
	if (rh264_video_bit_depth(dec_) != 8) {
		WARN_LOG(Log::ME, "AvcDecoder: %d-bit frame, ignoring it", rh264_video_bit_depth(dec_));
		return false;
	}
	planes_[0] = rh264_video_plane(dec_, 0, &strides_[0], &w, &h);
	planes_[1] = rh264_video_plane(dec_, 1, &strides_[1], &cw, &ch);
	planes_[2] = rh264_video_plane(dec_, 2, &strides_[2], &cw, &ch);
	if (!planes_[0] || !planes_[1] || !planes_[2] || cw != (w + 1) / 2 || ch != (h + 1) / 2) {
		WARN_LOG(Log::ME, "AvcDecoder: frame not 4:2:0, ignoring it");
		return false;
	}
	width_ = w;
	height_ = h;
	frameCount_++;
	haveFrame_ = width_ > 0 && height_ > 0;
	return haveFrame_;
}

bool AvcDecoder::Decode(const u8 *data, int size, bool deferPlanes) {
	pendingPlanes_ = false;
	haveFrame_ = false;
	lastResult_ = -1;
	if ((!dec_ && !frontendDecoder_) || !data || size <= 0)
		return false;

	retro_video_decode_frame frame{};
	const bool deferred = deferPlanes && frontendDecoder_ && frontend_->interface_version >= 2
		&& frontend_->decode_ex && frontend_->drain_ex && frontend_->get_frame;
	const int r = deferred ? frontend_->decode_ex(frontendDecoder_, data, (size_t)size, &frame, RETRO_VIDEO_DECODE_DEFER_PLANES)
		: frontendDecoder_ ? frontend_->decode(frontendDecoder_, data, (size_t)size, &frame)
		: rh264_video_decode(dec_, data, (size_t)size);
	if (r < 0) {
		// A unit the decoder refuses leaves the pictures predicted from it wrong until the next
		// key frame. That is a damaged or unsupported stream, not a decoder fault.
		failedUnits_++;
		if (!wasFailing_) {
			WARN_LOG(Log::ME, "AvcDecoder: access unit %d (%d bytes) refused", frameCount_ + failedUnits_, size);
			wasFailing_ = true;
		}
		return false;
	}
	if (r == 2) {
		lastResult_ = 2;  // The caller must retain this access unit and retry later.
		return false;
	}
	if (wasFailing_) {
		WARN_LOG(Log::ME, "AvcDecoder: decoding again, %d access units were refused", failedUnits_);
		wasFailing_ = false;
		failedUnits_ = 0;
	}
	// 0 is normal: H.264 reorders, so the decoder wants more input before it gives a frame back.
	if (r != 1) {
		lastResult_ = 0;
		return false;
	}
	lastResult_ = (frontendDecoder_ ? TakeFrontendFrame(frame, deferred) : TakeFrame()) ? 1 : -2;
	return lastResult_ == 1;
}

bool AvcDecoder::Drain(bool deferPlanes) {
	pendingPlanes_ = false;
	haveFrame_ = false;
	lastResult_ = 0;
	retro_video_decode_frame frame{};
	if (frontendDecoder_) {
		const bool deferred = deferPlanes && frontend_->interface_version >= 2
			&& frontend_->decode_ex && frontend_->drain_ex && frontend_->get_frame;
		const int result = deferred ? frontend_->drain_ex(frontendDecoder_, &frame, RETRO_VIDEO_DECODE_DEFER_PLANES)
			: frontend_->drain(frontendDecoder_, &frame);
		lastResult_ = result == 1 ? (TakeFrontendFrame(frame, deferred) ? 1 : -2) : result;
		return lastResult_ == 1;
	}
	if (!dec_ || rh264_video_drain(dec_) != 0)
		return false;
	lastResult_ = TakeFrame() ? 1 : -2;
	return lastResult_ == 1;
}

bool AvcDecoder::Materialize() {
	if (!haveFrame_) return false;
	if (!pendingPlanes_) return true;
	retro_video_decode_frame frame{};
	if (frontend_->get_frame(frontendDecoder_, &frame) != 1
		|| frame.width != (unsigned)width_ || frame.height != (unsigned)height_
		|| !TakeFrontendFrame(frame, false, false)) {
		haveFrame_ = false;
		pendingPlanes_ = false;
		lastResult_ = -2;
		return false;
	}
	return true;
}

const u8 *AvcDecoder::Plane(int index) const {
	if (!haveFrame_ || pendingPlanes_ || index < 0 || index >= 3)
		return nullptr;
	return planes_[index];
}

int AvcDecoder::Stride(int index) const {
	if (!haveFrame_ || pendingPlanes_ || index < 0 || index >= 3)
		return 0;
	return strides_[index];
}
