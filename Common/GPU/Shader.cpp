#include "Common/GPU/Shader.h"
#include <cstring>

#ifdef USE_CRT_DBG
#undef new
#endif


const char *ShaderLanguageAsString(ShaderLanguage lang) {
	switch (lang) {
	case GLSL_1xx: return "GLSL 1.x";
	case GLSL_3xx: return "GLSL 3.x";
	case GLSL_VULKAN: return "GLSL-VK";
	case HLSL_D3D9: return "HLSL-D3D9";
	case HLSL_D3D11: return "HLSL-D3D11";
	default: return "(combination)";
	}
}

const char *ShaderStageAsString(ShaderStage stage) {
	switch (stage) {
	case ShaderStage::Fragment: return "Fragment";
	case ShaderStage::Vertex: return "Vertex";
	case ShaderStage::Compute: return "Compute";
	default: return "(unknown)";
	}
}

ShaderLanguageDesc::ShaderLanguageDesc(ShaderLanguage lang) {
	Init(lang);
}

void ShaderLanguageDesc::Init(ShaderLanguage lang) {
	shaderLanguage = lang;
	strcpy(driverInfo, "");
	switch (lang) {
	case GLSL_1xx:
		// Just used in the shader test, and as a basis for the others in DetectShaderLanguage.
		// The real OpenGL initialization happens in thin3d_gl.cpp.
		glslVersionNumber = 110;
		attribute = "attribute";
		varying_vs = "varying";
		varying_fs = "varying";
		fragColor0 = "gl_FragColor";
		fragColor1 = "fragColor1";
		texture = "texture2D";
		texture3D = "texture3D";
		texelFetch = nullptr;
		bitwiseOps = false;
		lastFragData = nullptr;
		gles = false;
		forceMatrix4x4 = true;
		depthMinusOneToOne = true;
		break;
	case GLSL_3xx:
		// Just used in the shader test.
		glslVersionNumber = 300;  // GLSL ES 3.0
		varying_vs = "out";
		varying_fs = "in";
		attribute = "in";
		fragColor0 = "fragColor0";
		fragColor1 = "fragColor1";
		texture = "texture";
		texture3D = "texture";
		texelFetch = "texelFetch";
		bitwiseOps = true;
		lastFragData = nullptr;
		gles = true;
		forceMatrix4x4 = true;
		glslES30 = true;
		depthMinusOneToOne = true;
		break;
	case GLSL_VULKAN:
		fragColor0 = "fragColor0";
		fragColor1 = "fragColor1";
		varying_fs = "in";
		varying_vs = "out";
		attribute = "in";
		bitwiseOps = true;
		framebufferFetchExtension = nullptr;
		gles = false;
		glslES30 = true;
		glslVersionNumber = 450;
		lastFragData = nullptr;
		texture = "texture";
		texture3D = "texture";
		texelFetch = "texelFetch";
		forceMatrix4x4 = false;
		coefsFromBuffers = true;
		vertexIndex = true;
		depthMinusOneToOne = false;
		break;
	case HLSL_D3D9:
	case HLSL_D3D11:
		if (lang == HLSL_D3D11) {
			fragColor0 = "outfragment.target";
			fragColor1 = "outfragment.target1";
			vertexIndex = true;  // if declared as a semantic input
		} else {
			fragColor0 = "outfragment.target";
		}
		varying_fs = "in";
		varying_vs = "out";
		attribute = "in";
		bitwiseOps = lang == HLSL_D3D11;
		framebufferFetchExtension = nullptr;
		gles = false;
		glslES30 = true;  // Hm, D3D9 too?
		glslVersionNumber = 0;
		lastFragData = nullptr;
		texture = "texture";
		texture3D = "texture";
		texelFetch = "texelFetch";
		forceMatrix4x4 = false;
		coefsFromBuffers = true;
		vsOutPrefix = "Out.";
		viewportYSign = "-";
		depthMinusOneToOne = false;
		break;
	}
}
