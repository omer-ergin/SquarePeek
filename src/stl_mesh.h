#pragma once

// Keeps old includes working: Mesh and LoadModel now live in model.h.
#include "model.h"

// Decodes STL only (binary or ASCII). The result follows the SAME contract as LoadModel
// (scene space, statistics, Turkish error). flags: ModelLoadFlags.
bool LoadStl(const void* data, size_t size, uint32_t maxTriangles, Mesh& out, std::wstring& error,
             uint32_t flags = kModelLoadDefault);
