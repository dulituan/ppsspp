// Copyright (c) 2014- PPSSPP Project.

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

#include "gfx_es2/glsl_program.h"
#include "Core/Reporting.h"
#include "ext/native/gfx/GLStateCache.h"
#include "GPU/GLES/FramebufferManagerGLES.h"
#include "GPU/GLES/ShaderManagerGLES.h"
#include "GPU/GLES/TextureCacheGLES.h"

static const char *gles_prefix =
"#version 100\n"
"precision highp float;\n";

static const char *stencil_fs =
"varying vec2 v_texcoord0;\n"
"uniform float u_stencilValue;\n"
"uniform sampler2D tex;\n"
"float roundAndScaleTo255f(in float x) { return floor(x * 255.99); }\n"
"void main() {\n"
"  vec4 index = texture2D(tex, v_texcoord0);\n"
"  gl_FragColor = vec4(index.a);\n"
"  float shifted = roundAndScaleTo255f(index.a) / roundAndScaleTo255f(u_stencilValue);\n"
"  if (mod(floor(shifted), 2.0) < 0.99) discard;\n"
"}\n";

static const char *stencil_vs =
"attribute vec4 a_position;\n"
"attribute vec2 a_texcoord0;\n"
"varying vec2 v_texcoord0;\n"
"void main() {\n"
"  v_texcoord0 = a_texcoord0;\n"
"  gl_Position = a_position;\n"
"}\n";

std::string GLSLES100PrefixProgram(std::string code) {
	if (gl_extensions.IsGLES) {
		return std::string(gles_prefix) + code;
	} else {
		return code;
	}
}

static u8 StencilBits5551(const u8 *ptr8, u32 numPixels) {
	const u32 *ptr = (const u32 *)ptr8;

	for (u32 i = 0; i < numPixels / 2; ++i) {
		if (ptr[i] & 0x80008000) {
			return 1;
		}
	}

	return 0;
}

static u8 StencilBits4444(const u8 *ptr8, u32 numPixels) {
	const u32 *ptr = (const u32 *)ptr8;
	u32 bits = 0;

	for (u32 i = 0; i < numPixels / 2; ++i) {
		bits |= ptr[i];
	}

	return ((bits >> 12) & 0xF) | (bits >> 28);
}

static u8 StencilBits8888(const u8 *ptr8, u32 numPixels) {
	const u32 *ptr = (const u32 *)ptr8;
	u32 bits = 0;

	for (u32 i = 0; i < numPixels; ++i) {
		bits |= ptr[i];
	}

	return bits >> 24;
}

