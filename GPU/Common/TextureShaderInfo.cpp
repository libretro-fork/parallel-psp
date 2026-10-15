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


// Texture scaling shader definitions

#include <string>
#include <vector>
#include <algorithm>

#include "Common/Log.h"
#include "Common/Data/Format/IniFile.h"
#include "Common/File/DirListing.h"
#include "Common/File/VFS/VFS.h"
#include "Common/GPU/OpenGL/GLFeatures.h"
#include "Common/GPU/thin3d.h"
#include "Common/StringUtils.h"

#include "Core/System.h"
#include "GPU/Common/TextureShaderInfo.h"

static std::vector<TextureShaderInfo> textureShaderInfo;

static Draw::GPUVendor VendorFromString(const std::string &vendor) {
	// TODO: This should probably be a function somewhere.
	if (vendor == "ARM") {
		return Draw::GPUVendor::VENDOR_ARM;
	} else if (vendor == "Qualcomm") {
		return Draw::GPUVendor::VENDOR_QUALCOMM;
	} else if (vendor == "IMGTEC") {
		return Draw::GPUVendor::VENDOR_IMGTEC;
	} else if (vendor == "NVIDIA") {
		return Draw::GPUVendor::VENDOR_NVIDIA;
	} else if (vendor == "AMD") {
		return Draw::GPUVendor::VENDOR_AMD;
	} else if (vendor == "Broadcom") {
		return Draw::GPUVendor::VENDOR_BROADCOM;
	} else if (vendor == "Apple") {
		return Draw::GPUVendor::VENDOR_APPLE;
	} else if (vendor == "Intel") {
		return Draw::GPUVendor::VENDOR_INTEL;
	} else if (vendor == "Mesa") {
		return Draw::GPUVendor::VENDOR_MESA;
	} else if (vendor == "Vivante") {
		return Draw::GPUVendor::VENDOR_VIVANTE;
	}
	return Draw::GPUVendor::VENDOR_UNKNOWN;
}

// Scans the directories for shader ini files and collects info about all the shaders found.

void LoadTextureShaderInfo(Draw::DrawContext *draw, const std::vector<Path> &directories) {

	Draw::GPUVendor gpuVendor = Draw::GPUVendor::VENDOR_UNKNOWN;
	if (draw) {
		gpuVendor = draw->GetDeviceCaps().vendor;
	}

	textureShaderInfo.clear();

	auto appendTextureShader = [&](const TextureShaderInfo &info) {
		auto beginErase = std::remove(textureShaderInfo.begin(), textureShaderInfo.end(), info.name);
		if (beginErase != textureShaderInfo.end()) {
			textureShaderInfo.erase(beginErase, textureShaderInfo.end());
		}
		textureShaderInfo.push_back(info);
	};

	for (size_t d = 0; d < directories.size(); d++) {
		std::vector<File::FileInfo> fileInfo;
		g_VFS.GetFileListing(directories[d].c_str(), &fileInfo, "ini:");

		if (fileInfo.empty()) {
			File::GetFilesInDir(directories[d], &fileInfo, "ini:");
		}

		for (size_t f = 0; f < fileInfo.size(); f++) {
			IniFile ini;
			bool success = false;
			if (fileInfo[f].isDirectory)
				continue;

			Path name = fileInfo[f].fullName;
			Path path = directories[d];
			// Hack around Android VFS path bug. really need to redesign this.
			if (name.ToString().substr(0, 7) == "assets/")
				name = Path(name.ToString().substr(7));
			if (path.ToString().substr(0, 7) == "assets/")
				path = Path(path.ToString().substr(7));

			if (ini.LoadFromVFS(g_VFS, name.ToString()) || ini.Load(fileInfo[f].fullName)) {
				success = true;
				// vsh load. meh.
			}

			if (!success)
				continue;

			// Alright, let's loop through the sections and see if any is a shader.
			for (size_t i = 0; i < ini.Sections().size(); i++) {
				Section &section = *(ini.Sections()[i].get());
				std::string shaderType = "render";
				section.Get("Type", &shaderType);

				std::vector<std::string> vendorBlacklist;
				section.Get("VendorBlacklist", &vendorBlacklist);
				bool skipped = false;
				for (auto &item : vendorBlacklist) {
					const Draw::GPUVendor blacklistedVendor = VendorFromString(item);
					if (blacklistedVendor == gpuVendor && blacklistedVendor != Draw::GPUVendor::VENDOR_UNKNOWN) {
						skipped = true;
						break;
					}
				}
				if (skipped) {
					continue;
				}

				if (section.HasKey("Compute") && strncasecmp(shaderType.c_str(), "texture", shaderType.size()) == 0) {
					// This is a texture shader.
					TextureShaderInfo info{};
					std::string temp;
					info.section = section.name();
					info.name = section.name();
					info.scaleFactor = 0;
					section.Get("Name", &info.name);
					section.Get("Scale", &info.scaleFactor);
					bool hidden = false;
					section.Get("Hidden", &info.hidden);
					std::string cbufferFilename;
					if (section.Get("ConstantBuffer", &cbufferFilename)) {
						Path cbufferPath = path / cbufferFilename;
						info.constantBuffer = cbufferPath;
					}
					if (section.Get("Compute", &temp)) {
						info.computeShaderFile = path / temp;
						info.computeShaderFiles.push_back(info.computeShaderFile);
						for (int computeIndex = 2; computeIndex <= 4; ++computeIndex) {
							temp.clear();
							if (section.Get(StringFromFormat("Compute%d", computeIndex).c_str(), &temp)) {
								info.computeShaderFiles.push_back(path / temp);
							}
						}
						if (info.scaleFactor >= 2 && info.scaleFactor < 8) {
							appendTextureShader(info);
						}
					} else {
						ERROR_LOG(Log::G3D, "Compute field missing for compute shader");
					}
				}
			}
		}
	}

	// Sort shaders alphabetically.
	std::sort(textureShaderInfo.begin(), textureShaderInfo.end());

	TextureShaderInfo textureOff{};
	textureOff.name = "Off";
	textureOff.section = "Off";
	textureShaderInfo.insert(textureShaderInfo.begin(), textureOff);

}

// Scans the directories for shader ini files and collects info about all the shaders found.
void ReloadAllTextureShaderInfo(Draw::DrawContext *draw) {
	std::vector<Path> directories;
	directories.push_back(Path("shaders"));  // For VFS
	directories.push_back(GetSysDirectory(DIRECTORY_CUSTOM_SHADERS));
	LoadTextureShaderInfo(draw, directories);
}

const TextureShaderInfo *GetTextureShaderInfo(std::string_view name) {
	for (auto &info : textureShaderInfo) {
		if (info.section == name) {
			return &info;
		}
	}
	return nullptr;
}
const std::vector<TextureShaderInfo> &GetAllTextureShaderInfo() {
	return textureShaderInfo;
}

