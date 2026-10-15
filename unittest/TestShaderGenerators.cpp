#include "ppsspp_config.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#if !PPSSPP_PLATFORM(WINDOWS)
#include <unistd.h>
#include <sys/wait.h>
#include <cerrno>
#endif

#include "Common/StringUtils.h"
#include "Common/File/FileUtil.h"

#include "GPU/Common/ShaderId.h"
#include "GPU/Common/ShaderCommon.h"
#include "GPU/Common/GPUStateUtils.h"
#include "Common/Data/Random/Rng.h"

#include "GPU/Vulkan/VulkanContext.h"

#include "GPU/Common/FragmentShaderGenerator.h"
#include "GPU/Common/VertexShaderGenerator.h"
#include "GPU/Common/ReinterpretFramebuffer.h"
#include "GPU/Common/StencilCommon.h"
#include "GPU/Common/DepalettizeShaderCommon.h"

#include "UnitTest.h"

#if PPSSPP_PLATFORM(WINDOWS)
#include <wrl/client.h>
#include "GPU/D3D11/D3D11Util.h"
#include "GPU/D3D11/D3D11Loader.h"
#endif

static constexpr size_t CODE_BUFFER_SIZE = 32768;

bool GenerateFShader(FShaderID id, char *buffer, ShaderLanguage lang, Draw::Bugs bugs, std::string *errorString) {
	buffer[0] = '\0';

	FragmentShaderFlags flags;

	uint64_t uniformMask;
	switch (lang) {
	case ShaderLanguage::GLSL_VULKAN:
	{
		ShaderLanguageDesc compat(ShaderLanguage::GLSL_VULKAN);
		return GenerateFragmentShader(id, buffer, compat, bugs, &uniformMask, &flags, errorString);
	}
	case ShaderLanguage::GLSL_1xx:
	{
		ShaderLanguageDesc compat(ShaderLanguage::GLSL_1xx);
		return GenerateFragmentShader(id, buffer, compat, bugs, &uniformMask, &flags, errorString);
	}
	case ShaderLanguage::GLSL_3xx:
	{
		ShaderLanguageDesc compat(ShaderLanguage::GLSL_3xx);
		return GenerateFragmentShader(id, buffer, compat, bugs, &uniformMask, &flags, errorString);
	}
	case ShaderLanguage::HLSL_D3D9:
	case ShaderLanguage::HLSL_D3D11:
	{
		ShaderLanguageDesc compat(lang);
		return GenerateFragmentShader(id, buffer, compat, bugs, &uniformMask, &flags, errorString);
	}
	default:
		return false;
	}
}

bool GenerateVShader(VShaderID id, char *buffer, ShaderLanguage lang, Draw::Bugs bugs, std::string *errorString) {
	buffer[0] = '\0';

	VertexShaderFlags flags;

	uint32_t attrMask;
	uint64_t uniformMask;
	switch (lang) {
	case ShaderLanguage::GLSL_VULKAN:
	{
		ShaderLanguageDesc compat(ShaderLanguage::GLSL_VULKAN);
		return GenerateVertexShader(id, buffer, compat, bugs, &attrMask, &uniformMask, &flags, errorString);
	}
	case ShaderLanguage::GLSL_1xx:
	{
		ShaderLanguageDesc compat(ShaderLanguage::GLSL_1xx);
		return GenerateVertexShader(id, buffer, compat, bugs, &attrMask, &uniformMask, &flags, errorString);
	}
	case ShaderLanguage::GLSL_3xx:
	{
		ShaderLanguageDesc compat(ShaderLanguage::GLSL_3xx);
		return GenerateVertexShader(id, buffer, compat, bugs, &attrMask, &uniformMask, &flags, errorString);
	}
	case ShaderLanguage::HLSL_D3D9:
	case ShaderLanguage::HLSL_D3D11:
	{
		ShaderLanguageDesc compat(lang);
		return GenerateVertexShader(id, buffer, compat, bugs, &attrMask, &uniformMask, &flags, errorString);
	}
	default:
		return false;
	}
}

