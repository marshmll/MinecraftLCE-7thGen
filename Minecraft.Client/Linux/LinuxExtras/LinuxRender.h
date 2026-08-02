#pragma once

// Linux-only accessors into LinuxRender.cpp, for callers that need the real OpenGL
// object behind a C4JRender texture id.
//
// C4JRender::TextureGetTexture() cannot serve this: its return type is
// ID3D11ShaderResourceView*, which is an opaque `void` typedef on Linux
// (LinuxStubs.h), so the Linux implementation returns nullptr. Rather than change a
// vendor-shaped signature, expose what Linux actually has alongside it.
//
// Used by LinuxUIController::getSubstitutionTexture() to wrap a game-owned texture
// for Iggy (gdraw_GL_WrappedTextureCreate), which is how world thumbnails and
// texture-pack icons reach the UI.

// Look up the GL texture name and dimensions for a C4JRender texture id.
// Returns false if the id is unknown, in which case the outputs are untouched.
// Width/height are 0 until the texture has had data uploaded (TextureData()).
bool LinuxRender_GetGLTexture(int textureId, unsigned int *outGLName,
                             int *outWidth, int *outHeight);
