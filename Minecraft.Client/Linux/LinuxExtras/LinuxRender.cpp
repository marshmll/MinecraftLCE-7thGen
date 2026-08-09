// Real OpenGL 3.3 core implementation of C4JRender (see 4J_Render.h). No
// source for the vendor's implementation exists anywhere in the leak - every
// method body here is a from-scratch reimplementation against the interface
// contract only (method names/semantics implied by their names and by how
// Minecraft.Client/glWrapper.cpp + Tesselator.cpp call them).
//
// Scope note: this file makes the *interface* real. It does not by itself
// make world geometry render - that needs Minecraft.Client's game/UI logic
// (Options, Textures, GameRenderer, LevelRenderer, Tesselator's caller chain)
// wired in, which is future work beyond this phase. Linux_Minecraft.cpp's
// test-triangle draw is what actually exercises this file end-to-end today.

// 4J_Render.h declares a vocabulary of plain `const int GL_*` values (e.g.
// GL_BLEND, GL_MODELVIEW, GL_TEXTURE_MIN_FILTER, GL_GREATER, ...) that exist
// purely so glWrapper.cpp/stubs.h can dispatch on them by name - they were
// never meant to be real OpenGL enums (Windows64 never includes real GL
// headers, it's D3D11). Real desktop GL headers define almost every one of
// those same names as preprocessor macros with DIFFERENT numeric values.
// Both are needed in this file (the fake ones to correctly interpret ints
// coming from the shared vocabulary at the C4JRender interface boundary;
// the real ones to actually call OpenGL). They cannot coexist as identical
// bare tokens, so: 4J_Render.h is included FIRST, so its `const int GL_BLEND
// = 2;`-style lines compile as ordinary C++ symbols; then <GL/glew.h> is
// included, whose #define statements macro-shadow those same bare tokens
// for every subsequent use in this file - i.e. every appearance of "GL_*"
// AFTER the glew.h include below resolves to the REAL macro, which is
// exactly what's wanted for actual gl*() calls. The few places that must
// instead compare against the ORIGINAL fake-vocabulary values (because an
// external caller in a non-glew translation unit supplied one) use the
// LinuxRenderFakeGL:: literals below instead of the now-shadowed bare names.
#include <cstdlib>
#include "LinuxTypes.h"
#include "LinuxStubs.h"
#include "../4JLibs/inc/4J_Render.h"

#include <GL/glew.h>
#include <SDL2/SDL.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <mutex>
#include <thread>

#define STB_IMAGE_IMPLEMENTATION
#include "../third_party/stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../third_party/stb/stb_image_write.h"

// Literal mirrors of 4J_Render.h's fake-vocabulary values, for the few spots
// that must compare against what an external (non-glew) caller actually
// passed rather than against the now glew-shadowed bare token. See the
// include-order comment above.
namespace LinuxRenderFakeGL
{
	const int MODELVIEW = 0, PROJECTION = 1, TEXTURE_MATRIX = 2;
	const int TEXTURE_MIN_FILTER = 1, TEXTURE_MAG_FILTER = 2, TEXTURE_WRAP_S = 3, TEXTURE_WRAP_T = 4;
	const int FILTER_NEAREST = 0, FILTER_LINEAR = 1;
	const int WRAP_CLAMP = 0, WRAP_REPEAT = 1;
	const int FOG_LINEAR = 1;
}

namespace
{
	// ---------------------------------------------------------------
	// Minimal 4x4 float matrix helpers (column-major, GL convention:
	// v' = M * v, and "multiply" below means M = M * Rhs, matching
	// glMultMatrixf/glTranslatef/glRotatef/glScalef semantics).
	// ---------------------------------------------------------------
	struct Mat4
	{
		float m[16];
	};

	Mat4 MatIdentity()
	{
		Mat4 r{};
		r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
		return r;
	}

	Mat4 MatMultiply(const Mat4 &a, const Mat4 &b)
	{
		Mat4 r{};
		for (int col = 0; col < 4; col++)
		{
			for (int row = 0; row < 4; row++)
			{
				float sum = 0.0f;
				for (int k = 0; k < 4; k++)
					sum += a.m[k * 4 + row] * b.m[col * 4 + k];
				r.m[col * 4 + row] = sum;
			}
		}
		return r;
	}

	Mat4 MatTranslate(float x, float y, float z)
	{
		Mat4 r = MatIdentity();
		r.m[12] = x; r.m[13] = y; r.m[14] = z;
		return r;
	}

	Mat4 MatScale(float x, float y, float z)
	{
		Mat4 r = MatIdentity();
		r.m[0] = x; r.m[5] = y; r.m[10] = z;
		return r;
	}

	Mat4 MatRotate(float angleRadians, float x, float y, float z)
	{
		float len = std::sqrt(x * x + y * y + z * z);
		if (len < 1e-8f)
			return MatIdentity();
		x /= len; y /= len; z /= len;

		float c = std::cos(angleRadians);
		float s = std::sin(angleRadians);
		float t = 1.0f - c;

		Mat4 r = MatIdentity();
		r.m[0] = t * x * x + c;       r.m[4] = t * x * y - s * z;   r.m[8]  = t * x * z + s * y;
		r.m[1] = t * x * y + s * z;   r.m[5] = t * y * y + c;       r.m[9]  = t * y * z - s * x;
		r.m[2] = t * x * z - s * y;   r.m[6] = t * y * z + s * x;   r.m[10] = t * z * z + c;
		return r;
	}

	Mat4 MatPerspective(float fovyRadians, float aspect, float zNear, float zFar)
	{
		Mat4 r{};
		float f = 1.0f / std::tan(fovyRadians * 0.5f);
		r.m[0] = f / aspect;
		r.m[5] = f;
		r.m[10] = (zFar + zNear) / (zNear - zFar);
		r.m[11] = -1.0f;
		r.m[14] = (2.0f * zFar * zNear) / (zNear - zFar);
		return r;
	}

	Mat4 MatOrtho(float left, float right, float bottom, float top, float zNear, float zFar)
	{
		Mat4 r = MatIdentity();
		r.m[0] = 2.0f / (right - left);
		r.m[5] = 2.0f / (top - bottom);
		r.m[10] = -2.0f / (zFar - zNear);
		r.m[12] = -(right + left) / (right - left);
		r.m[13] = -(top + bottom) / (top - bottom);
		r.m[14] = -(zFar + zNear) / (zFar - zNear);
		return r;
	}

	// ---------------------------------------------------------------
	// GLSL sources. One vertex shader handles all 4 eVertexType cases
	// via the u_vType uniform (lighting math gated to the _LIT case,
	// texgen math gated to the _TEXGEN case, matching the header's own
	// comment that these are the base PF3_TF2_CB4_NB4_XW1 layout "with
	// lighting/texgen applied"). Three fragment shader variants cover
	// ePixelShaderType: STANDARD is plain textured+fog+alpha-test;
	// PROJECTION additionally perspective-divides the (tex-gen'd)
	// texture coordinate before sampling - a real implementation of
	// "projective texturing", which is the only interpretation the
	// available StateSetTexGenCol (S/T/R/Q components, i.e. a full
	// homogeneous coordinate) supports; FORCELOD forces an explicit
	// texture LOD via textureLod() instead of automatic mip selection,
	// matching StateSetForceLOD(int) directly. No HLSL source exists
	// anywhere in the leak to check these against the original.
	// ---------------------------------------------------------------
	const char *kVertexShaderSrc = R"GLSL(
#version 330 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec2 a_tex;
layout(location = 2) in vec4 a_colour;
layout(location = 3) in vec4 a_normal;

uniform mat4 u_modelview;
uniform mat4 u_projection;
uniform mat4 u_texture;
uniform int u_vType; // 0=base, 2=_LIT, 3=_TEXGEN (1=_COMPRESSED is expanded to 0 on the CPU)

uniform bool u_lightingEnabled;
uniform vec3 u_lightAmbient;
uniform bool u_lightEnable[2];
uniform vec3 u_lightDir[2];
uniform vec3 u_lightColour[2];

// The colour set by StateSetColour (i.e. glColor4f), multiplied into the vertex colour.
//
// It cannot be applied as attribute 2's default value: that only takes effect when the
// attribute's array is *disabled*, and this renderer leaves all four arrays permanently
// enabled, so glVertexAttrib4f(2,...) was silently ignored for every tesselated draw.
// A multiply is the right emulation because the two cases compose: Tesselator::end()
// writes white when hasColor is false (so glColor4f wins, which is how grass, foliage and
// water get their biome tint), and callers that do supply per-vertex colours leave
// glColor4f at white (so the vertex colours win). Grass rendering grey in item icons was
// this: a greyscale terrain tile with its tint dropped.
uniform vec4 u_colourMul;

uniform bool u_texGenEnabled;
uniform vec4 u_texGenS;
uniform vec4 u_texGenT;
uniform vec4 u_texGenR;
uniform vec4 u_texGenQ;

out vec2 v_tex;
out vec4 v_colour;
out float v_fogDist;

void main()
{
	vec4 eyePos = u_modelview * vec4(a_pos, 1.0);
	gl_Position = u_projection * eyePos;
	v_fogDist = -eyePos.z;

	vec3 normal = normalize((u_modelview * vec4(a_normal.xyz, 0.0)).xyz);

	// Tesselator packs vertex colour as col = (r<<24)|(g<<16)|(b<<8)|a
	// (Tesselator.cpp:325) and stores that unsigned int straight into the vertex
	// (Tesselator.cpp:929). On a little-endian host the four bytes therefore
	// arrive as a,b,g,r - the packing is big-endian-oriented, left over from the
	// original console targets ("4J - removed little-endian option" on that same
	// line). Reverse it here rather than in the attribute binding, which cannot
	// express a byte-reversed read.
	vec4 colour = a_colour.wzyx * u_colourMul;
	if (u_vType == 2 && u_lightingEnabled)
	{
		vec3 lit = u_lightAmbient;
		for (int i = 0; i < 2; i++)
		{
			if (u_lightEnable[i])
				lit += max(dot(normal, -normalize(u_lightDir[i])), 0.0) * u_lightColour[i];
		}
		colour.rgb *= lit;
	}
	v_colour = colour;

	if (u_vType == 3 && u_texGenEnabled)
	{
		vec4 obj = vec4(a_pos, 1.0);
		v_tex = vec2(dot(obj, u_texGenS), dot(obj, u_texGenT));
	}
	else
	{
		v_tex = (u_texture * vec4(a_tex, 0.0, 1.0)).xy;
	}
}
)GLSL";

	const char *kFragShaderHeader = R"GLSL(
#version 330 core
in vec2 v_tex;
in vec4 v_colour;
in float v_fogDist;
out vec4 o_colour;

uniform sampler2D u_sampler;
uniform bool u_textureEnabled;

uniform bool u_fogEnabled;
uniform int u_fogMode; // 0 = linear, 2 = exp (matches GL_LINEAR/GL_EXP values in 4J_Render.h)
uniform float u_fogNear;
uniform float u_fogFar;
uniform float u_fogDensity;
uniform vec3 u_fogColour;

uniform bool u_alphaTestEnabled;
uniform int u_alphaFunc;
uniform float u_alphaRef;
uniform float u_forceLod;
)GLSL";

	const char *kFragShaderCommonMain_Standard = R"GLSL(
