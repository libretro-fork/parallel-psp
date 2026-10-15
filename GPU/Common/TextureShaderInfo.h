// Copyright (c) 2013- PPSSPP Project.

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


// Postprocessing shader manager
// For FXAA, "Natural", bloom, B&W, cross processing and whatnot.

#pragma once

#include <string>
#include <vector>

#include "Common/GPU/thin3d.h"

struct TextureShaderInfo {
	Path iniFile;
	std::string section;
	std::string name;

	Path computeShaderFile;
	std::vector<Path> computeShaderFiles;

	// Upscaling shaders have a fixed scale factor.
	int scaleFactor;

	// Some shaders need to pass some data to the shader, and this is the buffer for that. It's up to the shader to interpret it.
	Path constantBuffer;
	bool hidden = false;

	bool operator == (const std::string &other) const {
		return name == other;
	}
	bool operator == (const TextureShaderInfo &other) const {
		return name == other.name;
	}

	bool operator < (const TextureShaderInfo &other) const {
		if (name < other.name) return true;
		if (name > other.name) return false;
		// Tie breaker
		if (iniFile < other.iniFile) return true;
		if (iniFile > other.iniFile) return false;
		return false;
	}
};

void ReloadAllTextureShaderInfo(Draw::DrawContext *draw);
const TextureShaderInfo *GetTextureShaderInfo(std::string_view name);
const std::vector<TextureShaderInfo> &GetAllTextureShaderInfo();
