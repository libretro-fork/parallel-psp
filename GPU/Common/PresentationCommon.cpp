// Copyright (c) 2012- PPSSPP Project.

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

#include <algorithm>
#include <cmath>
#include <set>
#include <cstdint>
#include "Common/GPU/thin3d.h"

#include "Common/System/Display.h"
#include "Common/System/System.h"
#include "Common/VR/PPSSPPVR.h"
#include "Common/Math/geom2d.h"
#include "Common/Log.h"
#include "Core/Config.h"
#include "Core/ConfigValues.h"
#include "Core/System.h"
#include "GPU/Common/PresentationCommon.h"
#include "GPU/GPUState.h"

struct Vertex {
	float x, y, z;
	float u, v;
	uint32_t rgba;
};

static bool g_overrideScreenBounds;
static Bounds g_screenBounds;

void SetOverrideScreenFrame(const Bounds *bounds) {
	g_overrideScreenBounds = bounds != nullptr;
	if (bounds) {
		g_screenBounds = *bounds;
	}
}

FRect GetScreenFrame(bool ignoreScreenInsets, float pixelWidth, float pixelHeight) {
	FRect rc = FRect{
		0.0f,
		0.0f,
		pixelWidth,
		pixelHeight,
	};

	const bool applyInset = !ignoreScreenInsets;

	if (g_overrideScreenBounds) {
		// Set rectangle to match central node. Here we ignore bIgnoreScreenInsets.
		rc.x = g_screenBounds.x;
		rc.y = g_screenBounds.y;
		rc.w = g_screenBounds.w;
		rc.h = g_screenBounds.h;
	} else if (applyInset) {
		// Remove the DPI scale to get back to pixels.
		float left = System_GetPropertyFloat(SYSPROP_DISPLAY_SAFE_INSET_LEFT) / g_display.dpi_scale_x;
		float right = System_GetPropertyFloat(SYSPROP_DISPLAY_SAFE_INSET_RIGHT) / g_display.dpi_scale_x;
		float top = System_GetPropertyFloat(SYSPROP_DISPLAY_SAFE_INSET_TOP) / g_display.dpi_scale_y;
		float bottom = System_GetPropertyFloat(SYSPROP_DISPLAY_SAFE_INSET_BOTTOM) / g_display.dpi_scale_y;

		// NOTE: Similarly to what we do in UI, we disregard any bottom inset when in landscape mode.
		if (g_display.GetDeviceOrientation() == DeviceOrientation::Landscape) {
			bottom = 0.0f;
		}

		// Adjust left edge to compensate for cutouts (notches) if any.
		rc.x += left;
		rc.w -= (left + right);
		rc.y += top;
		rc.h -= (top + bottom);
	}

	return rc;
}

