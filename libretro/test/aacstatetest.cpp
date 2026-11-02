// The AAC decoder across a savestate: one loaded from another's state, at any frame, goes on
// exactly as that one would; one opened afresh does not.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "Common/Serialize/Serializer.h"
#include "Core/HW/SimpleAudioDec.h"

static int failures;

static std::vector<u8> ReadFile(const char *path) {
	std::ifstream f(path, std::ios::binary);
	return std::vector<u8>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// One ADTS frame at a time, as sceAudiocodec hands them over; its samples, or empty at the end.
static std::vector<s16> Next(AudioDecoder *d, const std::vector<u8> &aac, size_t &pos) {
	std::vector<s16> out(2048 * 2);
	int consumed = 0, samples = 0;
	if (pos >= aac.size())
		return std::vector<s16>();
	d->Decode(aac.data() + pos, (int)(aac.size() - pos), &consumed, 2, out.data(), &samples);
	if (consumed <= 0)
		return std::vector<s16>();
	pos += consumed;
	out.resize((size_t)samples * 2);
	return out;
}

static std::vector<u8> Save(AudioDecoder *d) {
	u8 *ptr = nullptr;
	PointerWrap measure(&ptr, PointerWrap::MODE_MEASURE);
	DoAudioDecoderState(measure, d);
	std::vector<u8> buf(measure.Offset());
	ptr = buf.data();
	PointerWrap write(&ptr, PointerWrap::MODE_WRITE);
	DoAudioDecoderState(write, d);
	return buf;
}

static bool Load(AudioDecoder *d, std::vector<u8> &state) {
	u8 *ptr = state.data();
	PointerWrap read(&ptr, PointerWrap::MODE_READ);
	read.SetReadEnd(state.data() + state.size());
	DoAudioDecoderState(read, d);
	return !read.Failed();
}

static void Stream(const char *path) {
	std::vector<u8> aac = ReadFile(path);
	if (aac.empty()) {
		printf("FAIL: %s: not readable\n", path);
		failures++;
		return;
	}
	std::vector<std::vector<s16>> ref;
	std::vector<size_t> at;
	{
		AudioDecoder *d = CreateAudioDecoder(PSP_CODEC_AAC, 44100, 2);
		size_t pos = 0;
		for (;;) {
			at.push_back(pos);
			std::vector<s16> f = Next(d, aac, pos);
			if (f.empty())
				break;
			ref.push_back(f);
		}
		delete d;
	}
	const int frames = (int)ref.size();
	int loads = 0, wrong = 0, freshSame = 0, resaved = 0;
	AudioDecoder *a = CreateAudioDecoder(PSP_CODEC_AAC, 44100, 2);
	size_t pos = 0;
	for (int k = 0; k < frames; k++) {
		if (k > 0) {
			std::vector<u8> state = Save(a);
			AudioDecoder *b = CreateAudioDecoder(PSP_CODEC_AAC, 44100, 2);
			AudioDecoder *fresh = CreateAudioDecoder(PSP_CODEC_AAC, 44100, 2);
			if (!Load(b, state)) {
				wrong++;
			} else {
				loads++;
				if (Save(b) != state)
					resaved++;
				size_t pb = pos, pf = pos;
				for (int i = k; i < frames && i < k + 4; i++) {
					if (Next(b, aac, pb) != ref[i]) {
						wrong++;
						break;
					}
				}
				if (Next(fresh, aac, pf) == ref[k])
					freshSame++;
			}
			delete b;
			delete fresh;
		}
		if (Next(a, aac, pos) != ref[k])
			wrong++;
	}
	delete a;
	printf("%s: %d frames, %d states loaded, %d wrong, %d saved differently, %d decoded right without the state\n",
		path, frames, loads, wrong, resaved, freshSame);
	if (frames < 10 || loads != frames - 1 || wrong || resaved || freshSame > frames / 4) {
		printf("FAIL: %s\n", path);
		failures++;
	}
}

int main(int argc, char **argv) {
	const char *dir = argc > 1 ? argv[1] : "pspautotests/tests/audio/audiocodec";
	const char *names[] = { "aac_64k.aac", "aac_128k.aac", "aac_192k.aac" };
	for (const char *n : names) {
		std::string path = std::string(dir) + "/" + n;
		Stream(path.c_str());
	}
	printf("aacstatetest: %s\n", failures ? "FAIL" : "ok");
	return failures ? 1 : 0;
}
