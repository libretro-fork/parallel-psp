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
#pragma once

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include "Common/GPU/thin3d.h"
#include "Common/TimeUtil.h"

namespace Draw {

// Request-thread diagnostics, enabled only by PPSSPP_READBACK_STATS=1.
// Elapsed time includes waiting, conversion and CPU copies; it is not GPU time.
class ReadbackStats {
	struct Row {
		uint64_t calls = 0, blocking = 0, requestedBytes = 0, downloadBytes = 0, coalesced = 0;
		double elapsed = 0.0;
	};
public:
	explicit ReadbackStats(const char *backend) : backend_(backend) {
		const char *setting = getenv("PPSSPP_READBACK_STATS");
		enabled_ = setting && !strcmp(setting, "1");
	}
	~ReadbackStats() {
		for (const auto &entry : rows_) {
			const Row &row = entry.second;
			printf("ReadbackStats backend=%s tag=%s calls=%llu blocking_requests=%llu requested_bytes=%llu download_attempt_bytes=%llu coalesced=%llu elapsed_ms=%.3f\n",
				backend_, entry.first.c_str(), (unsigned long long)row.calls, (unsigned long long)row.blocking,
				(unsigned long long)row.requestedBytes, (unsigned long long)row.downloadBytes,
				(unsigned long long)row.coalesced, row.elapsed * 1000.0);
		}
	}
	class Scope {
	public:
		Scope(ReadbackStats &stats, const char *tag, ReadbackMode mode, int width, int height, DataFormat format) {
			if (stats.enabled_) {
				row_ = &stats.rows_[tag ? tag : "(untagged)"];
				++row_->calls;
				row_->blocking += mode == ReadbackMode::BLOCK;
				if (width > 0 && height > 0) row_->requestedBytes += (uint64_t)width * height * DataFormatSizeInBytes(format);
				start_ = time_now_d();
			}
		}
		~Scope() { if (row_) row_->elapsed += time_now_d() - start_; }
		void Download(uint64_t bytes) { if (row_) row_->downloadBytes += bytes; }
		void Coalesced(size_t count) { if (row_) row_->coalesced += count; }
	private:
		Row *row_ = nullptr;
		double start_ = 0.0;
	};
private:
	const char *backend_;
	bool enabled_ = false;
	std::map<std::string, Row> rows_;
};

}  // namespace Draw
