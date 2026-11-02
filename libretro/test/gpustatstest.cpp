/* The GPU's per-frame statistics count one frame: the libretro core used to leave them growing
 * for the whole session, until the hashed-bytes count overflowed its int. Runs a gpu test and
 * checks the draw count falls back to zero on frames that draw nothing. */
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "GPU/GPU.h"
#include "libretro/libretro-common/include/libretro.h"

static const char *systemDir, *saveDir;
static bool shutdownRequested;
static unsigned long videoFrames;

static void Log(enum retro_log_level level, const char *fmt, ...) {
	(void)level;
	(void)fmt;
}

static bool Environment(unsigned cmd, void *data) {
	switch (cmd) {
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
		((struct retro_log_callback *)data)->log = Log;
		return true;
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
		*(const char **)data = systemDir;
		return true;
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
		*(const char **)data = saveDir;
		return true;
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
	case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
	case RETRO_ENVIRONMENT_SET_GEOMETRY:
	case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
	case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
	case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
	case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
		return true;
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
		*(bool *)data = false;
		return true;
	case RETRO_ENVIRONMENT_SHUTDOWN:
		shutdownRequested = true;
		return true;
	default:
		return false;
	}
}

static void Video(const void *data, unsigned width, unsigned height, size_t pitch) {
	if (data)
		videoFrames++;
}
static size_t AudioBatch(const int16_t *data, size_t frames) { return frames; }
static void Audio(int16_t l, int16_t r) {}
static void InputPoll() {}
static int16_t InputState(unsigned port, unsigned device, unsigned index, unsigned id) { return 0; }

int main(int argc, char **argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: gpustatstest test.prx\n");
		return 2;
	}
	systemDir = getenv("LRTEST_SYSTEM") ? getenv("LRTEST_SYSTEM") : "/tmp";
	saveDir = getenv("LRTEST_SAVE") ? getenv("LRTEST_SAVE") : "/tmp";
	retro_set_environment(Environment);
	retro_set_video_refresh(Video);
	retro_set_audio_sample(Audio);
	retro_set_audio_sample_batch(AudioBatch);
	retro_set_input_poll(InputPoll);
	retro_set_input_state(InputState);
	retro_init();
	struct retro_game_info info;
	memset(&info, 0, sizeof(info));
	info.path = argv[1];
	if (!retro_load_game(&info)) {
		fprintf(stderr, "gpustatstest: retro_load_game failed: FAIL\n");
		return 1;
	}

	/* The boot is deferred: frames before it come back as dupes. */
	int drawFrames = 0, idleAfterDraws = 0, polls = 0;
	for (int frame = 0; frame < 120 && !shutdownRequested && polls < 60000;) {
		const unsigned long before = videoFrames;
		retro_run();
		if (videoFrames == before) {
			polls++;
			usleep(1000);
			continue;
		}
		frame++;
		const int draws = gpuStats.perFrame.numEnqueue + gpuStats.perFrame.numDrawSyncs + gpuStats.perFrame.numListSyncs;
		if (getenv("GPUSTATS_VERBOSE")) printf("%d ", draws);
		if (draws > 0)
			drawFrames++;
		else if (drawFrames > 0)
			idleAfterDraws++;
	}
	retro_unload_game();
	retro_deinit();

	const bool ok = drawFrames > 0 && idleAfterDraws > 0;
	printf("gpustatstest: %d frames drew, %d idle frames after them counted no draws: %s\n",
		drawFrames, idleAfterDraws, ok ? "ok" : "FAIL");
	return ok ? 0 : 1;
}