bool FramebufferManagerGLES::NotifyStencilUpload(u32 addr, int size, bool skipZero) {
	if (!MayIntersectFramebuffer(addr)) {
		return false;
	}

	VirtualFramebuffer *dstBuffer = 0;
	for (size_t i = 0; i < vfbs_.size(); ++i) {
		VirtualFramebuffer *vfb = vfbs_[i];
		if (MaskedEqual(vfb->fb_address, addr)) {
			dstBuffer = vfb;
		}
	}
	if (!dstBuffer) {
		return false;
	}

	int values = 0;
	u8 usedBits = 0;

	const u8 *src = Memory::GetPointer(addr);
	if (!src)
		return false;

	switch (dstBuffer->format) {
	case GE_FORMAT_565:
		// Well, this doesn't make much sense.
		return false;
	case GE_FORMAT_5551:
		usedBits = StencilBits5551(src, dstBuffer->fb_stride * dstBuffer->bufferHeight);
		values = 2;
		break;
	case GE_FORMAT_4444:
		usedBits = StencilBits4444(src, dstBuffer->fb_stride * dstBuffer->bufferHeight);
		values = 16;
		break;
	case GE_FORMAT_8888:
		usedBits = StencilBits8888(src, dstBuffer->fb_stride * dstBuffer->bufferHeight);
		values = 256;
		break;
	case GE_FORMAT_INVALID:
		// Impossible.
		break;
	}

	if (usedBits == 0) {
		if (skipZero) {
			// Common when creating buffers, it's already 0.  We're done.
			return false;
		}

		// Let's not bother with the shader if it's just zero.
		glstate.scissorTest.disable();
		glstate.colorMask.set(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
		glClearColor(0, 0, 0, 0);
		glClearStencil(0);
		glClear(GL_STENCIL_BUFFER_BIT | GL_COLOR_BUFFER_BIT);
		return true;
	}

	if (!stencilUploadProgram_) {
		std::string errorString;
		stencilUploadProgram_ = glsl_create_source(GLSLES100PrefixProgram(stencil_vs).c_str(), GLSLES100PrefixProgram(stencil_fs).c_str(), &errorString);
		if (!stencilUploadProgram_) {
			ERROR_LOG_REPORT(G3D, "Failed to compile stencilUploadProgram! This shouldn't happen.\n%s", errorString.c_str());
		} else {
			glsl_bind(stencilUploadProgram_);
		}

		GLint u_tex = glsl_uniform_loc(stencilUploadProgram_, "tex");
		glUniform1i(u_tex, 0);
	} else {
		glsl_bind(stencilUploadProgram_);
	}

	shaderManagerGL_->DirtyLastShader();

	DisableState();
	glstate.colorMask.set(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
	glstate.stencilTest.enable();
	glstate.stencilOp.set(GL_REPLACE, GL_REPLACE, GL_REPLACE);

	bool useBlit = gstate_c.Supports(GPU_SUPPORTS_ARB_FRAMEBUFFER_BLIT | GPU_SUPPORTS_NV_FRAMEBUFFER_BLIT);

	// Our fragment shader (and discard) is slow.  Since the source is 1x, we can stencil to 1x.
	// Then after we're done, we'll just blit it across and stretch it there.
	if (dstBuffer->bufferWidth == dstBuffer->renderWidth || !dstBuffer->fbo) {
		useBlit = false;
	}
	u16 w = useBlit ? dstBuffer->bufferWidth : dstBuffer->renderWidth;
	u16 h = useBlit ? dstBuffer->bufferHeight : dstBuffer->renderHeight;

	Draw::Framebuffer *blitFBO = nullptr;
	if (useBlit) {
		blitFBO = GetTempFBO(w, h, Draw::FBO_8888);
		draw_->BindFramebufferAsRenderTarget(blitFBO);
	} else if (dstBuffer->fbo) {
		draw_->BindFramebufferAsRenderTarget(dstBuffer->fbo);
	}
	glViewport(0, 0, w, h);

	float u1 = 1.0f;
	float v1 = 1.0f;
	MakePixelTexture(src, dstBuffer->format, dstBuffer->fb_stride, dstBuffer->bufferWidth, dstBuffer->bufferHeight, u1, v1);
	textureCacheGL_->ForgetLastTexture();

	glClearStencil(0);
	glClear(GL_STENCIL_BUFFER_BIT);

	glstate.stencilFunc.set(GL_ALWAYS, 0xFF, 0xFF);

	GLint u_stencilValue = glsl_uniform_loc(stencilUploadProgram_, "u_stencilValue");
	for (int i = 1; i < values; i += i) {
		if (!(usedBits & i)) {
			// It's already zero, let's skip it.
			continue;
		}
		if (dstBuffer->format == GE_FORMAT_4444) {
			glstate.stencilMask.set((i << 4) | i);
			glUniform1f(u_stencilValue, i * (16.0f / 255.0f));
		} else if (dstBuffer->format == GE_FORMAT_5551) {
			glstate.stencilMask.set(0xFF);
			glUniform1f(u_stencilValue, i * (128.0f / 255.0f));
		} else {
			glstate.stencilMask.set(i);
			glUniform1f(u_stencilValue, i * (1.0f / 255.0f));
		}
		DrawActiveTexture(0, 0, dstBuffer->width, dstBuffer->height, dstBuffer->bufferWidth, dstBuffer->bufferHeight, 0.0f, 0.0f, u1, v1, ROTATION_LOCKED_HORIZONTAL, false);
	}
	glstate.stencilMask.set(0xFF);

	if (useBlit) {
		draw_->BlitFramebuffer(blitFBO, 0, 0, w, h, dstBuffer->fbo, 0, 0, dstBuffer->renderWidth, dstBuffer->renderHeight, Draw::FB_STENCIL_BIT, Draw::FB_BLIT_NEAREST);
	}

	RebindFramebuffer();
	return true;
}
