// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 2 of the License, or
// (at your option) any later version.
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#include <libretro.h>
#include "Core/HW/AvcDecoder.h"
#include "Core/HW/MediaEngine.h"
#include "GPU/ge_constants.h"
#include <cstring>
#include <vector>
#include "unittest/UnitTest.h"

namespace {
struct DecoderMock {
	int opens = 0, closes = 0, eager = 0, deferred = 0, readbacks = 0;
	int result = 1;
	bool failReadback = false, badStride = false, badDimensions = false;
	bool drainReady = true;
	unsigned flags = 0;
	bool busyOnce = false, stableInput = true;
	const uint8_t *retryInput = nullptr;
	size_t retrySize = 0;
	u8 y[8]{19, 19, 19, 19, 19, 19, 19, 19}, u[2]{71, 71}, v[2]{137, 137};
} mock;

void FillFrame(retro_video_decode_frame *frame, bool planes) {
	*frame = {};
	frame->width = mock.badDimensions ? 8 : 4;
	frame->height = 2;
	frame->hardware = true;
	if (planes) {
		frame->planes[0] = mock.y; frame->planes[1] = mock.u; frame->planes[2] = mock.v;
		frame->strides[0] = mock.badStride ? 3 : 4;
		frame->strides[1] = frame->strides[2] = 2;
	}
}
void *Open() { ++mock.opens; return &mock; }
void Close(void *) { ++mock.closes; }
int Decode(void *, const uint8_t *, size_t, retro_video_decode_frame *frame) {
	++mock.eager; FillFrame(frame, true); return mock.result;
}
int Drain(void *, retro_video_decode_frame *frame) {
	FillFrame(frame, true); return mock.result;
}
int DecodeEx(void *, const uint8_t *data, size_t size, retro_video_decode_frame *frame, unsigned flags) {
	if (mock.busyOnce) {
		mock.busyOnce = false; mock.retryInput = data; mock.retrySize = size; return 2;
	}
	if (mock.retryInput) {
		mock.stableInput = data == mock.retryInput && size == mock.retrySize;
		mock.retryInput = nullptr;
	}
	++mock.deferred; mock.flags = flags; FillFrame(frame, false); return mock.result;
}
int DrainEx(void *, retro_video_decode_frame *frame, unsigned flags) {
	mock.flags = flags; FillFrame(frame, false);
	if (!mock.drainReady) return 0;
	mock.drainReady = false; return 1;
}
int GetFrame(void *, retro_video_decode_frame *frame) {
	++mock.readbacks;
	if (mock.failReadback) return -1;
	FillFrame(frame, true); return 1;
}
struct ResetFrontend {
	~ResetFrontend() { AvcDecoder::SetFrontendDecoder(nullptr); }
};
}