// Shader syntax checks use a development tool, never a compiler linked into
// the core. Bank routing and specialization are tested independently.
static bool ValidateGLSL(const char *source, ShaderLanguage language, ShaderStage stage, std::string *error) {
	const char *stageName = stage == ShaderStage::Vertex ? "vert" : stage == ShaderStage::Fragment ? "frag" : "comp";
	bool vulkan = language == GLSL_VULKAN;
	int result = -1;
#if PPSSPP_PLATFORM(WINDOWS)
	wchar_t directory[MAX_PATH], filename[MAX_PATH];
	if (!GetTempPathW(MAX_PATH, directory) || !GetTempFileNameW(directory, L"psp", 0, filename)) {
		*error = "Cannot create temporary shader file";
		return false;
	}
	bool written = File::WriteStringToFile(false, source, Path(std::wstring(filename)));
#else
	char filename[] = "/tmp/ppsspp-shader-XXXXXX";
	int fd = mkstemp(filename);
	bool written = false;
	if (fd >= 0) {
		close(fd);
		written = File::WriteStringToFile(false, source, Path(filename));
	}
#endif
	if (written) {
#if PPSSPP_PLATFORM(WINDOWS)
		const wchar_t *wideStage = stage == ShaderStage::Vertex ? L"vert" : stage == ShaderStage::Fragment ? L"frag" : L"comp";
		std::wstring output = std::wstring(filename) + L".spv";
		std::wstring command = L"glslangValidator.exe ";
		if (vulkan) command += L"-V -o \"" + output + L"\" ";
		command += std::wstring(L"-S ") + wideStage + L" \"" + filename + L"\"";
		STARTUPINFOW startup{}; startup.cb = sizeof(startup);
		PROCESS_INFORMATION process{};
		if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
			WaitForSingleObject(process.hProcess, INFINITE);
			DWORD exitCode;
			if (GetExitCodeProcess(process.hProcess, &exitCode)) result = (int)exitCode;
			CloseHandle(process.hThread); CloseHandle(process.hProcess);
		}
		DeleteFileW(output.c_str());
#else
		std::string output = std::string(filename) + ".spv";
		pid_t child = fork();
		if (child == 0) {
			if (vulkan) execlp("glslangValidator", "glslangValidator", "-V", "-S", stageName, "-o", output.c_str(), filename, (char *)nullptr);
			else execlp("glslangValidator", "glslangValidator", "-S", stageName, filename, (char *)nullptr);
			_exit(127);
		}
		if (child > 0) {
			int status;
			pid_t waited;
			do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
			if (waited == child && WIFEXITED(status)) result = WEXITSTATUS(status);
		}
		unlink(output.c_str());
#endif
	}
#if PPSSPP_PLATFORM(WINDOWS)
	DeleteFileW(filename);
#else
	unlink(filename);
#endif
	if (result != 0) *error = "GLSL validation failed (glslangValidator must be available on PATH for unit tests)";
	return result == 0;
}

bool TestCompileShader(const char *buffer, ShaderLanguage lang, ShaderStage stage, std::string *errorMessage) {
	switch (lang) {
#if PPSSPP_PLATFORM(WINDOWS)
	case ShaderLanguage::HLSL_D3D9:
	case ShaderLanguage::HLSL_D3D11:
	{
		const char *programType = nullptr;
		switch (stage) {
		case ShaderStage::Vertex: programType = lang == HLSL_D3D9 ? "vs_3_0" : "vs_4_0"; break;
		case ShaderStage::Fragment: programType = lang == HLSL_D3D9 ? "ps_3_0" : "ps_4_0"; break;
		default:
			*errorMessage = "Unknown shader stage";
			return false;
		}
		auto output = CompileShaderToBytecodeD3D11(buffer, strlen(buffer), programType, 0, errorMessage);
		if (output.empty() && errorMessage->empty()) {
			*errorMessage = "Error compiling HLSL shader: bytecode empty";
		}
		return !output.empty();
	}
#endif

	case ShaderLanguage::GLSL_VULKAN:
	case ShaderLanguage::GLSL_1xx:
	case ShaderLanguage::GLSL_3xx:
		return ValidateGLSL(buffer, lang, stage, errorMessage);
	default:
		*errorMessage = "Unknown shader language";
		return false;
	}
}

void PrintDiff(const char *a, const char *b) {
	// Stupidest diff ever: Just print both lines, and a few around it, when we find a mismatch.
	std::vector<std::string> a_lines;
	std::vector<std::string> b_lines;
	SplitString(a, '\n', a_lines);
	SplitString(b, '\n', b_lines);
	for (size_t i = 0; i < a_lines.size() && i < b_lines.size(); i++) {
		if (a_lines[i] != b_lines[i]) {
			// Print some context
			for (size_t j = std::max((int)i - 4, 0); j < i; j++) {
				printf("%s\n", a_lines[j].c_str());
			}
			printf("DIFF found at line %d:\n", (int)i);
			printf("a: %s\n", a_lines[i].c_str());
			printf("b: %s\n", b_lines[i].c_str());
			printf("...continues...\n");
			for (size_t j = i + 1; j < i + 5 && j < a_lines.size() && j < b_lines.size(); j++) {
				printf("a: %s\n", a_lines[j].c_str());
				printf("b: %s\n", b_lines[j].c_str());
			}
			printf("==================\n");
			return;
		}
	}
}

