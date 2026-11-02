// The Media Engine's busy time across a savestate: a job queued behind one still running when the
// state was made waits for it after the load too, as it would have without one.
#include <cstdio>
#include <vector>

#include "Common/Serialize/Serializer.h"
#include "Core/CoreTiming.h"
#include "Core/HLE/sceVideocodec.h"
#include "Core/MIPS/MIPS.h"

static std::vector<u8> Save() {
	u8 *ptr = nullptr;
	PointerWrap measure(&ptr, PointerWrap::MODE_MEASURE);
	__VideocodecDoState(measure);
	std::vector<u8> buf(measure.Offset());
	ptr = buf.data();
	PointerWrap write(&ptr, PointerWrap::MODE_WRITE);
	__VideocodecDoState(write);
	return buf;
}

int main() {
	int failures = 0;
	CoreTiming::Init(currentMIPS);
	__VideocodecInit();

	// A 5ms job, then 2ms on: 3ms of it left.
	const int first = MEScheduleJob(5000);
	currentMIPS->downcount -= (int)usToCycles(2000);
	std::vector<u8> state = Save();
	const int straight = MEScheduleJob(1000);

	// The same moment again, from the state.
	__VideocodecInit();
	u8 *ptr = state.data();
	PointerWrap read(&ptr, PointerWrap::MODE_READ);
	read.SetReadEnd(state.data() + state.size());
	__VideocodecDoState(read);
	const int loaded = read.Failed() ? -1 : MEScheduleJob(1000);

	printf("first job %dus; a 1ms job 2ms later waits %dus, and after a load %dus\n", first, straight, loaded);
	if (first != 5000 || straight != 4000 || loaded != straight) {
		printf("FAIL: metimetest\n");
		failures++;
	}
	printf("metimetest: %s\n", failures ? "FAIL" : "ok");
	return failures ? 1 : 0;
}