void CalculateDisplayOutputRect(const DisplayLayoutConfig &config, FRect *rc, float origW, float origH, const FRect &frame, int rotation) {
	float outW;
	float outH;

	bool rotated = rotation == ROTATION_LOCKED_VERTICAL || rotation == ROTATION_LOCKED_VERTICAL180;

	bool stretch = config.bDisplayStretch && !config.bDisplayIntegerScale;

	float offsetX = config.fDisplayOffsetX;
	float offsetY = config.fDisplayOffsetY;

	float scale = config.fDisplayScale;
	float aspectRatioAdjust = config.fDisplayAspectRatio;

	float origRatio = !rotated ? origW / origH : origH / origW;
	float frameRatio = frame.w / frame.h;

	if (stretch) {
		// Automatically set aspect ratio to match the display, IF the rotation matches the output display ratio! Otherwise, just
		// ignore it because actually stretching will just look silly.
		if (rotated == (g_display.GetDeviceOrientation() == DeviceOrientation::Portrait)) {
			origRatio = frameRatio;
		} else {
			origRatio *= aspectRatioAdjust;
		}
	} else {
		origRatio *= aspectRatioAdjust;
	}

	float scaledWidth = frame.w * scale;
	float scaledHeight = frame.h * scale;

	if (origRatio > frameRatio) {
		// Image is wider than frame. Center vertically.
		outW = scaledWidth;
		outH = scaledWidth / origRatio;
	} else {
		// Image is taller than frame. Center horizontally.
		outW = scaledHeight * origRatio;
		outH = scaledHeight;
	}

	// Ye olde 1080p hack: If everything is setup to exactly cover the screen (defaults), and the screen display aspect ratio is 16:9,
	// cut off one line from the top and bottom.
	if (scale == 1.0f && aspectRatioAdjust == 1.0f && offsetX == 0.5f && offsetY == 0.5f && g_Config.bDisplayCropTo16x9) {
		if (fabsf(frame.w / frame.h - 16.0f / 9.0f) < 0.0001f) {
			outW *= 272.0f / 270.0f;
			outH *= 272.0f / 270.0f;
		}
	}

	if (config.bDisplayIntegerScale) {
		float wDim = 480.0f;
		if (rotated) {
			wDim = 272.0f;
		}

		int zoom = g_Config.iInternalResolution;
		if (zoom == 0) {
			// Auto (1:1) mode, not super meaningful with integer scaling, but let's do something that makes
			// some sense. use the longest dimension, just to have something. round down.
			if (!config.InternalRotationIsPortrait()) {
				zoom = (PSP_CoreParameter().pixelWidth) / 480;
			} else {
				zoom = (PSP_CoreParameter().pixelHeight) / 480;
			}
		}
		// If integer scaling, limit ourselves to even multiples of the rendered resolution,
		// to make sure all the pixels are square.
		wDim *= zoom;
		outW = std::max(1.0f, floorf(outW / wDim)) * wDim;
		outH = outW / origRatio;
	}

	if (IsVREnabled()) {
		rc->x = 0;
		rc->y = 0;
		rc->w = floorf(frame.w);
		rc->h = floorf(frame.h);
		outW = frame.w;
		outH = frame.h;
	} else {
		rc->x = floorf(frame.x + frame.w * offsetX - outW * 0.5f);
		rc->y = floorf(frame.y + frame.h * offsetY - outH * 0.5f);
		rc->w = floorf(outW);
		rc->h = floorf(outH);
	}
}

PresentationCommon::PresentationCommon(Draw::DrawContext *draw) : draw_(draw) {
	CreateDeviceObjects();
}

PresentationCommon::~PresentationCommon() {
	DestroyDeviceObjects();
}

void PresentationCommon::GetCardboardSettings(const DisplayLayoutConfig &config, CardboardSettings *cardboardSettings) const {
	if (!config.bEnableCardboardVR) {
		cardboardSettings->enabled = false;
		return;
	}

	// Calculate Cardboard Settings
	float cardboardScreenScale = config.iCardboardScreenSize / 100.0f;
	float cardboardScreenWidth = pixelWidth_ / 2.0f * cardboardScreenScale;
	float cardboardScreenHeight = pixelHeight_ * cardboardScreenScale;
	float cardboardMaxXShift = (pixelWidth_ / 2.0f - cardboardScreenWidth) / 2.0f;
	float cardboardUserXShift = config.iCardboardXShift / 100.0f * cardboardMaxXShift;
	float cardboardLeftEyeX = cardboardMaxXShift + cardboardUserXShift;
	float cardboardRightEyeX = pixelWidth_ / 2.0f + cardboardMaxXShift - cardboardUserXShift;
	float cardboardMaxYShift = pixelHeight_ / 2.0f - cardboardScreenHeight / 2.0f;
	float cardboardUserYShift = config.iCardboardYShift / 100.0f * cardboardMaxYShift;
	float cardboardScreenY = cardboardMaxYShift + cardboardUserYShift;

	cardboardSettings->enabled = true;
	cardboardSettings->leftEyeXPosition = cardboardLeftEyeX;
	cardboardSettings->rightEyeXPosition = cardboardRightEyeX;
	cardboardSettings->screenYPosition = cardboardScreenY;
	cardboardSettings->screenWidth = cardboardScreenWidth;
	cardboardSettings->screenHeight = cardboardScreenHeight;
}

void PresentationCommon::DeviceLost() {
	DestroyDeviceObjects();
	draw_ = nullptr;
}