const char *ShaderLanguageToString(ShaderLanguage lang) {
	switch (lang) {
	case HLSL_D3D9: return "HLSL_D3D9";
	case HLSL_D3D11: return "HLSL_D3D11";
	case GLSL_VULKAN: return "GLSL_VULKAN";
	case GLSL_1xx: return "GLSL_1xx";
	case GLSL_3xx: return "GLSL_3xx";
	default: return "N/A";
	}
}

bool TestReinterpretShaders() {
	Draw::Bugs bugs;

	ShaderLanguage languages[] = {
#if PPSSPP_PLATFORM(WINDOWS)
		ShaderLanguage::HLSL_D3D9,
		ShaderLanguage::HLSL_D3D11,
#endif
		ShaderLanguage::GLSL_VULKAN,
		ShaderLanguage::GLSL_3xx,
	};
	GEBufferFormat fmts[3] = {
		GE_FORMAT_565,
		GE_FORMAT_5551,
		GE_FORMAT_4444,
	};
	char *buffer = new char[65536];

	// Generate all despite failures - it's only 6.
	bool failed = false;

	for (int k = 0; k < ARRAY_SIZE(languages); k++) {
		if (g_testLog) {
			printf("=== %s ===\n\n", ShaderLanguageToString(languages[k]));
		}

		ShaderLanguageDesc desc(languages[k]);

		std::string errorMessage;

		for (int i = 0; i < 3; i++) {
			for (int j = 0; j < 3; j++) {
				if (i == j)
					continue;  // useless shader!
				ShaderWriter writer(buffer, desc, ShaderStage::Fragment);
				GenerateReinterpretFragmentShader(writer, fmts[i], fmts[j]);
				if (strlen(buffer) >= 8192) {
					printf("Reinterpret fragment shader %d exceeded buffer:\n\n%s\n", (int)j, LineNumberString(buffer).c_str());
					failed = true;
				}
				if (!TestCompileShader(buffer, languages[k], ShaderStage::Fragment, &errorMessage)) {
					printf("Error compiling reinterpret fragment shader %d:\n\n%s\n\n%s\n", (int)j, LineNumberString(buffer).c_str(), errorMessage.c_str());
					failed = true;
				} else {
					if (g_testLog) {
						printf("===\n%s\n===\n", buffer);
					}
				}
			}
		}
	}

	delete[] buffer;
	return !failed;
}

bool TestStencilShaders() {
	Draw::Bugs bugs;

	ShaderLanguage languages[] = {
#if PPSSPP_PLATFORM(WINDOWS)
		ShaderLanguage::HLSL_D3D9,
		ShaderLanguage::HLSL_D3D11,
#endif
		ShaderLanguage::GLSL_VULKAN,
		ShaderLanguage::GLSL_3xx,
	};

	char *buffer = new char[65536];

	bool failed = false;

	for (int k = 0; k < ARRAY_SIZE(languages); k++) {
		if (g_testLog) {
			printf("=== %s ===\n\n", ShaderLanguageToString(languages[k]));
		}

		ShaderLanguageDesc desc(languages[k]);
		std::string errorMessage;

		// Generate all despite failures - it's only a few.
		// Only use export on Vulkan, because GLSL_3xx is ES which doesn't support stencil export.
		bool allowUseExport = languages[k] == ShaderLanguage::GLSL_VULKAN;
		for (int useExport = 0; useExport <= (allowUseExport ? 1 : 0); ++useExport) {
			GenerateStencilFs(buffer, desc, bugs, useExport == 1);
			if (strlen(buffer) >= 8192) {
				printf("Stencil fragment shader (useExport=%d) exceeded buffer:\n\n%s\n", useExport, LineNumberString(buffer).c_str());
				failed = true;
			}
			if (!TestCompileShader(buffer, languages[k], ShaderStage::Fragment, &errorMessage)) {
				printf("Error compiling stencil shader (useExport=%d):\n\n%s\n\n%s\n", useExport, LineNumberString(buffer).c_str(), errorMessage.c_str());
				failed = true;
			} else {
				if (g_testLog) {
					printf("===\n%s\n===\n", buffer);
				}
			}
		}

		GenerateStencilVs(buffer, desc);
		if (strlen(buffer) >= 8192) {
			printf("Stencil vertex shader exceeded buffer:\n\n%s\n", LineNumberString(buffer).c_str());
			failed = true;
		}
		if (!TestCompileShader(buffer, languages[k], ShaderStage::Vertex, &errorMessage)) {
			printf("Error compiling stencil shader:\n\n%s\n\n%s\n", LineNumberString(buffer).c_str(), errorMessage.c_str());
			failed = true;
		} else {
			if (g_testLog) {
				printf("===\n%s\n===\n", buffer);
			}
		}
	}

	delete[] buffer;
	return !failed;
}