bool TestDeferredVideoDecode() {
	ResetFrontend reset;
	retro_video_decode_interface iface{};
	iface.interface_version = 2;
	iface.open = Open; iface.close = Close; iface.decode = Decode; iface.drain = Drain;
	iface.decode_ex = DecodeEx; iface.drain_ex = DrainEx; iface.get_frame = GetFrame;
	mock = {};
	AvcDecoder::SetFrontendDecoder(&iface);
	u8 input = 0;
	{
		AvcDecoder decoder;
		EXPECT_TRUE(decoder.Decode(&input, 1, true));
		EXPECT_EQ_INT(decoder.Width(), 4); EXPECT_EQ_INT(decoder.Height(), 2);
		EXPECT_TRUE(!decoder.Plane(0)); EXPECT_EQ_INT(decoder.Stride(0), 0);
		EXPECT_EQ_INT(mock.flags, RETRO_VIDEO_DECODE_DEFER_PLANES);
		// Discard one output by decoding the next, without touching CPU planes.
		EXPECT_TRUE(decoder.Decode(&input, 1, true));
		EXPECT_EQ_INT(mock.readbacks, 0); EXPECT_EQ_INT(mock.deferred, 2);
		EXPECT_TRUE(decoder.Materialize()); EXPECT_TRUE(decoder.Materialize());
		EXPECT_EQ_INT(mock.readbacks, 1); EXPECT_EQ_INT(decoder.Plane(0)[0], 19);
		EXPECT_EQ_INT(decoder.Stride(0), 4);
		EXPECT_TRUE(decoder.Drain(true)); EXPECT_TRUE(!decoder.Plane(0));
		EXPECT_TRUE(decoder.Materialize()); EXPECT_EQ_INT(mock.readbacks, 2);
		EXPECT_TRUE(!decoder.Drain(true)); EXPECT_TRUE(!decoder.Materialize());
		EXPECT_EQ_INT(mock.readbacks, 2);
		EXPECT_TRUE(decoder.Decode(&input, 1, true));
		mock.failReadback = true;
		EXPECT_TRUE(!decoder.Materialize()); EXPECT_TRUE(!decoder.HasFrame());
		EXPECT_EQ_INT(decoder.LastResult(), -2);
		EXPECT_TRUE(!decoder.Materialize()); EXPECT_EQ_INT(mock.readbacks, 3);
		mock.failReadback = false;
		EXPECT_TRUE(decoder.Decode(&input, 1, true));
		mock.badStride = true; EXPECT_TRUE(!decoder.Materialize()); mock.badStride = false;
		EXPECT_TRUE(decoder.Decode(&input, 1, true));
		mock.badDimensions = true; EXPECT_TRUE(!decoder.Materialize()); mock.badDimensions = false;
		decoder.Flush(); EXPECT_TRUE(!decoder.Materialize());
		EXPECT_TRUE(decoder.Decode(&input, 1)); EXPECT_EQ_INT(mock.eager, 1);
		int before = mock.readbacks; EXPECT_TRUE(decoder.Materialize()); EXPECT_EQ_INT(mock.readbacks, before);
	}
	EXPECT_EQ_INT(mock.opens, mock.closes);
	// A PSMF busy result must preserve the exact access unit and timestamp.
	mock = {}; mock.busyOnce = true; mock.drainReady = false;
	{
		std::vector<u8> header(2048), packet(2048, 0xff);
		memcpy(header.data(), "PSMF0015", 8);
		header[10] = 8; header[14] = 8;  // offset and data size: 2048, big endian.
		header[0x81] = 1; header[0x82] = 0xe0;
		const u8 pes[] = {0,0,1,0xe0,0,13,0x80,0,0,0,0,1,0x65,0x80,0,0,1,0x41,0x80};
		memcpy(packet.data(), pes, sizeof(pes));
		const u8 padding[] = {0,0,1,0xbe,7,0xe7};
		memcpy(packet.data() + sizeof(pes), padding, sizeof(padding));
		MediaEngine engine;
		EXPECT_TRUE(engine.loadStream(header.data(), (int)header.size(), 8192));
		EXPECT_EQ_INT(engine.addStreamData(packet.data(), (int)packet.size()), 2048);
		EXPECT_TRUE(!engine.stepVideo(GE_CMODE_32BIT_ABGR8888, true));
		EXPECT_TRUE(engine.IsVideoDecodeBusy()); EXPECT_TRUE(!engine.IsVideoEnd());
		EXPECT_EQ_INT(engine.getVideoTimeStamp(), 0); EXPECT_EQ_INT(mock.readbacks, 0);
		EXPECT_TRUE(engine.stepVideo(GE_CMODE_32BIT_ABGR8888, true));
		EXPECT_TRUE(mock.stableInput); EXPECT_TRUE(!engine.IsVideoDecodeBusy());
		EXPECT_EQ_INT(mock.readbacks, 0); EXPECT_EQ_INT(mock.deferred, 1);
		EXPECT_TRUE(engine.stepVideo(GE_CMODE_32BIT_ABGR8888));
		EXPECT_EQ_INT(mock.readbacks, 1); EXPECT_EQ_INT(mock.deferred, 2);
		EXPECT_TRUE(!engine.stepVideo(GE_CMODE_32BIT_ABGR8888));
		EXPECT_TRUE(engine.IsVideoEnd());
	}
	EXPECT_EQ_INT(mock.opens, mock.closes);
	// A v1 frontend still supplies eager planes when the core asks to defer.
	iface.interface_version = 1;
	iface.decode_ex = nullptr; iface.drain_ex = nullptr; iface.get_frame = nullptr;
	mock = {};
	{
		AvcDecoder decoder;
		EXPECT_TRUE(decoder.Decode(&input, 1, true)); EXPECT_TRUE(decoder.Materialize());
		EXPECT_EQ_INT(mock.eager, 1); EXPECT_EQ_INT(mock.deferred, 0); EXPECT_EQ_INT(mock.readbacks, 0);
	}
	EXPECT_EQ_INT(mock.opens, mock.closes);
	return true;
}
