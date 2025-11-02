#pragma once

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#if defined(min)
#undef min
#endif

#if defined(max)
#undef max
#endif

#include <d3d12.h>
#include <dxgi1_6.h>
#include <DirectXMath.h>

#endif
