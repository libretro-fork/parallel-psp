#include "Common/GPU/OpenGL/GLCommon.h"
#include "Common/GPU/OpenGL/GLFrameData.h"
#include "Common/GPU/OpenGL/GLRenderManager.h"
#include "Common/Log.h"
#include "Common/MemoryUtil.h"

namespace {
// A frame's uploads usually fit one chunk of this; more are kept only up to the cap.
const size_t UPLOAD_CHUNK = 4 * 1024 * 1024;
const size_t UPLOAD_RETAIN = 16 * 1024 * 1024;
}

GLUploadArena::~GLUploadArena() {
	for (const Chunk &chunk : chunks_)
		FreeAlignedMemory(chunk.data);
}

uint8_t *GLUploadArena::Allocate(size_t size) {
	size = (size + 15) & ~(size_t)15;
	while (chunk_ < chunks_.size() && offset_ + size > chunks_[chunk_].size) {
		chunk_++;
		offset_ = 0;
	}
	if (chunk_ == chunks_.size()) {
		const size_t chunkSize = size > UPLOAD_CHUNK ? size : UPLOAD_CHUNK;
		uint8_t *data = (uint8_t *)AllocateAlignedMemory(chunkSize, 16);
		if (!data)
			return nullptr;
		chunks_.push_back({ data, chunkSize });
		offset_ = 0;
	}
	uint8_t *p = chunks_[chunk_].data + offset_;
	offset_ += size;
	used_ += size;
	return p;
}

void GLUploadArena::Reset() {
	// A frame that spilled into several chunks gets one that fits it next time, up to the cap;
	// past the cap, the extra goes back.
	if (chunks_.size() > 1 || (!chunks_.empty() && chunks_[0].size > UPLOAD_RETAIN)) {
		const size_t want = used_ > UPLOAD_CHUNK ? (used_ < UPLOAD_RETAIN ? used_ : UPLOAD_RETAIN) : UPLOAD_CHUNK;
		for (const Chunk &chunk : chunks_)
			FreeAlignedMemory(chunk.data);
		chunks_.clear();
		uint8_t *data = (uint8_t *)AllocateAlignedMemory(want, 16);
		if (data)
			chunks_.push_back({ data, want });
	}
	chunk_ = 0;
	offset_ = 0;
	used_ = 0;
}

size_t GLUploadArena::Retained() const {
	size_t total = 0;
	for (const Chunk &chunk : chunks_)
		total += chunk.size;
	return total;
}

void GLDeleter::Take(GLDeleter &other) {
	_assert_msg_(IsEmpty(), "Deleter already has stuff");
	shaders = std::move(other.shaders);
	programs = std::move(other.programs);
	buffers = std::move(other.buffers);
	textures = std::move(other.textures);
	inputLayouts = std::move(other.inputLayouts);
	framebuffers = std::move(other.framebuffers);
	pushBuffers = std::move(other.pushBuffers);
	other.shaders.clear();
	other.programs.clear();
	other.buffers.clear();
	other.textures.clear();
	other.inputLayouts.clear();
	other.framebuffers.clear();
	other.pushBuffers.clear();
}

// Runs on the GPU thread.
void GLDeleter::Perform(GLRenderManager *renderManager, bool skipGLCalls) {
	for (auto pushBuffer : pushBuffers) {
		renderManager->UnregisterPushBuffer(pushBuffer);
		if (skipGLCalls) {
			pushBuffer->Destroy(false);
		}
		delete pushBuffer;
	}
	pushBuffers.clear();
	for (auto shader : shaders) {
		if (skipGLCalls && shader)
			shader->shader = 0;  // prevent the glDeleteShader
		delete shader;
	}
	shaders.clear();
	for (auto program : programs) {
		if (skipGLCalls && program)
			program->program = 0;  // prevent the glDeleteProgram
		delete program;
	}
	programs.clear();
	for (auto buffer : buffers) {
		if (skipGLCalls && buffer)
			buffer->buffer_ = 0;
		delete buffer;
	}
	buffers.clear();
	for (auto texture : textures) {
		if (skipGLCalls && texture)
			texture->texture = 0;
		delete texture;
	}
	textures.clear();
	for (auto inputLayout : inputLayouts) {
		// No GL objects in an inputLayout yet
		delete inputLayout;
	}
	inputLayouts.clear();
	for (auto framebuffer : framebuffers) {
		if (skipGLCalls) {
			if (framebuffer->delayedReadback) {
				framebuffer->delayedReadback->Destroy(true);
			}
			framebuffer->handle = 0;
			framebuffer->color_texture.texture = 0;
			framebuffer->z_stencil_buffer = 0;
			framebuffer->z_stencil_texture.texture = 0;
			framebuffer->z_buffer = 0;
			framebuffer->stencil_buffer = 0;
		}
		delete framebuffer;
	}
	framebuffers.clear();
}