void PresentationCommon::DeviceRestore(Draw::DrawContext *draw) {
	draw_ = draw;
	CreateDeviceObjects();
}

Draw::Pipeline *PresentationCommon::CreatePipeline(std::vector<Draw::ShaderModule *> shaders, const UniformBufferDesc *uniformDesc) const {
	using namespace Draw;

	Semantic pos = SEM_POSITION;
	Semantic tc = SEM_TEXCOORD0;
	// TODO: Maybe get rid of color0.
	InputLayoutDesc inputDesc = {
		sizeof(Vertex),
		{
			{ pos, DataFormat::R32G32B32_FLOAT, 0 },
			{ tc, DataFormat::R32G32_FLOAT, 12 },
			{ SEM_COLOR0, DataFormat::R8G8B8A8_UNORM, 20 },
		},
	};

	InputLayout *inputLayout = draw_->CreateInputLayout(inputDesc);
	DepthStencilState *depth = draw_->CreateDepthStencilState({ false, false, Comparison::LESS });
	BlendState *blendstateOff = draw_->CreateBlendState({ false, 0xF });
	RasterState *rasterNoCull = draw_->CreateRasterState({});

	PipelineDesc pipelineDesc{ Primitive::TRIANGLE_STRIP, shaders, inputLayout, depth, blendstateOff, rasterNoCull, uniformDesc };
	Pipeline *pipeline = draw_->CreateGraphicsPipeline(pipelineDesc, "presentation");

	inputLayout->Release();
	depth->Release();
	blendstateOff->Release();
	rasterNoCull->Release();

	return pipeline;
}

void PresentationCommon::CreateDeviceObjects() {
	using namespace Draw;

	// Still hitting this somehow!
	_dbg_assert_(vdata_ == nullptr);

	// TODO: Could probably just switch to DrawUP, it's supported well by all backends now.
	vdata_ = draw_->CreateBuffer(sizeof(Vertex) * 4, BufferUsageFlag::DYNAMIC | BufferUsageFlag::VERTEXDATA);

	samplerNearest_ = draw_->CreateSamplerState({ TextureFilter::NEAREST, TextureFilter::NEAREST, TextureFilter::NEAREST, 0.0f, TextureAddressMode::CLAMP_TO_EDGE, TextureAddressMode::CLAMP_TO_EDGE, TextureAddressMode::CLAMP_TO_EDGE });
	samplerLinear_ = draw_->CreateSamplerState({ TextureFilter::LINEAR, TextureFilter::LINEAR, TextureFilter::LINEAR, 0.0f, TextureAddressMode::CLAMP_TO_EDGE, TextureAddressMode::CLAMP_TO_EDGE, TextureAddressMode::CLAMP_TO_EDGE });

	texColor_ = CreatePipeline({ draw_->GetVshaderPreset(VS_TEXTURE_COLOR_2D), draw_->GetFshaderPreset(FS_TEXTURE_COLOR_2D) }, &vsTexColBufDesc);
	texColorRBSwizzle_ = CreatePipeline({ draw_->GetVshaderPreset(VS_TEXTURE_COLOR_2D), draw_->GetFshaderPreset(FS_TEXTURE_COLOR_2D_RB_SWIZZLE) }, &vsTexColBufDesc);

}

template <typename T>
static void DoRelease(T *&obj) {
	if (obj)
		obj->Release();
	obj = nullptr;
}

void PresentationCommon::DestroyDeviceObjects() {
	DoRelease(texColor_);
	DoRelease(texColorRBSwizzle_);
	DoRelease(samplerNearest_);
	DoRelease(samplerLinear_);
	DoRelease(vdata_);
	DoRelease(srcTexture_);
	DoRelease(srcFramebuffer_);

}

void PresentationCommon::SourceBlank() {
	DoRelease(srcTexture_);
	DoRelease(srcFramebuffer_);

	srcWidth_ = 0;
	srcHeight_ = 0;
}

