#pragma once

#include <vector>
#include <string>
#include <set>

#include <retro_atomic.h>

#include "Common/GPU/OpenGL/GLCommon.h"

class GLRShader;
class GLRBuffer;
class GLRTexture;
class GLRInputLayout;
class GLRFramebuffer;
class GLPushBuffer;
class GLRProgram;
class GLRenderManager;

class GLDeleter {
public:
	void Perform(GLRenderManager *renderManager, bool skipGLCalls);

	bool IsEmpty() const {
		return shaders.empty() && programs.empty() && buffers.empty() && textures.empty() && inputLayouts.empty() && framebuffers.empty() && pushBuffers.empty();
	}

	void Take(GLDeleter &other);

	std::vector<GLRShader *> shaders;
	std::vector<GLRProgram *> programs;
	std::vector<GLRBuffer *> buffers;
	std::vector<GLRTexture *> textures;
	std::vector<GLRInputLayout *> inputLayouts;
	std::vector<GLRFramebuffer *> framebuffers;
	std::vector<GLPushBuffer *> pushBuffers;
};

// Texture data the render thread uploads from, written by the thread that records the frame.
// Reset once the frame slot comes round again, when the render thread is done with it, so an
// upload costs no allocation: large ones were a fresh, page-faulting mapping each on Windows.
class GLUploadArena {
public:
	GLUploadArena() = default;
	GLUploadArena(const GLUploadArena &) = delete;
	GLUploadArena &operator=(const GLUploadArena &) = delete;
	~GLUploadArena();

	// 16-byte aligned, valid until Reset().
	uint8_t *Allocate(size_t size);
	void Reset();
	size_t Retained() const;

private:
	struct Chunk {
		uint8_t *data;
		size_t size;
	};
	std::vector<Chunk> chunks_;
	size_t chunk_ = 0;
	size_t offset_ = 0;
	size_t used_ = 0;
};

struct GLQueueProfileContext {
	bool enabled;
	double cpuStartTime;
	double cpuEndTime;
	std::string passesString;
	int commandCounts[25];  // Can't grab count from the enum as it would mean a circular include. Might clean this up later.
};


// Per-frame data, round-robin so we can overlap submission with execution of the previous frame.
struct GLFrameData {
	bool skipSwap = false;

	// Frames need unique IDs to wait for present on, let's keep them here.
	// Also used for indexing into the frame timing history buffer.
	uint64_t frameId;

	// Set by the render thread once it has presented this frame, cleared by BeginFrame.
	retro_atomic_int_t readyForFence{ 1 };

	// Swapchain.
	bool hasBegun = false;

	GLDeleter deleter;
	GLDeleter deleter_prev;
	// Recording thread only.
	GLUploadArena upload;
	// Render thread only.
	std::set<GLPushBuffer *> activePushBuffers;

	GLQueueProfileContext profile;
};