bool TestDepalShaders() {
	Draw::Bugs bugs;

	ShaderLanguage languages[] = {
#if PPSSPP_PLATFORM(WINDOWS)
		ShaderLanguage::HLSL_D3D9,
		ShaderLanguage::HLSL_D3D11,
#endif
		ShaderLanguage::GLSL_VULKAN,
		ShaderLanguage::GLSL_3xx,
		ShaderLanguage::GLSL_1xx,
	};

	char *buffer = new char[65536];

	for (int k = 0; k < ARRAY_SIZE(languages); k++) {
		if (g_testLog) {
			printf("=== %s ===\n\n", ShaderLanguageToString(languages[k]));
		}

		ShaderLanguageDesc desc(languages[k]);
		std::string errorMessage;

		// TODO: Try some different configurations of the fragment shader.
		// But first just try one.
		DepalConfig config{};
		config.clutFormat = GE_CMODE_16BIT_ABGR4444;
		config.shift = 8;
		config.startPos = 64;
		config.mask = 0xFF;
		config.bufferFormat = GE_FORMAT_8888;
		config.textureFormat = GE_TFMT_CLUT32;
		config.depthUpperBits = 0;

		ShaderWriter writer(buffer, desc, ShaderStage::Fragment);
		GenerateDepalFs(writer, config);
		if (strlen(buffer) >= 8192) {
			printf("Depal shader exceeded buffer:\n\n%s\n", LineNumberString(buffer).c_str());
			delete[] buffer;
			return false;
		}
		if (!TestCompileShader(buffer, languages[k], ShaderStage::Fragment, &errorMessage)) {
			printf("Error compiling depal shader:\n\n%s\n\n%s\n", LineNumberString(buffer).c_str(), errorMessage.c_str());
			delete[] buffer;
			return false;
		} else {
			if (g_testLog) {
				printf("===\n%s\n===\n", buffer);
			}
		}
	}

	delete[] buffer;
	return true;
}

const ShaderLanguage languages[] = {
#if PPSSPP_PLATFORM(WINDOWS)
	ShaderLanguage::HLSL_D3D9,
	ShaderLanguage::HLSL_D3D11,
#endif
	ShaderLanguage::GLSL_VULKAN,
	ShaderLanguage::GLSL_1xx,
	ShaderLanguage::GLSL_3xx,
};
const int numLanguages = ARRAY_SIZE(languages);

bool TestVertexShaders() {
	char *buffer[numLanguages];

	for (int i = 0; i < numLanguages; i++) {
		buffer[i] = new char[65536];
	}
	GMRng rng;
	int successes = 0;
	int count = 700;

	Draw::Bugs bugs;

	// Generate a bunch of random vertex shader IDs, try to generate shader source.
	// Then compile it and check that it's ok.
	for (int i = 0; i < count; i++) {
		uint64_t id64 = rng.R64();
		VShaderID id;
		id.FromUint64(id64);

		// The generated bits need some adjustment:

		// If mode is through, we won't do hardware transform.
		if (id.Bit(VS_BIT_IS_THROUGH)) {
			id.SetBit(VS_BIT_USE_HW_TRANSFORM, 0);
		}
		if (id.Bit(VS_BIT_VERTEX_RANGE_CULLING)) {
			continue;
		}

		bool generateSuccess[numLanguages]{};
		std::string genErrorString[numLanguages];

		for (int j = 0; j < numLanguages; j++) {
			generateSuccess[j] = GenerateVShader(id, buffer[j], languages[j], bugs, &genErrorString[j]);
			if (!genErrorString[j].empty()) {
				if (g_testLog) {
					printf("%s\n", genErrorString[j].c_str());
				}
			}
		}

		for (int j = 0; j < numLanguages; j++) {
			if (strlen(buffer[j]) >= CODE_BUFFER_SIZE) {
				printf("Vertex shader exceeded buffer:\n\n%s\n", LineNumberString(buffer[j]).c_str());
				for (int i = 0; i < numLanguages; i++) {
					delete[] buffer[i];
				}
				return false;
			}
		}

		// Now that we have the strings ready for easy comparison (buffer,4 in the watch window),
		// let's try to compile them.
		for (int j = 0; j < numLanguages; j++) {
			if (generateSuccess[j]) {
				std::string errorMessage;
				if (!TestCompileShader(buffer[j], languages[j], ShaderStage::Vertex, &errorMessage)) {
					printf("Error compiling vertex shader %d:\n\n%s\n\nERROR: %s\n\n(end of error)\n\n", (int)j, LineNumberString(buffer[j]).c_str(), errorMessage.c_str());
					for (int i = 0; i < numLanguages; i++) {
						delete[] buffer[i];
					}
					return false;
				}
				successes++;
			}
		}
	}

	if (g_testLog) {
		printf("%d/%d vertex shaders generated (it's normal that it's not all, there are invalid bit combos)\n", successes, count * numLanguages);
	}

	for (int i = 0; i < numLanguages; i++) {
		delete[] buffer[i];
	}
	return true;
}