// If texture == null, that means there's nothing to display, so we should show a black screen in CopyToOutput.
void PresentationCommon::SourceTexture(Draw::Texture *texture, int bufferWidth, int bufferHeight) {
	// AddRef before release and assign in case it's the same.
	texture->AddRef();

	DoRelease(srcTexture_);
	DoRelease(srcFramebuffer_);

	srcTexture_ = texture;
	srcWidth_ = bufferWidth;
	srcHeight_ = bufferHeight;
}

void PresentationCommon::SourceFramebuffer(Draw::Framebuffer *fb, int bufferWidth, int bufferHeight) {
	fb->AddRef();

	DoRelease(srcTexture_);
	DoRelease(srcFramebuffer_);

	srcFramebuffer_ = fb;
	srcWidth_ = bufferWidth;
	srcHeight_ = bufferHeight;
}

// Return value is if stereo binding succeeded.
void PresentationCommon::BindSource(int binding) {
	if (srcTexture_) {
		draw_->BindTexture(binding, srcTexture_);
	} else if (srcFramebuffer_) {
		draw_->BindFramebufferAsTexture(srcFramebuffer_, binding, Draw::Aspect::COLOR_BIT, 0);
	} else {
		_assert_(false);
	}
}

void PresentationCommon::PrepareOutput(const DisplayLayoutConfig &config, OutputFlags flags, int uvRotation, float u0, float v0, float u1, float v1) {
	draw_->Invalidate(InvalidationFlags::CACHED_RENDER_STATE);

	outputFlags_ = flags;

	int pixelWidth = pixelWidth_;
	int pixelHeight = pixelHeight_;

	// These are the output coordinates.
	FRect frame = GetScreenFrame(config.bIgnoreScreenInsets, (float)pixelWidth, (float)pixelHeight);
	// Note: In cardboard mode, we halve the width here to compensate
	// for splitting the window in half, while still reusing normal centering.
	if (config.bEnableCardboardVR) {
		frame.w /= 2.0;
		pixelWidth /= 2;
	}
	CalculateDisplayOutputRect(config, &rc_, 480.0f, 272.0f, frame, uvRotation);

	if (!srcTexture_ && !srcFramebuffer_) {
		// Presenting blank. We still computed the output rectangle.
		return;
	}

	if (GetGPUBackend() == GPUBackend::DIRECT3D9) {
		rc_.x -= 0.5f;
		rc_.y -= 0.5f;
	}

	Vertex verts[4] = {
		{ rc_.x, rc_.y, 0, u0, v0, 0xFFFFFFFF }, // TL
		{ rc_.x + rc_.w, rc_.y, 0, u1, v0, 0xFFFFFFFF }, // TR
		{ rc_.x, rc_.y + rc_.h, 0, u0, v1, 0xFFFFFFFF }, // BL
		{ rc_.x + rc_.w, rc_.y + rc_.h, 0, u1, v1, 0xFFFFFFFF }, // BR
	};

	// Rescale X, Y to normalized coordinate system.
	float invDestW = 2.0f / pixelWidth;
	float invDestH = 2.0f / pixelHeight;
	for (int i = 0; i < 4; i++) {
		verts[i].x = verts[i].x * invDestW - 1.0f;
		verts[i].y = verts[i].y * invDestH - 1.0f;
	}

	if (uvRotation != ROTATION_LOCKED_HORIZONTAL) {
		struct {
			float u;
			float v;
		} temp[4];
		int rotation = 0;
		// Vertical and Vertical180 needed swapping after we changed the coordinate system.
		switch (uvRotation) {
		case ROTATION_LOCKED_HORIZONTAL180: rotation = 2; break;
		case ROTATION_LOCKED_VERTICAL: rotation = 3; break;
		case ROTATION_LOCKED_VERTICAL180: rotation = 1; break;
		}

		// If we flipped, we rotate the other way.
		if ((flags & OutputFlags::BACKBUFFER_FLIPPED)) {
			if ((rotation & 1) != 0)
				rotation ^= 2;
		}

		static int rotLookup[4] = {0, 1, 3, 2};

		for (int i = 0; i < 4; i++) {
			int otherI = rotLookup[(rotLookup[i] + rotation) & 3];
			temp[i].u = verts[otherI].u;
			temp[i].v = verts[otherI].v;
		}
		for (int i = 0; i < 4; i++) {
			verts[i].u = temp[i].u;
			verts[i].v = temp[i].v;
		}
	}

	if (flags & OutputFlags::PILLARBOX) {
		for (int i = 0; i < 4; i++) {
			// Looks about right.
			verts[i].x *= 0.75f;
		}
	}

	// Finally, we compensate the y vertex positions for the backbuffer for any flipping.
	if (flags & OutputFlags::BACKBUFFER_FLIPPED) {
		for (int i = 0; i < 4; i++) {
			verts[i].y = -verts[i].y;
		}
	}

	draw_->UpdateBuffer(vdata_, (const uint8_t *)verts, 0, sizeof(verts), Draw::UPDATE_DISCARD);
}

