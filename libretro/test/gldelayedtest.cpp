// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// GLDelayedReadback's mailboxes, without GL: the render thread fills and
// publishes results, the emulation thread takes the latest and hands its
// storage back. Results must arrive whole and in order, only the latest is
// kept, and once warm nothing is allocated: the same few results and their
// pixel storage go round.

#include <cstdio>
#include <cstring>
#include <memory>
#include <set>

#include <rthreads/rthreads.h>
#include <retro_atomic.h>

#include "Common/GPU/OpenGL/GLQueueRunner.h"

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static const size_t kSize = 4096;

static void Fill(GLDelayedReadback &d, uint32_t seq) {
	GLDelayedReadbackResult *r = d.Acquire();
	r->rect = { (int)seq, 0, 32, 32 };
	r->pixels.resize(kSize);
	memset(r->pixels.data(), (int)(seq & 0xFF), kSize);
	d.Publish(r);
}

static void SingleThread() {
	GLDelayedReadback d;
	Fill(d, 1);
	std::unique_ptr<GLDelayedReadbackResult> r = d.Take();
	CHECK(r && r->rect.x == 1 && r->pixels.size() == kSize, "first result");
	CHECK(!d.Take(), "a result is taken once");
	GLDelayedReadbackResult *obj = r.get();
	const uint8_t *storage = r->pixels.data();
	d.Return(std::move(r));
	// The returned result, storage and all, is the next one filled.
	GLDelayedReadbackResult *again = d.Acquire();
	CHECK(again == obj, "returned result is reused");
	again->pixels.resize(kSize);
	CHECK(again->pixels.data() == storage, "its storage is reused");
	again->rect = { 2, 0, 32, 32 };
	d.Publish(again);

	// Only the latest is kept; the one never taken is filled next.
	Fill(d, 3);
	r = d.Take();
	CHECK(r && r->rect.x == 3 && r->pixels[0] == 3, "latest wins");
	GLDelayedReadbackResult *spare = d.Acquire();
	CHECK(spare == obj, "the overtaken result is filled next");
	d.Publish(spare);
	d.Return(std::move(r));
	r = d.Take();
	d.Return(std::move(r));

	// A failure publishes an empty result for the emulation thread to see.
	d.gpuRect = { 7, 8, 9, 10 };
	d.Fail();
	r = d.Take();
	CHECK(r && r->pixels.empty() && r->rect.x == 7 && d.failed, "failure is published empty");
}

struct Stress {
	GLDelayedReadback d;
	retro_atomic_int_t done{0};
	uint32_t count = 0;
};

static void Producer(void *arg) {
	Stress *s = (Stress *)arg;
	for (uint32_t seq = 1; seq <= s->count; seq++)
		Fill(s->d, seq);
	retro_atomic_store_release_int(&s->done, 1);
}

static void Concurrent() {
	Stress s;
	s.count = 200000;
	sthread_t *t = sthread_create(Producer, &s);
	std::vector<uint8_t> cached;
	std::set<const GLDelayedReadbackResult *> seen;
	uint32_t last = 0, taken = 0;
	for (;;) {
		const bool finished = retro_atomic_load_acquire_int(&s.done) != 0;
		std::unique_ptr<GLDelayedReadbackResult> r = s.d.Take();
		if (r) {
			const uint32_t seq = (uint32_t)r->rect.x;
			bool whole = r->pixels.size() == kSize;
			for (size_t i = 0; whole && i < kSize; i++)
				whole = r->pixels[i] == (uint8_t)(seq & 0xFF);
			CHECK(whole, "result %u arrived torn", seq);
			CHECK(seq > last, "result %u after %u", seq, last);
			last = seq;
			taken++;
			seen.insert(r.get());
			cached.swap(r->pixels);
			s.d.Return(std::move(r));
		} else if (finished) {
			break;
		}
	}
	sthread_join(t);
	CHECK(last == s.count, "the last result (%u) arrived, got %u", s.count, last);
	// Being filled, published, held by the emulation thread and on its way back: never more.
	CHECK(seen.size() <= 4, "%u result objects used", (unsigned)seen.size());
	printf("gldelayedtest: %u of %u results taken, %u objects\n", taken, s.count, (unsigned)seen.size());
}

int main() {
	SingleThread();
	Concurrent();
	if (failures) {
		printf("gldelayedtest: %d failures\n", failures);
		return 1;
	}
	printf("gldelayedtest: mailboxes pass\n");
	return 0;
}