void main()
{
	vec4 colour = v_colour;
	if (u_textureEnabled)
		colour *= texture(u_sampler, v_tex);

	if (u_alphaTestEnabled)
	{
		// GL_GREATER-style compare used by Minecraft's cutout alpha test.
		if (u_alphaFunc == 1 && colour.a <= u_alphaRef) discard;
	}

	if (u_fogEnabled)
	{
		float fogFactor;
		if (u_fogMode == 2)
			fogFactor = exp(-u_fogDensity * v_fogDist);
		else
			fogFactor = (u_fogFar - v_fogDist) / (u_fogFar - u_fogNear);
		fogFactor = clamp(fogFactor, 0.0, 1.0);
		colour.rgb = mix(u_fogColour, colour.rgb, fogFactor);
	}

	o_colour = colour;
}
)GLSL";

	const char *kFragShaderCommonMain_Projection = R"GLSL(
void main()
{
	vec2 tex = v_tex;
	vec4 colour = v_colour;
	if (u_textureEnabled)
		colour *= texture(u_sampler, tex);

	if (u_alphaTestEnabled)
	{
		if (u_alphaFunc == 1 && colour.a <= u_alphaRef) discard;
	}

	if (u_fogEnabled)
	{
		float fogFactor;
		if (u_fogMode == 2)
			fogFactor = exp(-u_fogDensity * v_fogDist);
		else
			fogFactor = (u_fogFar - v_fogDist) / (u_fogFar - u_fogNear);
		fogFactor = clamp(fogFactor, 0.0, 1.0);
		colour.rgb = mix(u_fogColour, colour.rgb, fogFactor);
	}

	o_colour = colour;
}
)GLSL";

	const char *kFragShaderCommonMain_ForceLod = R"GLSL(
void main()
{
	vec4 colour = v_colour;
	if (u_textureEnabled)
		colour *= textureLod(u_sampler, v_tex, u_forceLod);

	if (u_alphaTestEnabled)
	{
		if (u_alphaFunc == 1 && colour.a <= u_alphaRef) discard;
	}

	if (u_fogEnabled)
	{
		float fogFactor;
		if (u_fogMode == 2)
			fogFactor = exp(-u_fogDensity * v_fogDist);
		else
			fogFactor = (u_fogFar - v_fogDist) / (u_fogFar - u_fogNear);
		fogFactor = clamp(fogFactor, 0.0, 1.0);
		colour.rgb = mix(u_fogColour, colour.rgb, fogFactor);
	}

	o_colour = colour;
}
)GLSL";

	GLuint CompileShader(GLenum type, const char *src)
	{
		GLuint shader = glCreateShader(type);
		glShaderSource(shader, 1, &src, nullptr);
		glCompileShader(shader);

		GLint status = GL_FALSE;
		glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
		if (status != GL_TRUE)
		{
			char log[2048];
			GLsizei len = 0;
			glGetShaderInfoLog(shader, sizeof(log), &len, log);
			fprintf(stderr, "LinuxRender: shader compile failed:\n%s\n", log);
		}
		return shader;
	}

	GLuint LinkProgram(GLuint vs, GLuint fs)
	{
		GLuint prog = glCreateProgram();
		glAttachShader(prog, vs);
		glAttachShader(prog, fs);
		glLinkProgram(prog);

		GLint status = GL_FALSE;
		glGetProgramiv(prog, GL_LINK_STATUS, &status);
		if (status != GL_TRUE)
		{
			char log[2048];
			GLsizei len = 0;
			glGetProgramInfoLog(prog, sizeof(log), &len, log);
			fprintf(stderr, "LinuxRender: program link failed:\n%s\n", log);
		}
		return prog;
	}

	struct UniformSet
	{
		GLint modelview, projection, texture, vType;
		GLint lightingEnabled, lightAmbient, lightEnable, lightDir, lightColour;
		GLint colourMul;
		GLint texGenEnabled, texGenS, texGenT, texGenR, texGenQ;
		GLint sampler, textureEnabled;
		GLint fogEnabled, fogMode, fogNear, fogFar, fogDensity, fogColour;
		GLint alphaTestEnabled, alphaFunc, alphaRef, forceLod;
	};

	void FetchUniforms(GLuint prog, UniformSet &u)
	{
		u.modelview = glGetUniformLocation(prog, "u_modelview");
		u.projection = glGetUniformLocation(prog, "u_projection");
		u.texture = glGetUniformLocation(prog, "u_texture");
		u.vType = glGetUniformLocation(prog, "u_vType");
		u.lightingEnabled = glGetUniformLocation(prog, "u_lightingEnabled");
		u.lightAmbient = glGetUniformLocation(prog, "u_lightAmbient");
		u.lightEnable = glGetUniformLocation(prog, "u_lightEnable");
		u.lightDir = glGetUniformLocation(prog, "u_lightDir");
		u.lightColour = glGetUniformLocation(prog, "u_lightColour");
		u.colourMul = glGetUniformLocation(prog, "u_colourMul");
		u.texGenEnabled = glGetUniformLocation(prog, "u_texGenEnabled");
		u.texGenS = glGetUniformLocation(prog, "u_texGenS");
		u.texGenT = glGetUniformLocation(prog, "u_texGenT");
		u.texGenR = glGetUniformLocation(prog, "u_texGenR");
		u.texGenQ = glGetUniformLocation(prog, "u_texGenQ");
		u.sampler = glGetUniformLocation(prog, "u_sampler");
		u.textureEnabled = glGetUniformLocation(prog, "u_textureEnabled");
		u.fogEnabled = glGetUniformLocation(prog, "u_fogEnabled");
		u.fogMode = glGetUniformLocation(prog, "u_fogMode");
		u.fogNear = glGetUniformLocation(prog, "u_fogNear");
		u.fogFar = glGetUniformLocation(prog, "u_fogFar");
		u.fogDensity = glGetUniformLocation(prog, "u_fogDensity");
		u.fogColour = glGetUniformLocation(prog, "u_fogColour");
		u.alphaTestEnabled = glGetUniformLocation(prog, "u_alphaTestEnabled");
		u.alphaFunc = glGetUniformLocation(prog, "u_alphaFunc");
		u.alphaRef = glGetUniformLocation(prog, "u_alphaRef");
		u.forceLod = glGetUniformLocation(prog, "u_forceLod");
	}

	// One recorded command inside a CBuffStart/CBuffEnd pair.
	//
	// A command buffer is the Linux stand-in for a GL display list (glWrapper.cpp
	// maps glNewList/glEndList/glCallList straight onto CBuffStart/CBuffEnd/
	// CBuffCall), so it has to capture *every* command issued between the two,
	// not just the geometry. Recording only draws is not a tolerable
	// simplification: the real recording sites all put state inside the list,
	// and one of them is load-bearing for the whole world rendering correctly -
	//
	//   Chunk.cpp:386-450   glPushMatrix / glDepthMask(true) / translateToPos()
	//                       / ... / glPopMatrix
	//   LevelRenderer.cpp:172  glDepthMask(false), with the 4J comment
	//                       "added to get depth mask disabled within the
	//                        command buffer"
	//   ModelPart.cpp:290   glEnable(GL_DEPTH_TEST) / glDepthFunc(GL_LEQUAL)
	//                       / glDepthMask(true)
	//   ItemInHandRenderer.cpp:101  glDepthFunc(GL_EQUAL)
	//
	// Chunk::rebuild() tesselates *chunk-local* vertices (t->offset(-x,-y,-z))
	// and supplies the chunk's world position only via that translateToPos()
	// inside the list; LevelRenderer::renderChunks issues no matrix ops around
	// CBuffCall. Dropping the recorded translate therefore draws every chunk in
	// the world stacked inside a single 16x16x16 box.
	//
	// Recording (rather than executing) is also the correct GL semantic:
	// glNewList(..., GL_COMPILE) compiles commands without executing them, so a
	// call that arrives while recording must not touch live state at all. That
	// property is what makes concurrent recording from the chunk-rebuild threads
	// safe - see the threading comment on g_cbuffMutex below.
	struct RecordedCmd
	{
		enum Op
		{
			OP_DRAW,
			OP_MATRIX_MODE, OP_MATRIX_PUSH, OP_MATRIX_POP, OP_MATRIX_IDENTITY,
			OP_MATRIX_TRANSLATE, OP_MATRIX_ROTATE, OP_MATRIX_SCALE, OP_MATRIX_MULT,
			OP_MATRIX_PERSPECTIVE, OP_MATRIX_ORTHO,
			OP_DEPTH_MASK, OP_DEPTH_FUNC, OP_DEPTH_TEST_ENABLE, OP_DEPTH_SLOPE_BIAS,
			OP_ALPHA_TEST_ENABLE, OP_ALPHA_FUNC,
			OP_BLEND_ENABLE, OP_BLEND_FUNC, OP_BLEND_FACTOR,
			OP_FACE_CULL, OP_FACE_CULL_CW, OP_LINE_WIDTH, OP_WRITE_ENABLE,
			OP_COLOUR, OP_TEXTURE_BIND, OP_TEXTURE_BIND_VERTEX,
			OP_VERTEX_TEXTURE_UV, OP_FORCE_LOD,
			OP_LIGHTING_ENABLE, OP_LIGHT_ENABLE, OP_LIGHT_COLOUR,
			OP_LIGHT_AMBIENT, OP_LIGHT_DIR,
			OP_FOG_ENABLE, OP_FOG_MODE, OP_FOG_NEAR, OP_FOG_FAR,
			OP_FOG_DENSITY, OP_FOG_COLOUR, OP_TEXGEN_COL
		};

		int op = OP_DRAW;
		float f[16] = {0};                 // float args (16 wide for OP_MATRIX_MULT)
		int i[4] = {0};                    // int/enum/bool args
		std::vector<unsigned char> data;   // vertex payload, OP_DRAW only
	};

	struct CommandBuffer
	{
		bool alive = false;
		std::vector<RecordedCmd> cmds;
		// Running total of cmds' vertex payload bytes, so CBuffSize() stays O(1).
		// It is polled up to 10x per update tick (LevelRenderer.cpp:1807 via
		// CBuffSize(-1)) and summing on demand under the store lock would stall
		// the render thread proportionally to how much world is loaded.
		size_t bytes = 0;
	};

	int VertexStride(C4JRender::eVertexType vType)
	{
		return (vType == C4JRender::VERTEX_TYPE_COMPRESSED) ? 16 : 32;
	}

	// Expand VERTEX_TYPE_COMPRESSED into VERTEX_TYPE_PF3_TF2_CB4_NB4_XW1.
	//
	// This is the format ALL chunk terrain uses: Chunk::rebuild() calls
	// t->useCompactVertices(true) (Chunk.cpp:390) before tesselating, which makes
	// Tesselator::end() submit VERTEX_TYPE_COMPRESSED (Tesselator.cpp:174/182).
	// It is not an Xbox 360-only path despite useCompactFormat360's name.
	//
	// The layout has no header or shader source in the leak, but the write side
	// fully determines it - Tesselator::vertex(), generic branch
	// (Tesselator.cpp:824-841). 16 bytes = 8 signed shorts:
	//   [0..2]  position * 1024                (chunk-local, so it fits a short)
	//   [3]     RGB565 colour, biased by -32768 so it round-trips a signed short
	//   [4]     u * 8192   (u > 1.0 means "mipmapping off for this vertex")
	//   [5]     v * 8192
	//   [6..7]  secondary/lightmap UVs (_tex2)
	//
	// Expanding here rather than adding a second vertex format to the shader
	// keeps one draw path, and for recorded command buffers it happens once at
	// record time on the chunk-rebuild thread instead of every frame on replay.
	void ExpandCompressedVertices(const void *src, int count, std::vector<unsigned char> &out)
	{
		const int16_t *in = (const int16_t *)src;
		out.assign((size_t)count * 32, 0);

		for (int i = 0; i < count; i++)
		{
			const int16_t *s = in + (size_t)i * 8;
			unsigned char *d = out.data() + (size_t)i * 32;

			float *pos = (float *)d;
			pos[0] = s[0] / 1024.0f;
			pos[1] = s[1] / 1024.0f;
			pos[2] = s[2] / 1024.0f;
			pos[3] = s[4] / 8192.0f;   // u, at byte offset 12
			pos[4] = s[5] / 8192.0f;   // v, at byte offset 16

			// Undo the -32768 bias, then unpack RGB565. Alpha is not carried by
			// this format (Tesselator drops col's low byte when packing), so the
			// expanded vertex is opaque.
			unsigned int packed = (unsigned int)(uint16_t)(s[3] + (int16_t)0x8000u);
			unsigned int r5 = (packed >> 11) & 0x1F;
			unsigned int g6 = (packed >> 5) & 0x3F;
			unsigned int b5 = packed & 0x1F;

			// Written as Tesselator packs colour for the 32-byte format -
			// col = (r << 24) | (g << 16) | (b << 8) | a (Tesselator.cpp:325) -
			// so both formats reach the shader through the identical attribute.
			unsigned int col = ((r5 * 255u / 31u) << 24) |
			                   ((g6 * 255u / 63u) << 16) |
			                   ((b5 * 255u / 31u) << 8) |
			                   0xFFu;
			memcpy(d + 20, &col, 4);

			// Normal (offset 24) and the trailing padding/tex2 DWORD (offset 28)
			// are left zeroed: this format carries no normal, the expanded type
			// is the non-_LIT one so the shader never reads it, and the secondary
			// UVs at s[6..7] have no consumer in this renderer (the 32-byte path's
			// equivalent DWORD is not bound as an attribute either).
		}
	}

	// PRIMITIVE_TYPE_QUAD_LIST is deliberately absent: core GL has no quad
	// primitive, so it never maps to a single GLenum. ApplyStateAndDraw()
	// handles it separately with an index buffer.
	GLenum PrimitiveToGL(C4JRender::ePrimitiveType t)
	{
		switch (t)
		{
		case C4JRender::PRIMITIVE_TYPE_TRIANGLE_LIST:  return GL_TRIANGLES;
		case C4JRender::PRIMITIVE_TYPE_TRIANGLE_STRIP: return GL_TRIANGLE_STRIP;
		case C4JRender::PRIMITIVE_TYPE_TRIANGLE_FAN:   return GL_TRIANGLE_FAN;
		case C4JRender::PRIMITIVE_TYPE_LINE_LIST:      return GL_LINES;
		case C4JRender::PRIMITIVE_TYPE_LINE_STRIP:     return GL_LINE_STRIP;
		default: return GL_TRIANGLES;
		}
	}
}