void PresentationCommon::CopyToOutput(const DisplayLayoutConfig &config) {
	bool useNearest = outputFlags_ & OutputFlags::NEAREST;
	draw_->SetScissorRect(0, 0, pixelWidth_, pixelHeight_);

	if (!srcFramebuffer_ && !srcTexture_) {
		// Bound blank. We're done (although we could draw a black rectangle here).
		presentedThisFrame_ = true;
		return;
	}

	Draw::Pipeline *pipeline = (outputFlags_ & OutputFlags::RB_SWIZZLE) ? texColorRBSwizzle_ : texColor_;

	draw_->BindPipeline(pipeline);
	BindSource(0);
	Draw::VsTexColUB ub{};
	memcpy(ub.WorldViewProj, g_display.rot_matrix.m, sizeof(float) * 16);
	draw_->UpdateDynamicUniformBuffer(&ub, sizeof(ub));

	draw_->BindVertexBuffer(vdata_, 0);

	Draw::SamplerState *sampler = useNearest ? samplerNearest_ : samplerLinear_;
	draw_->BindSamplerStates(0, 1, &sampler);

	auto setViewport = [&](float x, float y, float w, float h) {
		Draw::Viewport viewport{ x, y, w, h, 0.0f, 1.0f };
		draw_->SetViewport(viewport);
	};

	CardboardSettings cardboardSettings;
	GetCardboardSettings(config, &cardboardSettings);
	if (cardboardSettings.enabled) {
		// This is what the left eye sees.
		setViewport(cardboardSettings.leftEyeXPosition, cardboardSettings.screenYPosition, cardboardSettings.screenWidth, cardboardSettings.screenHeight);
		draw_->Draw(4, 0);

		// And this is the right eye, unless they're a pirate.
		setViewport(cardboardSettings.rightEyeXPosition, cardboardSettings.screenYPosition, cardboardSettings.screenWidth, cardboardSettings.screenHeight);
		draw_->Draw(4, 0);
	} else {
		setViewport(0.0f, 0.0f, (float)pixelWidth_, (float)pixelHeight_);
		draw_->Draw(4, 0);
	}

	DoRelease(srcFramebuffer_);
	DoRelease(srcTexture_);

	// Unbinds all textures and samplers too, needed since sometimes a MakePixelTexture is deleted etc.
	draw_->Invalidate(InvalidationFlags::CACHED_RENDER_STATE);

	presentedThisFrame_ = true;
}

void PresentationCommon::CalculateRenderResolution(const DisplayLayoutConfig &config, int *width, int *height, int *scaleFactor) const {
	// In auto mode (zoom == 0), round up to an integer zoom factor for the render size.
	int zoom = g_Config.iInternalResolution;
	if (zoom == 0) {
		// auto mode, use the longest dimension
		if (!config.InternalRotationIsPortrait()) {
			zoom = (PSP_CoreParameter().pixelWidth + 479) / 480;
		} else {
			zoom = (PSP_CoreParameter().pixelHeight + 479) / 480;
		}
	}
	if (zoom <= 1)
		zoom = 1;

	if (IsVREnabled()) {
		*width = 480 * zoom;
		*height = 480 * zoom;
	} else {
		// Note: We previously checked g_Config.IsPortrait (internal rotation) here but that was wrong -
		// we still render at 480x272 * zoom.
		*width = 480 * zoom;
		*height = 272 * zoom;
	}

	*scaleFactor = zoom;
}