bool TestFragmentShaders() {
	char *buffer[numLanguages];

	for (int i = 0; i < numLanguages; i++) {
		buffer[i] = new char[65536];
	}
	GMRng rng;
	int successes = 0;
	int count = 300;

	Draw::Bugs bugs;

	// Generate a bunch of random fragment shader IDs, try to generate shader source.
	// Then compile it and check that it's ok.
	for (int i = 0; i < count; i++) {
		uint64_t id64 = rng.R64();
		FShaderID id;
		id.FromUint64(id64);

		// bits we don't need to test because they are irrelevant on d3d11
		id.SetBit(FS_BIT_NO_DEPTH_CANNOT_DISCARD_STENCIL, false);

		// DX9 disabling:
		if (static_cast<ReplaceAlphaType>(id.Bits(FS_BIT_STENCIL_TO_ALPHA, 2)) == ReplaceAlphaType::REPLACE_ALPHA_DUALSOURCE)
			continue;

		bool generateSuccess[numLanguages]{};
		std::string genErrorString[numLanguages];

		for (int j = 0; j < numLanguages; j++) {
			generateSuccess[j] = GenerateFShader(id, buffer[j], languages[j], bugs, &genErrorString[j]);
			if (!genErrorString[j].empty()) {
				if (g_testLog) {
					printf("%s\n", genErrorString[j].c_str());
				}
			}
			// We ignore the contents of the error string here, not even gonna try to compile if it errors.
		}

		for (int j = 0; j < numLanguages; j++) {
			if (strlen(buffer[j]) >= CODE_BUFFER_SIZE) {
				printf("Fragment shader exceeded buffer:\n\n%s\n", LineNumberString(buffer[j]).c_str());
				for (int i = 0; i < numLanguages; i++) {
					delete[] buffer[i];
				}
				return false;
			}
		}

		// Now that we have the strings ready for easy comparison (buffer,4 in the watch window),
		// let's try to compile them.
		for (int j = 0; j < numLanguages; j++) {
			if (generateSuccess[j]) {
				std::string errorMessage;
				if (!TestCompileShader(buffer[j], languages[j], ShaderStage::Fragment, &errorMessage)) {
					printf("Error compiling fragment shader %d:\n\n%s\n\nERROR: %s\n\n(end of error)\n\n", (int)j, LineNumberString(buffer[j]).c_str(), errorMessage.c_str());
					for (int i = 0; i < numLanguages; i++) {
						delete[] buffer[i];
					}
					return false;
				}
				successes++;
			}
		}
	}

	if (g_testLog) {
		printf("%d/%d fragment shaders generated (it's normal that it's not all, there are invalid bit combos)\n", successes, count * numLanguages);
	}

	for (int i = 0; i < numLanguages; i++) {
		delete[] buffer[i];
	}
	return true;
}

bool TestShaderGenerators() {
#if PPSSPP_PLATFORM(WINDOWS)
	LoadD3D11();
#else
#endif

	if (!TestStencilShaders()) {
		return false;
	}

	if (!TestReinterpretShaders()) {
		return false;
	}

	if (!TestDepalShaders()) {
		return false;
	}

	if (!TestFragmentShaders()) {
		return false;
	}

	if (!TestVertexShaders()) {
		return false;
	}

	return true;
} 