// ---------------------------------------------------------------------
// C4JRender state (kept file-static rather than as class members so the
// header doesn't need to expose GL types to Minecraft.World's stdafx.h).
// ---------------------------------------------------------------------
namespace
{
	SDL_Window *g_window = nullptr;

	std::vector<Mat4> g_matStack[3];
	int g_matMode = LinuxRenderFakeGL::MODELVIEW;

	GLuint g_programs[C4JRender::PIXEL_SHADER_COUNT] = {0, 0, 0};
	UniformSet g_uniforms[C4JRender::PIXEL_SHADER_COUNT];

	GLuint g_vao = 0, g_vbo = 0;

	// Shared quad->triangle index buffer, grown on demand. See the
	// PRIMITIVE_TYPE_QUAD_LIST branch in ApplyStateAndDraw().
	GLuint g_quadIbo = 0;
	int g_quadIboQuads = 0;

	float g_clearColour[4] = {0, 0, 0, 1};

	bool g_blendEnabled = false;
	bool g_depthTestEnabled = false;
	bool g_alphaTestEnabled = false;
	bool g_fogEnabled = false;
	bool g_lightingEnabled = false;
	bool g_cullEnabled = false;
	bool g_lightEnable[2] = {false, false};

	int g_fogMode = LinuxRenderFakeGL::FOG_LINEAR;
	float g_fogNear = 0.0f, g_fogFar = 1.0f, g_fogDensity = 1.0f;
	float g_fogColour[3] = {0, 0, 0};
	int g_alphaFunc = D3D11_COMPARISON_GREATER;
	float g_alphaRef = 0.0f;
	int g_forceLod = 0;

	float g_colour[4] = {1, 1, 1, 1};
	float g_lightAmbient[3] = {1, 1, 1};
	float g_lightDir[2][3] = {{0, -1, 0}, {0, -1, 0}};
	float g_lightColour[2][3] = {{1, 1, 1}, {1, 1, 1}};
	float g_texGen[4][4] = {{0,0,0,0},{0,0,0,0},{0,0,0,0},{0,0,0,0}};
	bool g_texGenEnabled = false;

	int g_boundTexture = -1;
	// The texture that upload/parameter operations act on: whichever slot was
	// bound most recently, fragment or vertex. GameRenderer::turnOnLightLayer()
	// (GameRenderer.cpp:841-845) relies on this - it binds the light texture with
	// TextureBindVertex() and *then* sets MIN/MAG LINEAR and WRAP CLAMP, intending
	// them for that texture. Aiming those at g_boundTexture instead applied them
	// to the terrain atlas every frame, which is why block textures came out
	// blurry and bilinear-filtered instead of pixelated.
	int g_lastBoundTexture = -1;
	// Separate slot for TextureBindVertex() - the light texture. Tracked rather
	// than sampled: the shaders here don't implement lightmap modulation yet (the
	// secondary UVs it needs are dropped in ExpandCompressedVertices), so per-block
	// light comes only from the baked vertex colours. Keeping it out of
	// g_boundTexture is what matters - see TextureBindVertex().
	int g_boundTextureVertex = -1;
	int g_textureLevels = 1;

	struct TextureRec { GLuint id = 0; int width = 0, height = 0; };
	std::unordered_map<int, TextureRec> g_textures;
	int g_nextTextureId = 1;

	// ------------------------------------------------------------------
	// Command-buffer store and recording state.
	//
	// THREADING: the engine records command buffers from FOUR threads
	// concurrently while the main thread replays them:
	//   - LevelRenderer.cpp:3612 rebuildChunkThreadProc, x3
	//     (MAX_CHUNK_REBUILD_THREADS, LevelRenderer.h:265-266)
	//   - GameRenderer.cpp:1138 runUpdate ("Chunk update"), which also calls
	//     permaChunk[index].rebuild() itself (LevelRenderer.cpp:2068)
	//   - the main thread, replaying via CBuffCall (LevelRenderer.cpp:793)
	// Each of those worker threads calls RenderManager.InitialiseContext()
	// on entry (LevelRenderer.cpp:3617, GameRenderer.cpp:1145) precisely
	// because the vendor renderer gives every recording thread its own
	// context.
	//
	// So the recording cursor is per-thread, and the shared store is only ever
	// touched under g_cbuffMutex. Recording appends to a thread-local buffer
	// with no locking at all (that is the hot path - thousands of draws per
	// chunk), and CBuffEnd() commits it into the store in one locked move.
	// Committing whole rather than clearing-then-filling in place also means a
	// chunk being rebuilt is never momentarily empty while the main thread
	// renders it, which would show as a hole in the world.
	std::mutex g_cbuffMutex;
	std::unordered_map<int, CommandBuffer> g_cbuffs;   // guarded by g_cbuffMutex
	int g_nextCBuffId = 1;                             // guarded by g_cbuffMutex
	size_t g_cbuffTotalBytes = 0;                      // guarded by g_cbuffMutex

	thread_local bool t_recording = false;
	thread_local int t_recordingIndex = -1;
	thread_local std::vector<RecordedCmd> t_recordBuf;

	// The one thread that owns the GL context (set in Initialise()). Only used
	// to catch draws that arrive on a worker thread without recording active,
	// which would otherwise issue GL calls with no current context.
	std::thread::id g_glThreadId;
	bool g_glThreadIdValid = false;

	bool g_suspended = false;

	Mat4 &Top(int mode) { return g_matStack[mode].back(); }

	inline bool Recording() { return t_recording; }

	// Append a command to this thread's recording. Args are copied by value into
	// the fixed-size arrays; `fn`/`in` are how many of each are meaningful.
	void Rec(int op, const float *f, int fn, const int *iv, int in)
	{
		t_recordBuf.emplace_back();
		RecordedCmd &c = t_recordBuf.back();
		c.op = op;
		for (int k = 0; k < fn && k < 16; k++) c.f[k] = f[k];
		for (int k = 0; k < in && k < 4; k++) c.i[k] = iv[k];
	}

	inline void Rec(int op) { Rec(op, nullptr, 0, nullptr, 0); }
	inline void RecI(int op, int a) { int iv[1] = {a}; Rec(op, nullptr, 0, iv, 1); }
	inline void RecI(int op, int a, int b) { int iv[2] = {a, b}; Rec(op, nullptr, 0, iv, 2); }
	inline void RecB(int op, bool a) { RecI(op, a ? 1 : 0); }
	inline void RecF(int op, float a) { float f[1] = {a}; Rec(op, f, 1, nullptr, 0); }
	inline void RecF(int op, float a, float b) { float f[2] = {a, b}; Rec(op, f, 2, nullptr, 0); }
	inline void RecF(int op, float a, float b, float c) { float f[3] = {a, b, c}; Rec(op, f, 3, nullptr, 0); }
	inline void RecF(int op, float a, float b, float c, float d)
	{
		float f[4] = {a, b, c, d};
		Rec(op, f, 4, nullptr, 0);
	}

	// For the handful of entry points that genuinely cannot be replayed as a
	// recorded command (whole-framebuffer/viewport scope rather than per-draw
	// state). Silently ignoring these is the exact failure mode that made this
	// file drop Chunk::rebuild's translate, so make it loud instead.
	void WarnUnrecordable(const char *name)
	{
		static bool warned[8] = {false};
		static const char *seen[8] = {nullptr};
		for (int k = 0; k < 8; k++)
		{
			if (seen[k] && strcmp(seen[k], name) == 0) return;
			if (!seen[k])
			{
				seen[k] = name;
				warned[k] = true;
				fprintf(stderr, "LinuxRender: %s() called inside a command buffer but is not "
				                "recordable - it will apply immediately instead of on replay.\n", name);
				return;
			}
		}
	}
}

void C4JRender::Set_matrixDirty()
{
	// Pure perf hint on the vendor implementation (lazily recompute a
	// cached MVP). LinuxRender recomputes the MVP uniforms fresh on every
	// DrawVertices call instead, so there is nothing to mark dirty.
}

// Every matrix entry point below records instead of mutating while a command
// buffer is open. That is both the correct GL_COMPILE semantic and what keeps
// the shared g_matStack/g_matMode single-threaded: the only callers that touch
// matrices off the GL thread are the chunk-rebuild threads, and they only ever
// do so between glNewList and glEndList (Chunk.cpp:386-450).

void C4JRender::MatrixMode(int type)
{
	if (Recording()) { RecI(RecordedCmd::OP_MATRIX_MODE, type); return; }
	g_matMode = type;
}

void C4JRender::MatrixSetIdentity()
{
	if (Recording()) { Rec(RecordedCmd::OP_MATRIX_IDENTITY); return; }
	Top(g_matMode) = MatIdentity();
}

void C4JRender::MatrixTranslate(float x, float y, float z)
{
	if (Recording()) { RecF(RecordedCmd::OP_MATRIX_TRANSLATE, x, y, z); return; }
	Top(g_matMode) = MatMultiply(Top(g_matMode), MatTranslate(x, y, z));
}

