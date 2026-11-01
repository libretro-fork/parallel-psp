// SAS reverb across a savestate: a reverb loaded from another's state goes on exactly as that
// one would, for every preset, and one that only had its preset set does not.
#include <cstdio>
#include <cstring>
#include <vector>

#include "Common/Serialize/Serializer.h"
#include "Core/Config.h"
#include "Core/HW/SasAudio.h"
#include "Core/HW/SasReverb.h"

static const int GRAIN = 256;  // input samples a channel at 22 kHz, as SasInstance feeds it
static int failures;

static void Feed(SasReverb &r, int grain, int16_t *out) {
	int16_t in[GRAIN * 2];
	for (int i = 0; i < GRAIN * 2; i++) {
		// A burst, then silence: what follows the burst is the reverb tail alone.
		in[i] = grain < 4 ? (int16_t)(((i * 7919 + grain * 104729) & 0x3FFF) - 0x2000) : 0;
	}
	r.ProcessReverb(out, in, GRAIN, 0x8000, 0x8000);
}

static std::vector<u8> Save(SasReverb &r) {
	u8 *ptr = nullptr;
	PointerWrap measure(&ptr, PointerWrap::MODE_MEASURE);
	r.DoState(measure);
	std::vector<u8> buf(measure.Offset());
	ptr = buf.data();
	PointerWrap write(&ptr, PointerWrap::MODE_WRITE);
	r.DoState(write);
	return buf;
}

static void Preset(int preset) {
	SasReverb a, b, fresh;
	a.SetPreset(preset);
	a.SetParams(40, 90);
	int16_t outA[GRAIN * 4], outB[GRAIN * 4];
	for (int g = 0; g < 6; g++)
		Feed(a, g, outA);
	std::vector<u8> state = Save(a);

	b.SetPreset(preset);
	b.SetParams(40, 90);
	u8 *ptr = state.data();
	PointerWrap read(&ptr, PointerWrap::MODE_READ);
	read.SetReadEnd(state.data() + state.size());
	b.DoState(read);
	if (read.Failed()) {
		printf("FAIL: preset %d: load refused\n", preset);
		failures++;
		return;
	}
	fresh.SetPreset(preset);
	fresh.SetParams(40, 90);

	bool freshDiffers = false;
	for (int g = 6; g < 40; g++) {
		int16_t outFresh[GRAIN * 4];
		Feed(a, g, outA);
		Feed(b, g, outB);
		Feed(fresh, g, outFresh);
		if (memcmp(outA, outB, sizeof(outA)) != 0) {
			printf("FAIL: preset %d (%s): grain %d differs after a load\n", preset, SasReverb::GetPresetName(preset), g);
			failures++;
			return;
		}
		if (memcmp(outA, outFresh, sizeof(outA)) != 0)
			freshDiffers = true;
	}
	if (!freshDiffers) {
		printf("FAIL: preset %d (%s): the tail does not depend on the history\n", preset, SasReverb::GetPresetName(preset));
		failures++;
	}
}

int main() {
	// The reverb is muted at zero, which is where a config that was never loaded stands.
	g_Config.iReverbVolume = 100;
	for (int preset = PSP_SAS_EFFECT_TYPE_ROOM; preset <= PSP_SAS_EFFECT_TYPE_MAX; preset++)
		Preset(preset);

	// A state from another preset does not fit.
	SasReverb hall, room;
	hall.SetPreset(PSP_SAS_EFFECT_TYPE_HALL);
	room.SetPreset(PSP_SAS_EFFECT_TYPE_ROOM);
	std::vector<u8> state = Save(hall);
	u8 *ptr = state.data();
	PointerWrap read(&ptr, PointerWrap::MODE_READ);
	read.SetReadEnd(state.data() + state.size());
	room.DoState(read);
	if (!read.Failed()) {
		printf("FAIL: a hall state loaded into a room reverb\n");
		failures++;
	}

	if (failures)
		return 1;
	printf("sasreverbtest: every preset carries its tail across a load\n");
	return 0;
}