void C4JRender::MatrixRotate(float angle, float x, float y, float z)
{
	if (Recording()) { RecF(RecordedCmd::OP_MATRIX_ROTATE, angle, x, y, z); return; }
	Top(g_matMode) = MatMultiply(Top(g_matMode), MatRotate(angle, x, y, z));
}

void C4JRender::MatrixScale(float x, float y, float z)
{
	if (Recording()) { RecF(RecordedCmd::OP_MATRIX_SCALE, x, y, z); return; }
	Top(g_matMode) = MatMultiply(Top(g_matMode), MatScale(x, y, z));
}

void C4JRender::MatrixPerspective(float fovyDegrees, float aspect, float zNear, float zFar)
{
	if (Recording()) { RecF(RecordedCmd::OP_MATRIX_PERSPECTIVE, fovyDegrees, aspect, zNear, zFar); return; }

	// fovy is in DEGREES here. This method is what glWrapper.cpp:51 maps
	// gluPerspective() onto, and gluPerspective is defined in degrees; GameRenderer
	// supplies degrees throughout (m_fov = 70.0f, GameRenderer.cpp:124).
	//
	// Passing them into MatPerspective's tan(fovy/2) as if they were radians made the
	// projection silently wrong, and non-linearly so. At fov 70, tan(35 rad) happens to
	// be +0.476, giving a plausible-looking (but incorrect) ~51 degree view. Underwater
	// getFov() scales it to fov * 60/70 = 60 (GameRenderer.cpp:439), and tan(30 rad) is
	// -6.40 - just past a tan pole - so the projection inverted and collapsed toward the
	// screen centre. That was the "viewport glitches underwater" report, and also why
	// ItemInHandRenderer's full-screen overlay quad appeared as a small centred square.
	const float kDegToRad = 3.14159265358979323846f / 180.0f;
	Top(g_matMode) = MatMultiply(Top(g_matMode),
	                             MatPerspective(fovyDegrees * kDegToRad, aspect, zNear, zFar));
}

void C4JRender::MatrixOrthogonal(float left, float right, float bottom, float top, float zNear, float zFar)
{
	if (Recording())
	{
		float f[6] = {left, right, bottom, top, zNear, zFar};
		Rec(RecordedCmd::OP_MATRIX_ORTHO, f, 6, nullptr, 0);
		return;
	}
	Top(g_matMode) = MatMultiply(Top(g_matMode), MatOrtho(left, right, bottom, top, zNear, zFar));
}

void C4JRender::MatrixPop()
{
	if (Recording()) { Rec(RecordedCmd::OP_MATRIX_POP); return; }
	if (g_matStack[g_matMode].size() > 1)
		g_matStack[g_matMode].pop_back();
}

void C4JRender::MatrixPush()
{
	if (Recording()) { Rec(RecordedCmd::OP_MATRIX_PUSH); return; }
	g_matStack[g_matMode].push_back(Top(g_matMode));
}

void C4JRender::MatrixMult(float *mat)
{
	if (Recording()) { Rec(RecordedCmd::OP_MATRIX_MULT, mat, 16, nullptr, 0); return; }
	Mat4 rhs;
	memcpy(rhs.m, mat, sizeof(rhs.m));
	Top(g_matMode) = MatMultiply(Top(g_matMode), rhs);
}

const float *C4JRender::MatrixGet(int type)
{
	// A read, so there is nothing to record - but reading the live matrix from a
	// recording thread would return the GL thread's camera, not anything this
	// list can rely on. No such caller exists (the tesselator path never reads
	// matrices); warn rather than return something misleading if one appears.
	if (Recording()) WarnUnrecordable("MatrixGet");
	return g_matStack[type].back().m;
}

void C4JRender::Tick()
{
}

void C4JRender::UpdateGamma(unsigned short usGamma)
{
	// No gamma-ramp equivalent wired up on Linux (SDL2 doesn't expose one
	// portably across Wayland/X11); no-op.
}

void C4JRender::Initialise(void *pWindowHandle)
{
	g_window = static_cast<SDL_Window *>(pWindowHandle);

	// The thread that runs Initialise() is the one CLinuxApp made the GL context
	// current on, and the only one allowed to issue real GL calls.
	g_glThreadId = std::this_thread::get_id();
	g_glThreadIdValid = true;

	glewExperimental = GL_TRUE;
	GLenum glewStatus = glewInit();
	if (glewStatus != GLEW_OK)
	{
		fprintf(stderr, "LinuxRender: glewInit failed: %s\n", glewGetErrorString(glewStatus));
	}
	// GLEW's core-profile probing can leave a spurious GL_INVALID_ENUM in
	// the error queue on some drivers; clear it so later real errors aren't
	// masked.
	glGetError();

	for (int i = 0; i < 3; i++)
		g_matStack[i].push_back(MatIdentity());

	GLuint vs = CompileShader(GL_VERTEX_SHADER, kVertexShaderSrc);

	std::string standardSrc = std::string(kFragShaderHeader) + kFragShaderCommonMain_Standard;
	std::string projectionSrc = std::string(kFragShaderHeader) + kFragShaderCommonMain_Projection;
	std::string forceLodSrc = std::string(kFragShaderHeader) + kFragShaderCommonMain_ForceLod;

	GLuint fsStandard = CompileShader(GL_FRAGMENT_SHADER, standardSrc.c_str());
	GLuint fsProjection = CompileShader(GL_FRAGMENT_SHADER, projectionSrc.c_str());
	GLuint fsForceLod = CompileShader(GL_FRAGMENT_SHADER, forceLodSrc.c_str());

	g_programs[C4JRender::PIXEL_SHADER_TYPE_STANDARD] = LinkProgram(vs, fsStandard);
	g_programs[C4JRender::PIXEL_SHADER_TYPE_PROJECTION] = LinkProgram(vs, fsProjection);
	g_programs[C4JRender::PIXEL_SHADER_TYPE_FORCELOD] = LinkProgram(vs, fsForceLod);

	for (int i = 0; i < C4JRender::PIXEL_SHADER_COUNT; i++)
		FetchUniforms(g_programs[i], g_uniforms[i]);

	glDeleteShader(vs);
	glDeleteShader(fsStandard);
	glDeleteShader(fsProjection);
	glDeleteShader(fsForceLod);

	glGenVertexArrays(1, &g_vao);
	glGenBuffers(1, &g_vbo);
	glBindVertexArray(g_vao);
	glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 32, (void *)0);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 32, (void *)12);
	glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, 32, (void *)20);
	glVertexAttribPointer(3, 4, GL_BYTE, GL_TRUE, 32, (void *)24);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glEnableVertexAttribArray(2);
	glEnableVertexAttribArray(3);
	glBindVertexArray(0);

	glClearColor(g_clearColour[0], g_clearColour[1], g_clearColour[2], g_clearColour[3]);
}

void C4JRender::InitialiseContext()
{
	// Called once per thread that is going to record command buffers, from that
	// thread: LevelRenderer.cpp:3617 (each of the 3 chunk-rebuild threads) and
	// GameRenderer.cpp:1145 (the "Chunk update" thread). It is NOT a main-thread
	// call, so it must not assume a current GL context - those worker threads
	// deliberately never get one. They only ever record (see RecordedCmd), and
	// recording touches no GL and no shared state, so all this needs to do is
	// prepare this thread's recording buffer.
	t_recording = false;
	t_recordingIndex = -1;
	t_recordBuf.clear();
	t_recordBuf.reserve(4096);
}

void C4JRender::StartFrame()
{
	int w = 0, h = 0;
	if (g_window)
		SDL_GL_GetDrawableSize(g_window, &w, &h);
	if (w > 0 && h > 0)
		glViewport(0, 0, w, h);
}

void C4JRender::DoScreenGrabOnNextPresent()
{
	// CaptureScreen()/CaptureThumbnail() below grab immediately instead of
	// deferring to the next Present(); no separate flag needed.
}

void C4JRender::Present()
{
	if (g_window)
		SDL_GL_SwapWindow(g_window);
}

void C4JRender::Clear(int flags, D3D11_RECT *pRect)
{
	GLbitfield mask = 0;
	if (flags & CLEAR_COLOUR_FLAG) mask |= GL_COLOR_BUFFER_BIT;
	if (flags & CLEAR_DEPTH_FLAG) mask |= GL_DEPTH_BUFFER_BIT;
	if (!mask) return;

	// glClear obeys the depth write mask, so a depth clear silently does nothing while
	// writes are masked off - which is the state GDraw leaves behind after most of its
	// draws. The vendor's own gdraw_ClearID() forces the mask for exactly this reason.
	GLboolean depthMaskWas = GL_TRUE;
	if (mask & GL_DEPTH_BUFFER_BIT)
	{
		glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMaskWas);
		glDepthMask(GL_TRUE);
	}

	// pRect is a partial-clear rect and it does have a live caller:
	// UIController::endCustomDrawGameState() passes &m_customRenderingClearRect to clear
	// depth for just the region a custom draw touched. Ignoring it wiped the *whole* depth
	// buffer part-way through the frame, which destroys the object ids Iggy encodes there
	// (depth_from_id + test_id/set_id) - so every Iggy draw issued after a custom-draw
	// region lost its depth ordering and flickered. Honour it with a scissored clear, as
	// the D3D11 and console backends do.
	if (pRect)
	{
		// The rect is in top-left window coordinates; GL's origin is bottom-left.
		int w = 0, h = 0;
		if (g_window) SDL_GL_GetDrawableSize(g_window, &w, &h);

		GLint rx = (GLint)pRect->left;
		GLint rw = (GLint)(pRect->right - pRect->left);
		GLint rh = (GLint)(pRect->bottom - pRect->top);
		GLint ry = (GLint)(h - pRect->bottom);

		// setupCustomDrawGameState seeds the rect inverted (LONG_MAX/LONG_MIN) and only
		// widens it per region, so an empty or inverted rect means "nothing was drawn" -
		// clear nothing rather than falling back to the whole buffer.
		if (rw > 0 && rh > 0)
		{
			GLboolean scissorWas = glIsEnabled(GL_SCISSOR_TEST);
			GLint oldBox[4] = {0, 0, 0, 0};
			glGetIntegerv(GL_SCISSOR_BOX, oldBox);

			glEnable(GL_SCISSOR_TEST);
			glScissor(rx, ry, rw, rh);
			glClear(mask);

			glScissor(oldBox[0], oldBox[1], oldBox[2], oldBox[3]);
			if (!scissorWas) glDisable(GL_SCISSOR_TEST);
		}
	}
	else
	{
		glClear(mask);
	}

	if ((mask & GL_DEPTH_BUFFER_BIT) && !depthMaskWas)
		glDepthMask(GL_FALSE);
}

void C4JRender::SetClearColour(const float colourRGBA[4])
{
	memcpy(g_clearColour, colourRGBA, sizeof(g_clearColour));
	glClearColor(g_clearColour[0], g_clearColour[1], g_clearColour[2], g_clearColour[3]);
}

bool C4JRender::IsWidescreen()
{
	int w = 1, h = 1;
	if (g_window)
		SDL_GL_GetDrawableSize(g_window, &w, &h);
	return h > 0 && (float)w / (float)h > 1.4f;
}

bool C4JRender::IsHiDef()
{
	int w = 0, h = 0;
	if (g_window)
		SDL_GL_GetDrawableSize(g_window, &w, &h);
	return h >= 720;
}

void C4JRender::CaptureThumbnail(ImageFileBuffer *pngOut)
{
	if (!pngOut) return;
	pngOut->m_pBuffer = nullptr;
	pngOut->m_bufferSize = 0;
}

void C4JRender::CaptureScreen(ImageFileBuffer *jpgOut, XSOCIAL_PREVIEWIMAGE *previewOut)
{
	if (jpgOut)
	{
		jpgOut->m_pBuffer = nullptr;
		jpgOut->m_bufferSize = 0;
	}
	// Console social/preview-image screenshot upload path; no Linux
	// equivalent target to upload to, left unimplemented.
}

void C4JRender::BeginConditionalSurvey(int identifier) {}
void C4JRender::EndConditionalSurvey() {}
void C4JRender::BeginConditionalRendering(int identifier) {}
void C4JRender::EndConditionalRendering() {}

namespace
{
	// Grow g_quadIbo so it indexes at least `quads` quads as 0,1,2, 0,2,3.
	// Content is position-independent, so it is built once and reused by every
	// quad draw; only a batch larger than anything seen so far rebuilds it.
	void EnsureQuadIndexCapacity(int quads)
	{
		if (g_quadIbo != 0 && quads <= g_quadIboQuads) return;

		int newQuads = g_quadIboQuads > 0 ? g_quadIboQuads : 1024;
		while (newQuads < quads) newQuads *= 2;

		std::vector<GLuint> indices((size_t)newQuads * 6);
		for (int q = 0; q < newQuads; q++)
		{
			GLuint base = (GLuint)q * 4;
			GLuint *out = &indices[(size_t)q * 6];
			out[0] = base + 0; out[1] = base + 1; out[2] = base + 2;
			out[3] = base + 0; out[4] = base + 2; out[5] = base + 3;
		}

		if (g_quadIbo == 0) glGenBuffers(1, &g_quadIbo);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_quadIbo);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)indices.size() * sizeof(GLuint),
		             indices.data(), GL_STATIC_DRAW);
		g_quadIboQuads = newQuads;
	}

	void ApplyStateAndDraw(C4JRender::ePrimitiveType primType, int count, const void *dataIn,
	                        C4JRender::eVertexType vType, C4JRender::ePixelShaderType psType)
	{
		// DrawVertices() expands VERTEX_TYPE_COMPRESSED before it gets here, so
		// this is unreachable; keep it as a guard because drawing 16-byte
		// vertices through the 32-byte attribute layout would read past the
		// buffer rather than fail visibly.
		if (vType == C4JRender::VERTEX_TYPE_COMPRESSED)
		{
			static bool warned = false;
			if (!warned)
			{
				fprintf(stderr, "LinuxRender: VERTEX_TYPE_COMPRESSED reached ApplyStateAndDraw - it "
				                "should have been expanded in DrawVertices(). Draw skipped.\n");
				warned = true;
			}
			return;
		}

		GLuint prog = g_programs[psType];
		glUseProgram(prog);
		const UniformSet &u = g_uniforms[psType];

		glUniformMatrix4fv(u.modelview, 1, GL_FALSE, Top(LinuxRenderFakeGL::MODELVIEW).m);
		glUniformMatrix4fv(u.projection, 1, GL_FALSE, Top(LinuxRenderFakeGL::PROJECTION).m);
		glUniformMatrix4fv(u.texture, 1, GL_FALSE, Top(LinuxRenderFakeGL::TEXTURE_MATRIX).m);
		glUniform1i(u.vType, (int)vType);

		glUniform1i(u.lightingEnabled, g_lightingEnabled);
		glUniform3fv(u.lightAmbient, 1, g_lightAmbient);
		int lightEnableInt[2] = {(int)g_lightEnable[0], (int)g_lightEnable[1]};
		glUniform1iv(u.lightEnable, 2, lightEnableInt);
		glUniform3fv(u.lightDir, 2, &g_lightDir[0][0]);
		glUniform3fv(u.lightColour, 2, &g_lightColour[0][0]);

		// glColor4f's value. See u_colourMul's comment in the vertex shader for why this
		// has to be a uniform multiply rather than attribute 2's default value.
		glUniform4fv(u.colourMul, 1, g_colour);

		glUniform1i(u.texGenEnabled, g_texGenEnabled);
		glUniform4fv(u.texGenS, 1, g_texGen[0]);
		glUniform4fv(u.texGenT, 1, g_texGen[1]);
		glUniform4fv(u.texGenR, 1, g_texGen[2]);
		glUniform4fv(u.texGenQ, 1, g_texGen[3]);

		bool textureEnabled = g_boundTexture >= 0 && g_textures.count(g_boundTexture) != 0;
		glUniform1i(u.textureEnabled, textureEnabled);
		if (textureEnabled)
		{
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, g_textures[g_boundTexture].id);
			glUniform1i(u.sampler, 0);
		}

		glUniform1i(u.fogEnabled, g_fogEnabled);
		glUniform1i(u.fogMode, g_fogMode);
		glUniform1f(u.fogNear, g_fogNear);
		glUniform1f(u.fogFar, g_fogFar);
		glUniform1f(u.fogDensity, g_fogDensity);
		glUniform3fv(u.fogColour, 1, g_fogColour);

		glUniform1i(u.alphaTestEnabled, g_alphaTestEnabled);
		glUniform1i(u.alphaFunc, g_alphaFunc == D3D11_COMPARISON_GREATER ? 1 : 0);
		glUniform1f(u.alphaRef, g_alphaRef);
		glUniform1f(u.forceLod, (float)g_forceLod);

		int stride = VertexStride(vType);
		glBindVertexArray(g_vao);
		glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
		glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)count * stride, dataIn, GL_DYNAMIC_DRAW);

		if (primType == C4JRender::PRIMITIVE_TYPE_QUAD_LIST)
		{
			// Core GL has no quad primitive, so expand to triangles with a
			// shared index buffer: 0,1,2, 0,2,3 per quad, which is the winding
			// a GL_QUADS list would have produced. One glDrawElements for the
			// whole batch - a chunk layer is thousands of quads, so a
			// per-quad draw call here is not viable.
			int quads = count / 4;
			if (quads <= 0) { glBindVertexArray(0); return; }
			EnsureQuadIndexCapacity(quads);
			glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_quadIbo);
			glDrawElements(GL_TRIANGLES, quads * 6, GL_UNSIGNED_INT, (void *)0);
		}
		else
		{
			glDrawArrays(PrimitiveToGL(primType), 0, count);
		}

		glBindVertexArray(0);
	}
}

void C4JRender::DrawVertices(ePrimitiveType PrimitiveType, int count, void *dataIn, eVertexType vType, C4JRender::ePixelShaderType psType)
{
	// Decode the packed terrain format up front so everything downstream - both
	// the recording path and the immediate path - deals in one vertex layout.
	std::vector<unsigned char> expanded;
	if (vType == VERTEX_TYPE_COMPRESSED)
	{
		ExpandCompressedVertices(dataIn, count, expanded);
		dataIn = expanded.data();
		vType = VERTEX_TYPE_PF3_TF2_CB4_NB4_XW1;
	}

	if (Recording())
	{
		t_recordBuf.emplace_back();
		RecordedCmd &c = t_recordBuf.back();
		c.op = RecordedCmd::OP_DRAW;
		c.i[0] = (int)PrimitiveType;
		c.i[1] = count;
		c.i[2] = (int)vType;
		c.i[3] = (int)psType;
		if (!expanded.empty())
		{
			c.data = std::move(expanded);
		}
		else
		{
			size_t bytes = (size_t)count * VertexStride(vType);
			c.data.assign((unsigned char *)dataIn, (unsigned char *)dataIn + bytes);
		}
		return;
	}

	// A draw arriving off the GL thread with no command buffer open would issue
	// GL calls with no current context. That cannot happen now that the
	// recording cursor is per-thread, but it is exactly what the previous shared
	// g_recording flag allowed (one thread's CBuffEnd() switched recording off
	// under another thread mid-list), and it silently dropped that chunk's
	// geometry. Fail loudly rather than call into a context-less GL.
	if (g_glThreadIdValid && std::this_thread::get_id() != g_glThreadId)
	{
		static bool warned = false;
		if (!warned)
		{
			fprintf(stderr, "LinuxRender: DrawVertices() called off the GL thread with no command "
			                "buffer open - draw dropped. This indicates a CBuffStart/CBuffEnd "
			                "mismatch.\n");
			warned = true;
		}
		return;
	}

	ApplyStateAndDraw(PrimitiveType, count, dataIn, vType, psType);
}

void C4JRender::DrawVertexBuffer(ePrimitiveType PrimitiveType, int count, ID3D11Buffer *buffer, C4JRender::eVertexType vType, C4JRender::ePixelShaderType psType)
{
	// Never called anywhere in the codebase (confirmed by repo-wide grep -
	// all real vertex submission goes through DrawVertices via Tesselator).
	// buffer is an opaque ID3D11Buffer* with no Linux-side buffer object
	// behind it, so there is nothing meaningful to draw here.
	static bool warned = false;
	if (!warned)
	{
		fprintf(stderr, "LinuxRender: DrawVertexBuffer() called but is unimplemented (dead code path).\n");
		warned = true;
	}
}

void C4JRender::CBuffLockStaticCreations()
{
}

int C4JRender::CBuffCreate(int count)
{
	std::lock_guard<std::mutex> lock(g_cbuffMutex);
	int first = g_nextCBuffId;
	for (int i = 0; i < count; i++)
		g_cbuffs[g_nextCBuffId++].alive = true;
	return first;
}

void C4JRender::CBuffDelete(int first, int count)
{
	std::lock_guard<std::mutex> lock(g_cbuffMutex);
	for (int i = 0; i < count; i++)
	{
		auto it = g_cbuffs.find(first + i);
		if (it == g_cbuffs.end()) continue;
		g_cbuffTotalBytes -= it->second.bytes;
		g_cbuffs.erase(it);
	}
}

void C4JRender::CBuffStart(int index, bool full)
{
	// Record into this thread's own buffer and publish it in CBuffEnd(). The
	// stored buffer is left untouched until then, so a chunk mid-rebuild keeps
	// rendering its previous geometry instead of flickering empty.
	t_recording = true;
	t_recordingIndex = index;
	t_recordBuf.clear();
}

void C4JRender::CBuffClear(int index)
{
	// Called from the chunk-rebuild threads too (Chunk.cpp:329/464/469).
	std::lock_guard<std::mutex> lock(g_cbuffMutex);
	auto it = g_cbuffs.find(index);
	if (it == g_cbuffs.end()) return;
	g_cbuffTotalBytes -= it->second.bytes;
	it->second.bytes = 0;
	it->second.cmds.clear();
}

int C4JRender::CBuffSize(int index)
{
	std::lock_guard<std::mutex> lock(g_cbuffMutex);
	if (index < 0)
		return (int)g_cbuffTotalBytes;
	auto it = g_cbuffs.find(index);
	return it == g_cbuffs.end() ? 0 : (int)it->second.bytes;
}

void C4JRender::CBuffEnd()
{
	if (!t_recording) return;

	size_t bytes = 0;
	for (const RecordedCmd &c : t_recordBuf)
		bytes += c.data.size();

	{
		std::lock_guard<std::mutex> lock(g_cbuffMutex);
		CommandBuffer &cb = g_cbuffs[t_recordingIndex];
		cb.alive = true;
		// swap rather than move-assign: this thread gets the buffer the stored
		// list was using, so its capacity is recycled instead of reallocated on
		// every rebuild.
		cb.cmds.swap(t_recordBuf);
		g_cbuffTotalBytes -= cb.bytes;
		g_cbuffTotalBytes += bytes;
		cb.bytes = bytes;
	}

	t_recordBuf.clear();   // frees the previous list's payloads, keeps capacity
	t_recording = false;
	t_recordingIndex = -1;
}

bool C4JRender::CBuffCall(int index, bool full)
{
	std::lock_guard<std::mutex> lock(g_cbuffMutex);

	auto it = g_cbuffs.find(index);
	if (it == g_cbuffs.end() || it->second.cmds.empty())
		return false;

	// Replay dispatches back through the public entry points. Recording is
	// inactive on this thread, so each one applies live - no duplicated state
	// logic, and a list correctly inherits the caller's matrices (which is what
	// makes Chunk::rebuild's recorded translateToPos() compose with the camera
	// modelview the way real glCallList does).
	size_t depthBefore[3];
	for (int m = 0; m < 3; m++) depthBefore[m] = g_matStack[m].size();
	int modeBefore = g_matMode;

	for (const RecordedCmd &c : it->second.cmds)
	{
		switch (c.op)
		{
		case RecordedCmd::OP_DRAW:
			ApplyStateAndDraw((ePrimitiveType)c.i[0], c.i[1], c.data.data(),
			                  (eVertexType)c.i[2], (ePixelShaderType)c.i[3]);
			break;

		case RecordedCmd::OP_MATRIX_MODE:        MatrixMode(c.i[0]); break;
		case RecordedCmd::OP_MATRIX_PUSH:        MatrixPush(); break;
		case RecordedCmd::OP_MATRIX_POP:         MatrixPop(); break;
		case RecordedCmd::OP_MATRIX_IDENTITY:    MatrixSetIdentity(); break;
		case RecordedCmd::OP_MATRIX_TRANSLATE:   MatrixTranslate(c.f[0], c.f[1], c.f[2]); break;
		case RecordedCmd::OP_MATRIX_ROTATE:      MatrixRotate(c.f[0], c.f[1], c.f[2], c.f[3]); break;
		case RecordedCmd::OP_MATRIX_SCALE:       MatrixScale(c.f[0], c.f[1], c.f[2]); break;
		case RecordedCmd::OP_MATRIX_MULT:        MatrixMult(const_cast<float *>(c.f)); break;
		case RecordedCmd::OP_MATRIX_PERSPECTIVE: MatrixPerspective(c.f[0], c.f[1], c.f[2], c.f[3]); break;
		case RecordedCmd::OP_MATRIX_ORTHO:       MatrixOrthogonal(c.f[0], c.f[1], c.f[2], c.f[3], c.f[4], c.f[5]); break;

		case RecordedCmd::OP_DEPTH_MASK:         StateSetDepthMask(c.i[0] != 0); break;
		case RecordedCmd::OP_DEPTH_FUNC:         StateSetDepthFunc(c.i[0]); break;
		case RecordedCmd::OP_DEPTH_TEST_ENABLE:  StateSetDepthTestEnable(c.i[0] != 0); break;
		case RecordedCmd::OP_DEPTH_SLOPE_BIAS:   StateSetDepthSlopeAndBias(c.f[0], c.f[1]); break;
		case RecordedCmd::OP_ALPHA_TEST_ENABLE:  StateSetAlphaTestEnable(c.i[0] != 0); break;
		case RecordedCmd::OP_ALPHA_FUNC:         StateSetAlphaFunc(c.i[0], c.f[0]); break;
		case RecordedCmd::OP_BLEND_ENABLE:       StateSetBlendEnable(c.i[0] != 0); break;
		case RecordedCmd::OP_BLEND_FUNC:         StateSetBlendFunc(c.i[0], c.i[1]); break;
		case RecordedCmd::OP_BLEND_FACTOR:       StateSetBlendFactor((unsigned int)c.i[0]); break;
		case RecordedCmd::OP_FACE_CULL:          StateSetFaceCull(c.i[0] != 0); break;
		case RecordedCmd::OP_FACE_CULL_CW:       StateSetFaceCullCW(c.i[0] != 0); break;
		case RecordedCmd::OP_LINE_WIDTH:         StateSetLineWidth(c.f[0]); break;
		case RecordedCmd::OP_WRITE_ENABLE:
			StateSetWriteEnable(c.i[0] & 1, c.i[0] & 2, c.i[0] & 4, c.i[0] & 8);
			break;
		case RecordedCmd::OP_COLOUR:             StateSetColour(c.f[0], c.f[1], c.f[2], c.f[3]); break;
		case RecordedCmd::OP_TEXTURE_BIND:       TextureBind(c.i[0]); break;
		case RecordedCmd::OP_TEXTURE_BIND_VERTEX: TextureBindVertex(c.i[0]); break;
		case RecordedCmd::OP_VERTEX_TEXTURE_UV:  StateSetVertexTextureUV(c.f[0], c.f[1]); break;
		case RecordedCmd::OP_FORCE_LOD:          StateSetForceLOD(c.i[0]); break;
		case RecordedCmd::OP_LIGHTING_ENABLE:    StateSetLightingEnable(c.i[0] != 0); break;
		case RecordedCmd::OP_LIGHT_ENABLE:       StateSetLightEnable(c.i[0], c.i[1] != 0); break;
		case RecordedCmd::OP_LIGHT_COLOUR:       StateSetLightColour(c.i[0], c.f[0], c.f[1], c.f[2]); break;
		case RecordedCmd::OP_LIGHT_AMBIENT:      StateSetLightAmbientColour(c.f[0], c.f[1], c.f[2]); break;
		case RecordedCmd::OP_LIGHT_DIR:          StateSetLightDirection(c.i[0], c.f[0], c.f[1], c.f[2]); break;
		case RecordedCmd::OP_FOG_ENABLE:         StateSetFogEnable(c.i[0] != 0); break;
		case RecordedCmd::OP_FOG_MODE:           StateSetFogMode(c.i[0]); break;
		case RecordedCmd::OP_FOG_NEAR:           StateSetFogNearDistance(c.f[0]); break;
		case RecordedCmd::OP_FOG_FAR:            StateSetFogFarDistance(c.f[0]); break;
		case RecordedCmd::OP_FOG_DENSITY:        StateSetFogDensity(c.f[0]); break;
		case RecordedCmd::OP_FOG_COLOUR:         StateSetFogColour(c.f[0], c.f[1], c.f[2]); break;
		case RecordedCmd::OP_TEXGEN_COL:
			StateSetTexGenCol(c.i[0], c.f[0], c.f[1], c.f[2], c.f[3], c.i[1] != 0);
			break;

		default: break;
		}
	}

	// Real GL display lists let state changes leak past glCallList, so ordinary
	// state is deliberately not restored here - LevelRenderer.cpp:172's
	// glDepthMask(false) inside skyList is there precisely to apply.
	//
	// Matrix *stack shape* is the exception. Which stack is selected and how deep
	// it is are not state the caller can re-establish; every recording site is
	// push/pop balanced, so a mismatch means a malformed list, and letting it
	// drift would corrupt the live camera for every subsequent draw. Restore it
	// and say so.
	g_matMode = modeBefore;
	for (int m = 0; m < 3; m++)
	{
		if (g_matStack[m].size() == depthBefore[m]) continue;
		static bool warned = false;
		if (!warned)
		{
			fprintf(stderr, "LinuxRender: command buffer %d left the matrix stack unbalanced "
			                "(%zu -> %zu); restoring.\n", index, depthBefore[m], g_matStack[m].size());
			warned = true;
		}
		g_matStack[m].resize(depthBefore[m] > 0 ? depthBefore[m] : 1, MatIdentity());
	}

	return true;
}

void C4JRender::CBuffTick()
{
}

void C4JRender::CBuffDeferredModeStart()
{
	// Deferred-mode batching hint; every CBuffCall already replays
	// immediately in submission order, so there is nothing to defer.
}

void C4JRender::CBuffDeferredModeEnd()
{
}

int C4JRender::TextureCreate()
{
	int id = g_nextTextureId++;
	TextureRec rec;
	glGenTextures(1, &rec.id);

	// Establish the sampler defaults *here*, once, rather than in TextureData().
	//
	// TextureData() used to re-apply these on every upload, which silently threw away
	// whatever the engine had just asked for: Textures::loadTexture sets the wrap and
	// filter modes (Textures.cpp:498-529) and *then* calls TextureData to upload
	// (Textures.cpp:584), so the parameters were overwritten a few lines later. Entity
	// shadows were the loudest victim - shadow.png is loaded as "%clamp%misc/shadow" and
	// the clamp is load-bearing, because the disc is inscribed in the full 64x64 with
	// zero-alpha borders and each ground quad spans exactly one UV period
	// (1/(2*shadowRadius) == 1.0 for the usual radius of 0.5). Clamping is what makes the
	// tiles around the mob transparent; with GL_REPEAT every neighbouring tile drew
	// another full disc, so one mob's shadow "leaked" into the adjacent tiles as half
	// circles around the correct one. The same clobber also discarded the GL_LINEAR that
	// "%blur%" textures (pumpkinblur, glint) ask for.
	//
	// GL's own defaults are no good as a fallback: GL_TEXTURE_MIN_FILTER defaults to
	// GL_NEAREST_MIPMAP_LINEAR, and almost nothing in this tree ships a complete mip
	// chain (…MipMapLevel2.png is absent for ~100 textures), which would make those
	// textures mipmap-incomplete and sample as opaque black. So default to GL_NEAREST,
	// matching what the engine asks for in the common case.
	glBindTexture(GL_TEXTURE_2D, rec.id);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

	g_textures[id] = rec;
	return id;
}

void C4JRender::TextureFree(int idx)
{
	auto it = g_textures.find(idx);
	if (it == g_textures.end()) return;
	glDeleteTextures(1, &it->second.id);
	g_textures.erase(it);
	if (g_boundTexture == idx)
		g_boundTexture = -1;
	if (g_boundTextureVertex == idx)
		g_boundTextureVertex = -1;
	if (g_lastBoundTexture == idx)
		g_lastBoundTexture = -1;
}

void C4JRender::TextureBind(int idx)
{
	if (Recording()) { RecI(RecordedCmd::OP_TEXTURE_BIND, idx); return; }
	g_boundTexture = idx;
	g_lastBoundTexture = idx;
}

void C4JRender::TextureBindVertex(int idx)
{
	// This is a SECOND, independent texture slot - not an alias for the
	// fragment-stage bind. Its only real callers are GameRenderer.cpp:808/841,
	// which bind the 16x16 light texture (GameRenderer.cpp:155 allocates
	// lightPixels as 16*16) for per-block light modulation.
	//
	// Collapsing it onto g_boundTexture was why nothing looked textured: the
	// engine binds terrain.png (256x256) with TextureBind and then the lightmap
	// (16x16) with TextureBindVertex, so every following draw sampled the 16x16
	// lightmap instead of the atlas. Terrain UVs span one atlas tile = 1/16 of
	// the texture, which over a 16x16 image is a single texel - hence flat,
	// arbitrarily-coloured faces, and correctly-textured geometry only where
	// something happened to re-bind afterwards.
	if (Recording()) { RecI(RecordedCmd::OP_TEXTURE_BIND_VERTEX, idx); return; }
	g_boundTextureVertex = idx;
	g_lastBoundTexture = idx;
}

void C4JRender::TextureSetTextureLevels(int levels)
{
	g_textureLevels = levels;
}

int C4JRender::TextureGetTextureLevels()
{
	return g_textureLevels;
}

void C4JRender::TextureData(int width, int height, void *data, int level, eTextureFormat format)
{
	if (g_lastBoundTexture < 0) return;
	auto it = g_textures.find(g_lastBoundTexture);
	if (it == g_textures.end()) return;

	glBindTexture(GL_TEXTURE_2D, it->second.id);
	// TEXTURE_FORMAT_RxGyBzAw is the only format the header still exposes
	// (the others are commented out as "not directly available on D3D11");
	// Tesselator/Textures.cpp always supply tightly-packed RGBA8 data.
	glTexImage2D(GL_TEXTURE_2D, level, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
	// Deliberately no glTexParameteri here - sampler state belongs to the texture object
	// and the engine sets it before uploading. Re-applying it after the upload used to
	// discard the caller's choice; see TextureCreate().
	if (level == 0)
	{
		it->second.width = width;
		it->second.height = height;
	}
}

void C4JRender::TextureDataUpdate(int xoffset, int yoffset, int width, int height, void *data, int level)
{
	if (g_lastBoundTexture < 0) return;
	auto it = g_textures.find(g_lastBoundTexture);
	if (it == g_textures.end()) return;
	glBindTexture(GL_TEXTURE_2D, it->second.id);
	glTexSubImage2D(GL_TEXTURE_2D, level, xoffset, yoffset, width, height, GL_RGBA, GL_UNSIGNED_BYTE, data);
}

void C4JRender::TextureSetParam(int param, int value)
{
	if (g_lastBoundTexture < 0) return;
	auto it = g_textures.find(g_lastBoundTexture);
	if (it == g_textures.end()) return;
	glBindTexture(GL_TEXTURE_2D, it->second.id);

	GLenum glParam = 0;
	// param/value arrive from glWrapper.cpp's glTexParameteri(), which never
	// includes glew.h - so they carry 4J_Render.h's *fake*-vocabulary values
	// (LinuxRenderFakeGL mirrors them), not real GL enums. Note the fake
	// vocabulary reuses 0/1 for both the filter group (NEAREST=0/LINEAR=1)
	// and the wrap group (CLAMP=0/REPEAT=1) - which group `value` belongs to
	// depends on which `param` this call is for, so the two must be
	// switched together, not via one shared value->glValue table.
	bool isWrapParam = (param == LinuxRenderFakeGL::TEXTURE_WRAP_S || param == LinuxRenderFakeGL::TEXTURE_WRAP_T);

	switch (param)
	{
	case LinuxRenderFakeGL::TEXTURE_MIN_FILTER: glParam = GL_TEXTURE_MIN_FILTER; break;
	case LinuxRenderFakeGL::TEXTURE_MAG_FILTER: glParam = GL_TEXTURE_MAG_FILTER; break;
	case LinuxRenderFakeGL::TEXTURE_WRAP_S: glParam = GL_TEXTURE_WRAP_S; break;
	case LinuxRenderFakeGL::TEXTURE_WRAP_T: glParam = GL_TEXTURE_WRAP_T; break;
	default: return;
	}

	GLint glValue;
	if (isWrapParam)
		glValue = (value == LinuxRenderFakeGL::WRAP_REPEAT) ? GL_REPEAT : GL_CLAMP_TO_EDGE;
	else
		glValue = (value == LinuxRenderFakeGL::FILTER_LINEAR) ? GL_LINEAR : GL_NEAREST;

	glTexParameteri(GL_TEXTURE_2D, glParam, glValue);
}

void C4JRender::TextureDynamicUpdateStart()
{
}

void C4JRender::TextureDynamicUpdateEnd()
{
}

namespace
{
	// stb hands back RGBA *bytes*. The engine, however, treats every loaded pixel
	// as a packed ARGB int - Texture.cpp:591 and :643 read (p>>24) as alpha,
	// (p>>16) as red, (p>>8) as green, (p>>0) as blue - which on a little-endian
	// host means the bytes must be laid out B,G,R,A. Handing over stb's R,G,B,A
	// therefore transposes red and blue at the very source of every texture.
	//
	// That is what made water render red, pigs blue and wood/sand/TNT blue: the
	// engine's byteRemapRGBA (Texture.cpp:571) faithfully moved the channels it
	// was told were red and blue, so the swap survived all the way to the GPU.
	// Correcting it here rather than by uploading as GL_BGRA also keeps the
	// engine's own per-pixel colour maths (crispBlend's mip generation, biome
	// tinting, texture stitching) operating on the channels it thinks it has.
	void PackRgbaBytesAsArgbInts(unsigned char *px, size_t pixelCount)
	{
		for (size_t i = 0; i < pixelCount; i++)
		{
			unsigned char *p = px + i * 4;
			std::swap(p[0], p[2]);
		}
	}

	HRESULT LoadTextureDataFromMemory(const unsigned char *bytes, size_t len, D3DXIMAGE_INFO *pSrcInfo, int **ppDataOut)
	{
		int w = 0, h = 0, comp = 0;
		unsigned char *pixels = stbi_load_from_memory(bytes, (int)len, &w, &h, &comp, 4);
		if (!pixels)
		{
			fprintf(stderr, "LinuxRender: LoadTextureData failed: %s\n", stbi_failure_reason());
			return -1; // generic HRESULT failure
		}
		if (pSrcInfo) { pSrcInfo->Width = w; pSrcInfo->Height = h; }
		int *out = (int *)malloc((size_t)w * h * 4);
		memcpy(out, pixels, (size_t)w * h * 4);
		stbi_image_free(pixels);
		PackRgbaBytesAsArgbInts((unsigned char *)out, (size_t)w * h);
		*ppDataOut = out;
		return S_OK;
	}
}

HRESULT C4JRender::LoadTextureData(const char *szFilename, D3DXIMAGE_INFO *pSrcInfo, int **ppDataOut)
{
	int w = 0, h = 0, comp = 0;
	unsigned char *pixels = stbi_load(szFilename, &w, &h, &comp, 4);
	if (!pixels)
	{
		fprintf(stderr, "LinuxRender: LoadTextureData(%s) failed: %s\n", szFilename, stbi_failure_reason());
		return -1;
	}
	if (pSrcInfo) { pSrcInfo->Width = w; pSrcInfo->Height = h; }
	int *out = (int *)malloc((size_t)w * h * 4);
	memcpy(out, pixels, (size_t)w * h * 4);
	stbi_image_free(pixels);
	PackRgbaBytesAsArgbInts((unsigned char *)out, (size_t)w * h);
	*ppDataOut = out;
	return S_OK;
}

HRESULT C4JRender::LoadTextureData(BYTE *pbData, DWORD dwBytes, D3DXIMAGE_INFO *pSrcInfo, int **ppDataOut)
{
	return LoadTextureDataFromMemory(pbData, dwBytes, pSrcInfo, ppDataOut);
}

HRESULT C4JRender::SaveTextureData(const char *szFilename, D3DXIMAGE_INFO *pSrcInfo, int *ppDataIn)
{
	if (!pSrcInfo || !ppDataIn) return -1;
	// Inverse of PackRgbaBytesAsArgbInts(): the caller's pixels are packed ARGB
	// ints (B,G,R,A in memory), stb_image_write expects R,G,B,A. Convert a copy
	// so the caller's buffer is left untouched.
	size_t n = (size_t)pSrcInfo->Width * pSrcInfo->Height;
	std::vector<unsigned char> rgba((unsigned char *)ppDataIn, (unsigned char *)ppDataIn + n * 4);
	PackRgbaBytesAsArgbInts(rgba.data(), n);
	int ok = stbi_write_png(szFilename, pSrcInfo->Width, pSrcInfo->Height, 4, rgba.data(), pSrcInfo->Width * 4);
	return ok ? S_OK : -1;
}

HRESULT C4JRender::SaveTextureDataToMemory(void *pOutput, int outputCapacity, int *outputLength, int width, int height, int *ppDataIn)
{
	// stb_image_write only writes to a file or a caller-supplied callback,
	// never into a fixed-size caller buffer with a hard capacity check;
	// no caller of this method exists in the source that's part of this
	// port's scope, so this is left unimplemented rather than guessed.
	if (outputLength) *outputLength = 0;
	return -1;
}

void C4JRender::TextureGetStats()
{
}

// See LinuxRender.h. TextureGetTexture() below can't serve this purpose because its
// return type is a D3D11 interface; this hands out what Linux actually has, so
// LinuxUIController can wrap a game texture for Iggy.
bool LinuxRender_GetGLTexture(int textureId, unsigned int *outGLName,
                             int *outWidth, int *outHeight)
{
	auto it = g_textures.find(textureId);
	if (it == g_textures.end())
		return false;
	if (outGLName) *outGLName = (unsigned int)it->second.id;
	if (outWidth)  *outWidth  = it->second.width;
	if (outHeight) *outHeight = it->second.height;
	return true;
}

ID3D11ShaderResourceView *C4JRender::TextureGetTexture(int idx)
{
	// Only ever called via Texture.cpp's TextureGetTextureLevels() check in
	// the source read for this port, never dereferenced as a real D3D11
	// view; ID3D11ShaderResourceView is an opaque `void` typedef on Linux
	// (see LinuxStubs.h), so there is nothing real to return.
	return nullptr;
}

void C4JRender::StateSetColour(float r, float g, float b, float a)
{
	if (Recording()) { RecF(RecordedCmd::OP_COLOUR, r, g, b, a); return; }
	g_colour[0] = r; g_colour[1] = g; g_colour[2] = b; g_colour[3] = a;
	glVertexAttrib4f(2, r, g, b, a);
}

void C4JRender::StateSetDepthMask(bool enable)
{
	if (Recording()) { RecB(RecordedCmd::OP_DEPTH_MASK, enable); return; }
	glDepthMask(enable ? GL_TRUE : GL_FALSE);
}

void C4JRender::StateSetBlendEnable(bool enable)
{
	if (Recording()) { RecB(RecordedCmd::OP_BLEND_ENABLE, enable); return; }
	g_blendEnabled = enable;
	if (enable) glEnable(GL_BLEND); else glDisable(GL_BLEND);
}

namespace
{
	GLenum BlendFactorToGL(int f)
	{
		switch (f)
		{
		case D3D11_BLEND_ZERO: return GL_ZERO;
		case D3D11_BLEND_ONE: return GL_ONE;
		case D3D11_BLEND_SRC_COLOR: return GL_SRC_COLOR;
		case D3D11_BLEND_INV_SRC_COLOR: return GL_ONE_MINUS_SRC_COLOR;
		case D3D11_BLEND_SRC_ALPHA: return GL_SRC_ALPHA;
		case D3D11_BLEND_INV_SRC_ALPHA: return GL_ONE_MINUS_SRC_ALPHA;
		case D3D11_BLEND_DEST_ALPHA: return GL_DST_ALPHA;
		case D3D11_BLEND_DEST_COLOR: return GL_DST_COLOR;
		case D3D11_BLEND_INV_DEST_COLOR: return GL_ONE_MINUS_DST_COLOR;
		case D3D11_BLEND_BLEND_FACTOR: return GL_CONSTANT_ALPHA;
		case D3D11_BLEND_INV_BLEND_FACTOR: return GL_ONE_MINUS_CONSTANT_ALPHA;
		default: return GL_ONE;
		}
	}
}

void C4JRender::StateSetBlendFunc(int src, int dst)
{
	if (Recording()) { RecI(RecordedCmd::OP_BLEND_FUNC, src, dst); return; }
	glBlendFunc(BlendFactorToGL(src), BlendFactorToGL(dst));
}

void C4JRender::StateSetBlendFactor(unsigned int colour)
{
	if (Recording()) { RecI(RecordedCmd::OP_BLEND_FACTOR, (int)colour); return; }
	float a = ((colour >> 24) & 0xFF) / 255.0f;
	float r = ((colour >> 16) & 0xFF) / 255.0f;
	float g = ((colour >> 8) & 0xFF) / 255.0f;
	float b = (colour & 0xFF) / 255.0f;
	glBlendColor(r, g, b, a);
}

void C4JRender::StateSetAlphaFunc(int func, float param)
{
	if (Recording())
	{
		float f[1] = {param};
		int iv[1] = {func};
		Rec(RecordedCmd::OP_ALPHA_FUNC, f, 1, iv, 1);
		return;
	}
	g_alphaFunc = func;
	g_alphaRef = param;
}

void C4JRender::StateSetDepthFunc(int func)
{
	if (Recording()) { RecI(RecordedCmd::OP_DEPTH_FUNC, func); return; }
	GLenum glFunc;
	switch (func)
	{
	case D3D11_COMPARISON_GREATER: glFunc = GL_GREATER; break;
	case D3D11_COMPARISON_EQUAL: glFunc = GL_EQUAL; break;
	case D3D11_COMPARISON_LESS_EQUAL: glFunc = GL_LEQUAL; break;
	case D3D11_COMPARISON_GREATER_EQUAL: glFunc = GL_GEQUAL; break;
	case D3D11_COMPARISON_ALWAYS: glFunc = GL_ALWAYS; break;
	default: glFunc = GL_LEQUAL; break;
	}
	glDepthFunc(glFunc);
}

void C4JRender::StateSetFaceCull(bool enable)
{
	if (Recording()) { RecB(RecordedCmd::OP_FACE_CULL, enable); return; }
	g_cullEnabled = enable;
	if (enable) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
}

void C4JRender::StateSetFaceCullCW(bool enable)
{
	if (Recording()) { RecB(RecordedCmd::OP_FACE_CULL_CW, enable); return; }

	// The sole caller is glWrapper.cpp:386's glCullFace(dir), which passes
	// `dir == GL_BACK` (stubs.h: GL_FRONT = 0, GL_BACK = 1). So `enable` means
	// "cull back faces" - which face to discard, NOT which winding is front.
	//
	// Mapping it to glFrontFace() instead was wrong and showed up as blocks
	// rendering inside out: the normal case (Minecraft.cpp:381 glCullFace(GL_BACK))
	// set glFrontFace(GL_CW), and since GL's cull target already defaults to
	// GL_BACK, that culled every CCW face - which in this GL-derived geometry are
	// the front faces, leaving only block interiors visible.
	//
	// Selecting the cull target and leaving the winding at GL's CCW default
	// reproduces the caller's original glCullFace(dir) exactly, and also expresses
	// DragonModel.cpp:211's glCullFace(GL_FRONT), which glFrontFace could not.
	glCullFace(enable ? GL_BACK : GL_FRONT);
}

void C4JRender::StateSetLineWidth(float width)
{
	if (Recording()) { RecF(RecordedCmd::OP_LINE_WIDTH, width); return; }
	glLineWidth(width);
}

void C4JRender::StateSetWriteEnable(bool red, bool green, bool blue, bool alpha)
{
	if (Recording())
	{
		RecI(RecordedCmd::OP_WRITE_ENABLE,
		     (red ? 1 : 0) | (green ? 2 : 0) | (blue ? 4 : 0) | (alpha ? 8 : 0));
		return;
	}
	glColorMask(red ? GL_TRUE : GL_FALSE, green ? GL_TRUE : GL_FALSE,
	            blue ? GL_TRUE : GL_FALSE, alpha ? GL_TRUE : GL_FALSE);
}

void C4JRender::StateSetDepthTestEnable(bool enable)
{
	if (Recording()) { RecB(RecordedCmd::OP_DEPTH_TEST_ENABLE, enable); return; }
	g_depthTestEnabled = enable;
	if (enable) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
}

void C4JRender::StateSetAlphaTestEnable(bool enable)
{
	if (Recording()) { RecB(RecordedCmd::OP_ALPHA_TEST_ENABLE, enable); return; }
	g_alphaTestEnabled = enable;
}

void C4JRender::StateSetDepthSlopeAndBias(float slope, float bias)
{
	if (Recording()) { RecF(RecordedCmd::OP_DEPTH_SLOPE_BIAS, slope, bias); return; }

	// Zero means "no offset", and it is the only way callers can say so: the engine's
	// glEnable/glDisable(GL_POLYGON_OFFSET_FILL) never reach C4JRender at all, because
	// that constant is 0 (stubs.h) and glWrapper.cpp's switches have no case for it.
	// So drive the enable from the values, rather than latching it on for the process.
	if (slope == 0.0f && bias == 0.0f)
	{
		glDisable(GL_POLYGON_OFFSET_FILL);
		glPolygonOffset(0.0f, 0.0f);
		return;
	}
	glEnable(GL_POLYGON_OFFSET_FILL);
	glPolygonOffset(slope, bias);
}

void C4JRender::StateSetFogEnable(bool enable)
{
	if (Recording()) { RecB(RecordedCmd::OP_FOG_ENABLE, enable); return; }
	g_fogEnabled = enable;
}

void C4JRender::StateSetFogMode(int mode)
{
	if (Recording()) { RecI(RecordedCmd::OP_FOG_MODE, mode); return; }
	g_fogMode = mode;
}

void C4JRender::StateSetFogNearDistance(float dist)
{
	if (Recording()) { RecF(RecordedCmd::OP_FOG_NEAR, dist); return; }
	g_fogNear = dist;
}

void C4JRender::StateSetFogFarDistance(float dist)
{
	if (Recording()) { RecF(RecordedCmd::OP_FOG_FAR, dist); return; }
	g_fogFar = dist;
}

void C4JRender::StateSetFogDensity(float density)
{
	if (Recording()) { RecF(RecordedCmd::OP_FOG_DENSITY, density); return; }
	g_fogDensity = density;
}

void C4JRender::StateSetFogColour(float red, float green, float blue)
{
	if (Recording()) { RecF(RecordedCmd::OP_FOG_COLOUR, red, green, blue); return; }
	g_fogColour[0] = red; g_fogColour[1] = green; g_fogColour[2] = blue;
}

void C4JRender::StateSetLightingEnable(bool enable)
{
	if (Recording()) { RecB(RecordedCmd::OP_LIGHTING_ENABLE, enable); return; }
	g_lightingEnabled = enable;
}

void C4JRender::StateSetVertexTextureUV(float u, float v)
{
	if (Recording()) { RecF(RecordedCmd::OP_VERTEX_TEXTURE_UV, u, v); return; }
	glVertexAttrib2f(1, u, v);
}

void C4JRender::StateSetLightColour(int light, float red, float green, float blue)
{
	if (Recording())
	{
		float f[3] = {red, green, blue};
		int iv[1] = {light};
		Rec(RecordedCmd::OP_LIGHT_COLOUR, f, 3, iv, 1);
		return;
	}
	if (light < 0 || light > 1) return;
	g_lightColour[light][0] = red; g_lightColour[light][1] = green; g_lightColour[light][2] = blue;
}

void C4JRender::StateSetLightAmbientColour(float red, float green, float blue)
{
	if (Recording()) { RecF(RecordedCmd::OP_LIGHT_AMBIENT, red, green, blue); return; }
	g_lightAmbient[0] = red; g_lightAmbient[1] = green; g_lightAmbient[2] = blue;
}

void C4JRender::StateSetLightDirection(int light, float x, float y, float z)
{
	if (Recording())
	{
		float f[3] = {x, y, z};
		int iv[1] = {light};
		Rec(RecordedCmd::OP_LIGHT_DIR, f, 3, iv, 1);
		return;
	}
	if (light < 0 || light > 1) return;
	g_lightDir[light][0] = x; g_lightDir[light][1] = y; g_lightDir[light][2] = z;
}

void C4JRender::StateSetLightEnable(int light, bool enable)
{
	if (Recording()) { RecI(RecordedCmd::OP_LIGHT_ENABLE, light, enable ? 1 : 0); return; }
	if (light < 0 || light > 1) return;
	g_lightEnable[light] = enable;
}

void C4JRender::StateSetViewport(eViewportType viewportType)
{
	// Viewport is whole-pass scope, not per-draw state, and no display list
	// contains one; applying live is the only sensible behaviour.
	if (Recording()) WarnUnrecordable("StateSetViewport");
	int w = 0, h = 0;
	if (g_window)
		SDL_GL_GetDrawableSize(g_window, &w, &h);

	switch (viewportType)
	{
	case VIEWPORT_TYPE_SPLIT_TOP:              glViewport(0, h / 2, w, h / 2); break;
	case VIEWPORT_TYPE_SPLIT_BOTTOM:           glViewport(0, 0, w, h / 2); break;
	case VIEWPORT_TYPE_SPLIT_LEFT:             glViewport(0, 0, w / 2, h); break;
	case VIEWPORT_TYPE_SPLIT_RIGHT:            glViewport(w / 2, 0, w / 2, h); break;
	case VIEWPORT_TYPE_QUADRANT_TOP_LEFT:      glViewport(0, h / 2, w / 2, h / 2); break;
	case VIEWPORT_TYPE_QUADRANT_TOP_RIGHT:     glViewport(w / 2, h / 2, w / 2, h / 2); break;
	case VIEWPORT_TYPE_QUADRANT_BOTTOM_LEFT:   glViewport(0, 0, w / 2, h / 2); break;
	case VIEWPORT_TYPE_QUADRANT_BOTTOM_RIGHT:  glViewport(w / 2, 0, w / 2, h / 2); break;
	default:                                   glViewport(0, 0, w, h); break;
	}
}

void C4JRender::StateSetEnableViewportClipPlanes(bool enable)
{
	// Split-screen clip-plane emulation (core GL has no gl_ClipDistance
	// wired up in these shaders); StateSetViewport's real glViewport
	// scissoring already prevents split-screen panes drawing over each
	// other for the rectangular splits above, so this is a no-op.
}

void C4JRender::StateSetTexGenCol(int col, float x, float y, float z, float w, bool eyeSpace)
{
	if (Recording())
	{
		float f[4] = {x, y, z, w};
		int iv[2] = {col, eyeSpace ? 1 : 0};
		Rec(RecordedCmd::OP_TEXGEN_COL, f, 4, iv, 2);
		return;
	}
	if (col < 0 || col > 3) return;
	g_texGen[col][0] = x; g_texGen[col][1] = y; g_texGen[col][2] = z; g_texGen[col][3] = w;
	g_texGenEnabled = true;
}

void C4JRender::StateSetStencil(int Function, uint8_t stencil_ref, uint8_t stencil_func_mask, uint8_t stencil_write_mask)
{
	if (Recording()) WarnUnrecordable("StateSetStencil");
	GLenum glFunc;
	switch (Function)
	{
	case D3D11_COMPARISON_GREATER: glFunc = GL_GREATER; break;
	case D3D11_COMPARISON_EQUAL: glFunc = GL_EQUAL; break;
	case D3D11_COMPARISON_LESS_EQUAL: glFunc = GL_LEQUAL; break;
	case D3D11_COMPARISON_GREATER_EQUAL: glFunc = GL_GEQUAL; break;
	case D3D11_COMPARISON_ALWAYS: glFunc = GL_ALWAYS; break;
	default: glFunc = GL_ALWAYS; break;
	}
	glStencilFunc(glFunc, stencil_ref, stencil_func_mask);
	glStencilMask(stencil_write_mask);
}

void C4JRender::StateSetForceLOD(int LOD)
{
	if (Recording()) { RecI(RecordedCmd::OP_FORCE_LOD, LOD); return; }
	g_forceLod = LOD;
}

void C4JRender::BeginEvent(LPCWSTR eventName)
{
	// GPU-debugger event markers (PIX-equivalent); no portable Linux/Mesa
	// hook wired up, no-op.
}

void C4JRender::EndEvent()
{
}

void C4JRender::Suspend()
{
	g_suspended = true;
}

bool C4JRender::Suspended()
{
	return g_suspended;
}

void C4JRender::Resume()
{
	g_suspended = false;
}

C4JRender RenderManager;
