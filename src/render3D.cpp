/*
	Copyright (C) 2006-2007 shash
	Copyright (C) 2008-2016 DeSmuME team

	This file is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.

	This file is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with the this software.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "render3D.h"

#include <string.h>

#ifdef ENABLE_SSE2
#include <emmintrin.h>
#endif

#ifdef ENABLE_SSSE3
#include <tmmintrin.h>
#endif

#include "bits.h"
#include "common.h"
#include "gfx3d.h"
#include "MMU.h"
#include "texcache.h"
#include "./filter/xbrz.h"

#define TEXTURE_DEPOSTERIZE_THRESHOLD 21	// Possible values are [0-255], where lower a value prevents blending and a higher value allows for more blending

static CACHE_ALIGN u32 dsDepthToD24_LUT[32768] = {0};
int cur3DCore = GPU3D_NULL;

GPU3DInterface gpu3DNull = { 
	"None",
	Render3DBaseCreate,
	Render3DBaseDestroy
};

GPU3DInterface *gpu3D = &gpu3DNull;
Render3D *BaseRenderer = NULL;
Render3D *CurrentRenderer = NULL;

void Render3D_Init()
{
	if (BaseRenderer == NULL)
	{
		BaseRenderer = new Render3D;
	}
	
	if (CurrentRenderer == NULL)
	{
		gpu3D = &gpu3DNull;
		cur3DCore = GPU3D_NULL;
		CurrentRenderer = BaseRenderer;
	}
}

void Render3D_DeInit()
{
	gpu3D->NDS_3D_Close();
	delete BaseRenderer;
	BaseRenderer = NULL;
}

bool NDS_3D_ChangeCore(int newCore)
{
	bool result = false;
		
	Render3DInterface *newRenderInterface = core3DList[newCore];
	if (newRenderInterface->NDS_3D_Init == NULL)
	{
		return result;
	}
	
	// Some resources are shared between renderers, such as the texture cache,
	// so we need to shut down the current renderer now to ensure that any
	// shared resources aren't in use.
	const bool didRenderBegin = CurrentRenderer->GetRenderNeedsFinish();
	CurrentRenderer->RenderFinish();
	gpu3D->NDS_3D_Close();
	gpu3D = &gpu3DNull;
	cur3DCore = GPU3D_NULL;
	BaseRenderer->SetRenderNeedsFinish(didRenderBegin);
	CurrentRenderer = BaseRenderer;
	
	Render3D *newRenderer = newRenderInterface->NDS_3D_Init();
	if (newRenderer == NULL)
	{
		return result;
	}
	
	newRenderer->RequestColorFormat(GPU->GetDisplayInfo().colorFormat);
	
	Render3DError error = newRenderer->SetFramebufferSize(GPU->GetCustomFramebufferWidth(), GPU->GetCustomFramebufferHeight());
	if (error != RENDER3DERROR_NOERR)
	{
		return result;
	}
	
	gpu3D = newRenderInterface;
	cur3DCore = newCore;
	newRenderer->SetRenderNeedsFinish( BaseRenderer->GetRenderNeedsFinish() );
	CurrentRenderer = newRenderer;
	
	result = true;
	return result;
}

Render3D* Render3DBaseCreate()
{
	BaseRenderer->Reset();
	return BaseRenderer;
}

void Render3DBaseDestroy()
{
	if (CurrentRenderer != BaseRenderer)
	{
		Render3D *oldRenderer = CurrentRenderer;
		CurrentRenderer = BaseRenderer;
		delete oldRenderer;
	}
}

static u32 TextureDeposterize_InterpLTE(const u32 pixA, const u32 pixB, const u32 threshold)
{
	const u32 aB = (pixB & 0xFF000000) >> 24;
	if (aB == 0)
	{
		return pixA;
	}
	
	const u32 rA = (pixA & 0x000000FF);
	const u32 gA = (pixA & 0x0000FF00) >> 8;
	const u32 bA = (pixA & 0x00FF0000) >> 16;
	const u32 aA = (pixA & 0xFF000000) >> 24;
	
	const u32 rB = (pixB & 0x000000FF);
	const u32 gB = (pixB & 0x0000FF00) >> 8;
	const u32 bB = (pixB & 0x00FF0000) >> 16;
	
	const u32 rC = ( (rB - rA <= threshold) || (rA - rB <= threshold) ) ? ( ((rA+rB)>>1)  ) : rA;
	const u32 gC = ( (gB - gA <= threshold) || (gA - gB <= threshold) ) ? ( ((gA+gB)>>1)  ) : gA;
	const u32 bC = ( (bB - bA <= threshold) || (bA - bB <= threshold) ) ? ( ((bA+bB)>>1)  ) : bA;
	const u32 aC = ( (bB - aA <= threshold) || (aA - aB <= threshold) ) ? ( ((aA+aB)>>1)  ) : aA;
	
	return (rC | (gC << 8) | (bC << 16) | (aC << 24));
}

static u32 TextureDeposterize_Blend(const u32 pixA, const u32 pixB, const u32 weightA, const u32 weightB)
{
	const u32  aB = (pixB & 0xFF000000) >> 24;
	if (aB == 0)
	{
		return pixA;
	}
	
	const u32 weightSum = weightA + weightB;
	
	const u32 rbA =  pixA & 0x00FF00FF;
	const u32  gA =  pixA & 0x0000FF00;
	const u32  aA = (pixA & 0xFF000000) >> 24;
	
	const u32 rbB =  pixB & 0x00FF00FF;
	const u32  gB =  pixB & 0x0000FF00;
	
	const u32 rbC = ( ((rbA * weightA) + (rbB * weightB)) / weightSum ) & 0x00FF00FF;
	const u32  gC = ( (( gA * weightA) + ( gB * weightB)) / weightSum ) & 0x0000FF00;
	const u32  aC = ( (( aA * weightA) + ( aB * weightB)) / weightSum ) << 24;
	
	return (rbC | gC | aC);
}

FragmentAttributesBuffer::FragmentAttributesBuffer(size_t newCount)
{
	count = newCount;
	
	depth = (u32 *)malloc_alignedCacheLine(count * sizeof(u32));
	opaquePolyID = (u8 *)malloc_alignedCacheLine(count * sizeof(u8));
	translucentPolyID = (u8 *)malloc_alignedCacheLine(count * sizeof(u8));
	stencil = (u8 *)malloc_alignedCacheLine(count * sizeof(u8));
	isFogged = (u8 *)malloc_alignedCacheLine(count * sizeof(u8));
	isTranslucentPoly = (u8 *)malloc_alignedCacheLine(count * sizeof(u8));
}

FragmentAttributesBuffer::~FragmentAttributesBuffer()
{
	free_aligned(depth);
	free_aligned(opaquePolyID);
	free_aligned(translucentPolyID);
	free_aligned(stencil);
	free_aligned(isFogged);
	free_aligned(isTranslucentPoly);
}

void FragmentAttributesBuffer::SetAtIndex(const size_t index, const FragmentAttributes &attr)
{
	this->depth[index]				= attr.depth;
	this->opaquePolyID[index]		= attr.opaquePolyID;
	this->translucentPolyID[index]	= attr.translucentPolyID;
	this->stencil[index]			= attr.stencil;
	this->isFogged[index]			= attr.isFogged;
	this->isTranslucentPoly[index]	= attr.isTranslucentPoly;
}

void FragmentAttributesBuffer::SetAll(const FragmentAttributes &attr)
{
	size_t i = 0;
	
#ifdef ENABLE_SSE2
	const __m128i attrDepth_vec128				= _mm_set1_epi32(attr.depth);
	const __m128i attrOpaquePolyID_vec128		= _mm_set1_epi8(attr.opaquePolyID);
	const __m128i attrTranslucentPolyID_vec128	= _mm_set1_epi8(attr.translucentPolyID);
	const __m128i attrStencil_vec128			= _mm_set1_epi8(attr.stencil);
	const __m128i attrIsFogged_vec128			= _mm_set1_epi8(attr.isFogged);
	const __m128i attrIsTranslucentPoly_vec128	= _mm_set1_epi8(attr.isTranslucentPoly);
	
	const size_t sseCount = count - (count % 16);
	for (; i < sseCount; i += 16)
	{
		_mm_stream_si128((__m128i *)(this->depth +  0), attrDepth_vec128);
		_mm_stream_si128((__m128i *)(this->depth +  4), attrDepth_vec128);
		_mm_stream_si128((__m128i *)(this->depth +  8), attrDepth_vec128);
		_mm_stream_si128((__m128i *)(this->depth + 12), attrDepth_vec128);
		
		_mm_stream_si128((__m128i *)this->opaquePolyID, attrOpaquePolyID_vec128);
		_mm_stream_si128((__m128i *)this->translucentPolyID, attrTranslucentPolyID_vec128);
		_mm_stream_si128((__m128i *)this->stencil, attrStencil_vec128);
		_mm_stream_si128((__m128i *)this->isFogged, attrIsFogged_vec128);
		_mm_stream_si128((__m128i *)this->isTranslucentPoly, attrIsTranslucentPoly_vec128);
	}
#endif
	
	for (; i < count; i++)
	{
		this->SetAtIndex(i, attr);
	}
}

void* Render3D::operator new(size_t size)
{
	void *newPtr = malloc_alignedCacheLine(size);
	if (newPtr == NULL)
	{
		#ifdef _3DS
			exit(1);
		#else
			throw std::bad_alloc();
		#endif
	}
	
	return newPtr;
}

void Render3D::operator delete(void *ptr)
{
	free_aligned(ptr);
}

Render3D::Render3D()
{
	_deviceInfo.renderID = RENDERID_NULL;
	_deviceInfo.renderName = "None";
	_deviceInfo.isTexturingSupported = false;
	_deviceInfo.isEdgeMarkSupported = false;
	_deviceInfo.isFogSupported = false;
	_deviceInfo.isTextureSmoothingSupported = false;
	_deviceInfo.maxAnisotropy = 1.0f;
	_deviceInfo.maxSamples = 0;
	
	static bool needTableInit = true;
	
	if (needTableInit)
	{
		for (size_t i = 0; i < 32768; i++)
		{
			dsDepthToD24_LUT[i] = (u32)DS_DEPTH15TO24(i);
		}
		
		needTableInit = false;
	}
	
	_framebufferWidth = GPU_FRAMEBUFFER_NATIVE_WIDTH;
	_framebufferHeight = GPU_FRAMEBUFFER_NATIVE_HEIGHT;
	_framebufferColorSizeBytes = 0;
	_framebufferColor = NULL;
	
	_internalRenderingFormat = NDSColorFormat_BGR666_Rev;
	_outputFormat = NDSColorFormat_BGR666_Rev;
	_renderNeedsFinish = false;
	_willFlushFramebufferRGBA6665 = true;
	_willFlushFramebufferRGBA5551 = true;
	
	_textureScalingFactor = 1;
	_textureSmooth = false;
	_textureDeposterizeBuffer = NULL;
	_textureUpscaleBuffer = NULL;
	
	Reset();
}

Render3D::~Render3D()
{
	// Do nothing.
}

const Render3DDeviceInfo& Render3D::GetDeviceInfo()
{
	return this->_deviceInfo;
}

RendererID Render3D::GetRenderID()
{
	return this->_deviceInfo.renderID;
}

std::string Render3D::GetName()
{
	return this->_deviceInfo.renderName;
}

FragmentColor* Render3D::GetFramebuffer()
{
	return this->_framebufferColor;
}

size_t Render3D::GetFramebufferWidth()
{
	return this->_framebufferWidth;
}

size_t Render3D::GetFramebufferHeight()
{
	return this->_framebufferHeight;
}

bool Render3D::IsFramebufferNativeSize()
{
	return ( (this->_framebufferWidth == GPU_FRAMEBUFFER_NATIVE_WIDTH) && (this->_framebufferHeight == GPU_FRAMEBUFFER_NATIVE_HEIGHT) );
}

Render3DError Render3D::SetFramebufferSize(size_t w, size_t h)
{
	if (w < GPU_FRAMEBUFFER_NATIVE_WIDTH || h < GPU_FRAMEBUFFER_NATIVE_HEIGHT)
	{
		return RENDER3DERROR_NOERR;
	}
	
	this->_framebufferWidth = w;
	this->_framebufferHeight = h;
	this->_framebufferColorSizeBytes = w * h * sizeof(FragmentColor);
	this->_framebufferColor = GPU->GetEngineMain()->Get3DFramebufferRGBA6665(); // Just use the buffer that is already present on the main GPU engine
	
	return RENDER3DERROR_NOERR;
}

NDSColorFormat Render3D::RequestColorFormat(NDSColorFormat colorFormat)
{
	this->_outputFormat = (colorFormat == NDSColorFormat_BGR555_Rev) ? NDSColorFormat_BGR666_Rev : colorFormat;
	return this->_outputFormat;
}

NDSColorFormat Render3D::GetColorFormat() const
{
	return this->_outputFormat;
}

void Render3D::GetFramebufferFlushStates(bool &willFlushRGBA6665, bool &willFlushRGBA5551)
{
	willFlushRGBA6665 = this->_willFlushFramebufferRGBA6665;
	willFlushRGBA5551 = this->_willFlushFramebufferRGBA5551;
}

void Render3D::SetFramebufferFlushStates(bool willFlushRGBA6665, bool willFlushRGBA5551)
{
	this->_willFlushFramebufferRGBA6665 = willFlushRGBA6665;
	this->_willFlushFramebufferRGBA5551 = willFlushRGBA5551;
}

bool Render3D::GetRenderNeedsFinish() const
{
	return this->_renderNeedsFinish;
}

void Render3D::SetRenderNeedsFinish(const bool renderNeedsFinish)
{
	this->_renderNeedsFinish = renderNeedsFinish;
}

void Render3D::SetTextureProcessingProperties(size_t scalingFactor, bool willDeposterize, bool willSmooth)
{
	const bool isScaleValid = ( (scalingFactor == 2) || (scalingFactor == 4) );
	const size_t newScalingFactor = (isScaleValid) ? scalingFactor : 1;
	bool needTexCacheReset = false;
	
	if ( willDeposterize && (this->_textureDeposterizeBuffer == NULL) )
	{
		// 1024x1024 texels is the largest possible texture size.
		// We need two buffers, one for each deposterize stage.
		const size_t bufferSize = 1024 * 1024 * 2 * sizeof(u32);
		this->_textureDeposterizeBuffer = (u32 *)malloc_alignedCacheLine(bufferSize);
		memset(this->_textureDeposterizeBuffer, 0, bufferSize);
		
		needTexCacheReset = true;
	}
	else if ( !willDeposterize && (this->_textureDeposterizeBuffer != NULL) )
	{
		free_aligned(this->_textureDeposterizeBuffer);
		this->_textureDeposterizeBuffer = NULL;
		
		needTexCacheReset = true;
	}
	
	if (newScalingFactor != this->_textureScalingFactor)
	{
		u32 *oldTextureBuffer = this->_textureUpscaleBuffer;
		u32 *newTextureBuffer = (u32 *)malloc_alignedCacheLine( (1024 * newScalingFactor) * (1024 * newScalingFactor) * sizeof(u32) );
		this->_textureScalingFactor = newScalingFactor;
		this->_textureUpscaleBuffer = newTextureBuffer;
		free_aligned(oldTextureBuffer);
		
		needTexCacheReset = true;
	}
	
	if (willSmooth != this->_textureSmooth)
	{
		this->_textureSmooth = willSmooth;
		
		needTexCacheReset = true;
	}
	
	if (needTexCacheReset)
	{
		TexCache_Reset();
	}
}

Render3DError Render3D::TextureDeposterize(const u32 *src, const size_t srcTexWidth, const size_t srcTexHeight)
{
	//---------------------------------------\n\
	// Input Pixel Mapping:  06|07|08
	//                       05|00|01
	//                       04|03|02
	//
	// Output Pixel Mapping:    00
	
	const int w = srcTexWidth;
	const int h = srcTexHeight;
	
	u32 color[9];
	u32 blend[9];
	u32 *dst = this->_textureDeposterizeBuffer + (1024 * 1024);
	u32 *finalDst = this->_textureDeposterizeBuffer;
	
	size_t i = 0;
	for (int y = 0; y < h; y++)
	{
		for (int x = 0; x < w; x++, i++)
		{
			if ((src[i] & 0xFF000000) == 0)
			{
				dst[i] = src[i];
				continue;
			}
			
			color[0] = src[i];
			color[1] =  (x < w-1)				? src[i+1]   : src[i];
			color[2] = ((x < w-1) && (y < h-1))	? src[i+w+1] : src[i];
			color[3] =               (y < h-1)	? src[i+w]   : src[i];
			color[4] = ((x > 0)   && (y < h-1))	? src[i+w-1] : src[i];
			color[5] =  (x > 0)					? src[i-1]   : src[i];
			color[6] = ((x > 0)   && (y > 0))	? src[i-w-1] : src[i];
			color[7] =               (y > 0)	? src[i-w]   : src[i];
			color[8] = ((x < w-1) && (y > 0))	? src[i-w+1] : src[i];
			
			blend[0] = color[0];
			blend[1] = TextureDeposterize_InterpLTE(color[0], color[1], TEXTURE_DEPOSTERIZE_THRESHOLD);
			blend[2] = TextureDeposterize_InterpLTE(color[0], color[2], TEXTURE_DEPOSTERIZE_THRESHOLD);
			blend[3] = TextureDeposterize_InterpLTE(color[0], color[3], TEXTURE_DEPOSTERIZE_THRESHOLD);
			blend[4] = TextureDeposterize_InterpLTE(color[0], color[4], TEXTURE_DEPOSTERIZE_THRESHOLD);
			blend[5] = TextureDeposterize_InterpLTE(color[0], color[5], TEXTURE_DEPOSTERIZE_THRESHOLD);
			blend[6] = TextureDeposterize_InterpLTE(color[0], color[6], TEXTURE_DEPOSTERIZE_THRESHOLD);
			blend[7] = TextureDeposterize_InterpLTE(color[0], color[7], TEXTURE_DEPOSTERIZE_THRESHOLD);
			blend[8] = TextureDeposterize_InterpLTE(color[0], color[8], TEXTURE_DEPOSTERIZE_THRESHOLD);
			
			dst[i] = TextureDeposterize_Blend(TextureDeposterize_Blend(TextureDeposterize_Blend(TextureDeposterize_Blend(blend[0], blend[5], 1, 7),
																								TextureDeposterize_Blend(blend[0], blend[1], 1, 7),
																								1, 1),
																	   TextureDeposterize_Blend(TextureDeposterize_Blend(blend[0], blend[7], 1, 7),
																								TextureDeposterize_Blend(blend[0], blend[3], 1, 7),
																								1, 1),
																	   1, 1),
											  TextureDeposterize_Blend(TextureDeposterize_Blend(TextureDeposterize_Blend(blend[0], blend[6], 7, 9),
																								TextureDeposterize_Blend(blend[0], blend[2], 7, 9),
																								1, 1),
																	   TextureDeposterize_Blend(TextureDeposterize_Blend(blend[0], blend[8], 7, 9),
																								TextureDeposterize_Blend(blend[0], blend[4], 7, 9),
																								1, 1),
																	   1, 1),
											  3, 1);
		}
	}
	
	i = 0;
	for (int y = 0; y < h; y++)
	{
		for (int x = 0; x < w; x++, i++)
		{
			if ((src[i] & 0xFF000000) == 0)
			{
				finalDst[i] = src[i];
				continue;
			}
			
			color[0] = dst[i];
			color[1] =  (x < w-1)				? dst[i+1]   : dst[i];
			color[2] = ((x < w-1) && (y < h-1))	? dst[i+w+1] : dst[i];
			color[3] =               (y < h-1)	? dst[i+w]   : dst[i];
			color[4] = ((x > 0)   && (y < h-1))	? dst[i+w-1] : dst[i];
			color[5] =  (x > 0)					? dst[i-1]   : dst[i];
			color[6] = ((x > 0)   && (y > 0))	? dst[i-w-1] : dst[i];
			color[7] =               (y > 0)	? dst[i-w]   : dst[i];
			color[8] = ((x < w-1) && (y > 0))	? dst[i-w+1] : dst[i];
			
			blend[0] = color[0];
			blend[1] = TextureDeposterize_InterpLTE(color[0], color[1], TEXTURE_DEPOSTERIZE_THRESHOLD);
			blend[2] = TextureDeposterize_InterpLTE(color[0], color[2], TEXTURE_DEPOSTERIZE_THRESHOLD);
			blend[3] = TextureDeposterize_InterpLTE(color[0], color[3], TEXTURE_DEPOSTERIZE_THRESHOLD);
			blend[4] = TextureDeposterize_InterpLTE(color[0], color[4], TEXTURE_DEPOSTERIZE_THRESHOLD);
			blend[5] = TextureDeposterize_InterpLTE(color[0], color[5], TEXTURE_DEPOSTERIZE_THRESHOLD);
			blend[6] = TextureDeposterize_InterpLTE(color[0], color[6], TEXTURE_DEPOSTERIZE_THRESHOLD);
			blend[7] = TextureDeposterize_InterpLTE(color[0], color[7], TEXTURE_DEPOSTERIZE_THRESHOLD);
			blend[8] = TextureDeposterize_InterpLTE(color[0], color[8], TEXTURE_DEPOSTERIZE_THRESHOLD);
			
			finalDst[i] = TextureDeposterize_Blend(TextureDeposterize_Blend(TextureDeposterize_Blend(TextureDeposterize_Blend(blend[0], blend[5], 1, 7),
																									 TextureDeposterize_Blend(blend[0], blend[1], 1, 7),
																									 1, 1),
																			TextureDeposterize_Blend(TextureDeposterize_Blend(blend[0], blend[7], 1, 7),
																									 TextureDeposterize_Blend(blend[0], blend[3], 1, 7),
																									 1, 1),
																			1, 1),
												   TextureDeposterize_Blend(TextureDeposterize_Blend(TextureDeposterize_Blend(blend[0], blend[6], 7, 9),
																									 TextureDeposterize_Blend(blend[0], blend[2], 7, 9),
																									 1, 1),
																			TextureDeposterize_Blend(TextureDeposterize_Blend(blend[0], blend[8], 7, 9),
																									 TextureDeposterize_Blend(blend[0], blend[4], 7, 9),
																									 1, 1),
																			1, 1),
												   3, 1);
		}
	}
	
	return RENDER3DERROR_NOERR;
}

template <size_t SCALEFACTOR>
Render3DError Render3D::TextureUpscale(const u32 *src, size_t &outTexWidth, size_t &outTexHeight)
{
	if ( (SCALEFACTOR != 2) && (SCALEFACTOR != 4) )
	{
		return RENDER3DERROR_NOERR;
	}
	
	//xbrz::scale<SCALEFACTOR, xbrz::ColorFormatARGB>(src, this->_textureUpscaleBuffer, outTexWidth, outTexHeight);
	
	//outTexWidth *= SCALEFACTOR;
	//outTexHeight *= SCALEFACTOR;
	return RENDER3DERROR_NOERR;
}

Render3DError Render3D::BeginRender(const GFX3D &engine)
{
	return RENDER3DERROR_NOERR;
}

Render3DError Render3D::RenderGeometry(const GFX3D_State &renderState, const POLYLIST *polyList, const INDEXLIST *indexList)
{
	return RENDER3DERROR_NOERR;
}

Render3DError Render3D::RenderEdgeMarking(const u16 *colorTable, const bool useAntialias)
{
	return RENDER3DERROR_NOERR;
}

Render3DError Render3D::RenderFog(const u8 *densityTable, const u32 color, const u32 offset, const u8 shift, const bool alphaOnly)
{
	return RENDER3DERROR_NOERR;
}

Render3DError Render3D::EndRender(const u64 frameCount)
{
	return RENDER3DERROR_NOERR;
}

Render3DError Render3D::FlushFramebuffer(const FragmentColor *__restrict srcFramebuffer, FragmentColor *__restrict dstFramebuffer, u16 *__restrict dstRGBA5551)
{
	if ( (dstFramebuffer == NULL) && (dstRGBA5551 == NULL) )
	{
		return RENDER3DERROR_NOERR;
	}
	
	const size_t pixCount = this->_framebufferWidth * this->_framebufferHeight;
	
	if (dstFramebuffer != NULL)
	{
		if ( (this->_internalRenderingFormat == NDSColorFormat_BGR888_Rev) && (this->_outputFormat == NDSColorFormat_BGR666_Rev) )
		{
			for (size_t i = 0; i < pixCount; i++)
			{
				dstFramebuffer[i].r = srcFramebuffer[i].r >> 2;
				dstFramebuffer[i].g = srcFramebuffer[i].g >> 2;
				dstFramebuffer[i].b = srcFramebuffer[i].b >> 2;
				dstFramebuffer[i].a = srcFramebuffer[i].a >> 3;
			}
		}
		else if ( (this->_internalRenderingFormat == NDSColorFormat_BGR666_Rev) && (this->_outputFormat == NDSColorFormat_BGR888_Rev) )
		{
			for (size_t i = 0; i < pixCount; i++)
			{
				dstFramebuffer[i].r = material_6bit_to_8bit[srcFramebuffer[i].r];
				dstFramebuffer[i].g = material_6bit_to_8bit[srcFramebuffer[i].g];
				dstFramebuffer[i].b = material_6bit_to_8bit[srcFramebuffer[i].b];
				dstFramebuffer[i].a = material_5bit_to_8bit[srcFramebuffer[i].a];
			}
		}
		else if ( ((this->_internalRenderingFormat == NDSColorFormat_BGR666_Rev) && (this->_outputFormat == NDSColorFormat_BGR666_Rev)) ||
		          ((this->_internalRenderingFormat == NDSColorFormat_BGR888_Rev) && (this->_outputFormat == NDSColorFormat_BGR888_Rev)) )
		{
			memcpy(dstFramebuffer, srcFramebuffer, pixCount * sizeof(FragmentColor));
		}
	}
	
	if (dstRGBA5551 != NULL)
	{
		for (size_t i = 0; i < pixCount; i++)
		{
			dstRGBA5551[i] = R6G6B6TORGB15(srcFramebuffer[i].r, srcFramebuffer[i].g, srcFramebuffer[i].b) | ((srcFramebuffer[i].a == 0) ? 0x0000 : 0x8000);
		}
	}
	
	return RENDER3DERROR_NOERR;
}

Render3DError Render3D::UpdateToonTable(const u16 *toonTableBuffer)
{
	return RENDER3DERROR_NOERR;
}

Render3DError Render3D::ClearFramebuffer(const GFX3D_State &renderState)
{
	Render3DError error = RENDER3DERROR_NOERR;
	
	FragmentColor clearColor;
	
#ifdef LOCAL_LE
	clearColor.r =  renderState.clearColor & 0x1F;
	clearColor.g = (renderState.clearColor >> 5) & 0x1F;
	clearColor.b = (renderState.clearColor >> 10) & 0x1F;
	clearColor.a = (renderState.clearColor >> 16) & 0x1F;
#else
	const u32 clearColorSwapped = LE_TO_LOCAL_32(renderState.clearColor);
	clearColor.r =  clearColorSwapped & 0x1F;
	clearColor.g = (clearColorSwapped >> 5) & 0x1F;
	clearColor.b = (clearColorSwapped >> 10) & 0x1F;
	clearColor.a = (clearColorSwapped >> 16) & 0x1F;
#endif
	
	FragmentAttributes clearFragment;
	clearFragment.opaquePolyID = (renderState.clearColor >> 24) & 0x3F;
	//special value for uninitialized translucent polyid. without this, fires in spiderman2 dont display
	//I am not sure whether it is right, though. previously this was cleared to 0, as a guess,
	//but in spiderman2 some fires with polyid 0 try to render on top of the background
	clearFragment.translucentPolyID = kUnsetTranslucentPolyID;
	clearFragment.depth = renderState.clearDepth;
	clearFragment.stencil = 0;
	clearFragment.isTranslucentPoly = 0;
	clearFragment.isFogged = BIT15(renderState.clearColor);
	
	if (renderState.enableClearImage)
	{
		//the lion, the witch, and the wardrobe (thats book 1, suck it you new-school numberers)
		//uses the scroll registers in the main game engine
		const u16 *__restrict clearColorBuffer = (u16 *__restrict)MMU.texInfo.textureSlotAddr[2];
		const u16 *__restrict clearDepthBuffer = (u16 *__restrict)MMU.texInfo.textureSlotAddr[3];
		const u16 scrollBits = T1ReadWord(MMU.ARM9_REG, 0x356); //CLRIMAGE_OFFSET
		const u8 xScroll = scrollBits & 0xFF;
		const u8 yScroll = (scrollBits >> 8) & 0xFF;
		
		if (xScroll == 0 && yScroll == 0)
		{
			for (size_t i = 0; i < GPU_FRAMEBUFFER_NATIVE_WIDTH * GPU_FRAMEBUFFER_NATIVE_HEIGHT; i++)
			{
				this->clearImageColor16Buffer[i] = clearColorBuffer[i];
				this->clearImageDepthBuffer[i] = dsDepthToD24_LUT[clearDepthBuffer[i] & 0x7FFF];
				this->clearImageFogBuffer[i] = BIT15(clearDepthBuffer[i]);
				this->clearImagePolyIDBuffer[i] = clearFragment.opaquePolyID;
			}
		}
		else
		{
			for (size_t dstIndex = 0, iy = 0; iy < GPU_FRAMEBUFFER_NATIVE_HEIGHT; iy++)
			{
				const size_t y = ((iy + yScroll) & 0xFF) << 8;
				
				for (size_t ix = 0; ix < GPU_FRAMEBUFFER_NATIVE_WIDTH; dstIndex++, ix++)
				{
					const size_t x = (ix + xScroll) & 0xFF;
					const size_t srcIndex = y | x;
					
					//this is tested by harry potter and the order of the phoenix.
					//TODO (optimization) dont do this if we are mapped to blank memory (such as in sonic chronicles)
					//(or use a special zero fill in the bulk clearing above)
					this->clearImageColor16Buffer[dstIndex] = clearColorBuffer[srcIndex];
					
					//this is tested quite well in the sonic chronicles main map mode
					//where depth values are used for trees etc you can walk behind
					this->clearImageDepthBuffer[dstIndex] = dsDepthToD24_LUT[clearDepthBuffer[srcIndex] & 0x7FFF];
					
					this->clearImageFogBuffer[dstIndex] = BIT15(clearDepthBuffer[srcIndex]);
					this->clearImagePolyIDBuffer[dstIndex] = clearFragment.opaquePolyID;
				}
			}
		}
		
		error = this->ClearUsingImage(this->clearImageColor16Buffer, this->clearImageDepthBuffer, this->clearImageFogBuffer, this->clearImagePolyIDBuffer);
		if (error != RENDER3DERROR_NOERR)
		{
			error = this->ClearUsingValues(clearColor, clearFragment);
		}
	}
	else
	{
		error = this->ClearUsingValues(clearColor, clearFragment);
	}
	
	return error;
}

Render3DError Render3D::ClearUsingImage(const u16 *__restrict colorBuffer, const u32 *__restrict depthBuffer, const u8 *__restrict fogBuffer, const u8 *__restrict polyIDBuffer)
{
	return RENDER3DERROR_NOERR;
}

Render3DError Render3D::ClearUsingValues(const FragmentColor &clearColor, const FragmentAttributes &clearAttributes) const
{
	return RENDER3DERROR_NOERR;
}

Render3DError Render3D::SetupPolygon(const POLY &thePoly)
{
	return RENDER3DERROR_NOERR;
}

Render3DError Render3D::SetupTexture(const POLY &thePoly, bool enableTexturing)
{
	return RENDER3DERROR_NOERR;
}

Render3DError Render3D::SetupViewport(const u32 viewportValue)
{
	return RENDER3DERROR_NOERR;
}

Render3DError Render3D::Reset()
{
	if (this->_framebufferColor != NULL)
	{
		memset(this->_framebufferColor, 0, this->_framebufferColorSizeBytes);
	}
	
	memset(this->clearImageColor16Buffer, 0, sizeof(this->clearImageColor16Buffer));
	memset(this->clearImageDepthBuffer, 0, sizeof(this->clearImageDepthBuffer));
	memset(this->clearImagePolyIDBuffer, 0, sizeof(this->clearImagePolyIDBuffer));
	memset(this->clearImageFogBuffer, 0, sizeof(this->clearImageFogBuffer));
	
	this->_willFlushFramebufferRGBA6665 = true;
	this->_willFlushFramebufferRGBA5551 = true;
	
	TexCache_Reset();
	
	return RENDER3DERROR_NOERR;
}

Render3DError Render3D::Render(const GFX3D &engine)
{
	Render3DError error = RENDER3DERROR_NOERR;
	
	error = this->BeginRender(engine);
	if (error != RENDER3DERROR_NOERR)
	{
		return error;
	}
	
	this->UpdateToonTable(engine.renderState.u16ToonTable);
	this->ClearFramebuffer(engine.renderState);
	
	this->RenderGeometry(engine.renderState, engine.polylist, &engine.indexlist);
	
	if (engine.renderState.enableEdgeMarking)
	{
		this->RenderEdgeMarking(engine.renderState.edgeMarkColorTable, engine.renderState.enableAntialiasing);
	}
	
	if (engine.renderState.enableFog)
	{
		this->RenderFog(engine.renderState.fogDensityTable, engine.renderState.fogColor, engine.renderState.fogOffset, engine.renderState.fogShift, engine.renderState.enableFogAlphaOnly);
	}

	this->EndRender(engine.render3DFrameCount);
	
	return error;
}

Render3DError Render3D::RenderFinish()
{
	return RENDER3DERROR_NOERR;
}

Render3DError Render3D::VramReconfigureSignal()
{
	TexCache_Invalidate();	
	return RENDER3DERROR_NOERR;
}

#ifdef ENABLE_SSE2

Render3DError Render3D_SSE2::FlushFramebuffer(const FragmentColor *__restrict srcFramebuffer, FragmentColor *__restrict dstFramebuffer, u16 *__restrict dstRGBA5551)
{
	if ( (dstFramebuffer == NULL) && (dstRGBA5551 == NULL) )
	{
		return RENDER3DERROR_NOERR;
	}
	
	size_t i = 0;
	const size_t pixCount = this->_framebufferWidth * this->_framebufferHeight;
	const size_t ssePixCount = pixCount - (pixCount % 4);
	
	if (dstFramebuffer != NULL)
	{
		if ( (this->_internalRenderingFormat == NDSColorFormat_BGR888_Rev) && (this->_outputFormat == NDSColorFormat_BGR666_Rev) )
		{
			for (; i < ssePixCount; i += 4)
			{
				// Convert to RGBA6665
				__m128i color6665 = _mm_load_si128((__m128i *)(srcFramebuffer + i));
				__m128i a = _mm_srli_epi32(_mm_and_si128(color6665, _mm_set1_epi32(0xF8000000)), 3);
				color6665 = _mm_srli_epi32(_mm_and_si128(color6665, _mm_set1_epi32(0x00FCFCFC)), 2);
				
				color6665 = _mm_or_si128(color6665, a);
				_mm_store_si128((__m128i *)(dstFramebuffer + i), color6665);
			}
			
			for (; i < pixCount; i++)
			{
				dstFramebuffer[i].r = srcFramebuffer[i].r >> 2;
				dstFramebuffer[i].g = srcFramebuffer[i].g >> 2;
				dstFramebuffer[i].b = srcFramebuffer[i].b >> 2;
				dstFramebuffer[i].a = srcFramebuffer[i].a >> 3;
			}
		}
		else if ( (this->_internalRenderingFormat == NDSColorFormat_BGR666_Rev) && (this->_outputFormat == NDSColorFormat_BGR888_Rev) )
		{
			for (; i < ssePixCount; i += 4)
			{
				// Convert to RGBA8888:
				//    RGB   6-bit to 8-bit formula: dstRGB8 = (srcRGB6 << 2) | ((srcRGB6 >> 4) & 0x03)
				//    Alpha 5-bit to 8-bit formula: dstA8   = (srcA5   << 3) | ((srcA5   >> 2) & 0x07)
				__m128i color8888 = _mm_load_si128((__m128i *)(srcFramebuffer + i));
				__m128i a = _mm_or_si128( _mm_and_si128(_mm_slli_epi32(color8888, 3), _mm_set1_epi8(0xF8)), _mm_and_si128(_mm_srli_epi32(color8888, 2), _mm_set1_epi8(0x07)) );
				color8888 = _mm_or_si128( _mm_and_si128(_mm_slli_epi32(color8888, 2), _mm_set1_epi8(0xFC)), _mm_and_si128(_mm_srli_epi32(color8888, 4), _mm_set1_epi8(0x03)) );
				
				color8888 = _mm_or_si128(_mm_and_si128(color8888, _mm_set1_epi32(0x00FFFFFF)), _mm_and_si128(a, _mm_set1_epi32(0xFF000000)));
				_mm_store_si128((__m128i *)(dstFramebuffer + i), color8888);
			}
			
			for (; i < pixCount; i++)
			{
				dstFramebuffer[i].r = material_6bit_to_8bit[srcFramebuffer[i].r];
				dstFramebuffer[i].g = material_6bit_to_8bit[srcFramebuffer[i].g];
				dstFramebuffer[i].b = material_6bit_to_8bit[srcFramebuffer[i].b];
				dstFramebuffer[i].a = material_5bit_to_8bit[srcFramebuffer[i].a];
			}
		}
		else if ( ((this->_internalRenderingFormat == NDSColorFormat_BGR666_Rev) && (this->_outputFormat == NDSColorFormat_BGR666_Rev)) ||
		          ((this->_internalRenderingFormat == NDSColorFormat_BGR888_Rev) && (this->_outputFormat == NDSColorFormat_BGR888_Rev)) )
		{
			memcpy(dstFramebuffer, srcFramebuffer, pixCount * sizeof(FragmentColor));
		}
	}
	
	if (dstRGBA5551 != NULL)
	{
		for (; i < ssePixCount; i += 4)
		{
			// Convert to RGBA5551
			__m128i color5551 = _mm_load_si128((__m128i *)(srcFramebuffer + i));
			__m128i r = _mm_and_si128(color5551, _mm_set1_epi32(0x0000003E));	// Read from R
			r = _mm_srli_epi32(r, 1);											// Shift to R
			
			__m128i g = _mm_and_si128(color5551, _mm_set1_epi32(0x00003E00));	// Read from G
			g = _mm_srli_epi32(g, 4);											// Shift in G
			
			__m128i b = _mm_and_si128(color5551, _mm_set1_epi32(0x003E0000));	// Read from B
			b = _mm_srli_epi32(b, 7);											// Shift to B
			
			__m128i a = _mm_and_si128(color5551, _mm_set1_epi32(0xFF000000));	// Read from A
			a = _mm_cmpeq_epi32(a, _mm_setzero_si128());						// Determine A
			
			// From here on, we're going to do an SSE2 trick to pack 32-bit down to unsigned
			// 16-bit. Since SSE2 only has packssdw (signed saturated 16-bit pack), using
			// packssdw on the alpha bit (0x8000) will result in a value of 0x7FFF, which is
			// incorrect. Now if we were to use SSE4.1's packusdw (unsigned saturated 16-bit
			// pack), we  wouldn't have to go through this hassle. But not everyone has an
			// SSE4.1-capable CPU, so doing this the SSE2 way is more guaranteed to work for
			// everyone's CPU.
			//
			// To use packssdw, we take a bit one position lower for the alpha bit, run
			// packssdw, then shift the bit back to its original position. Then we por the
			// alpha vector with the post-packed color vector to get the final color.
			
			a = _mm_andnot_si128(a, _mm_set1_epi32(0x00004000));				// Mask out the bit before A
			a = _mm_packs_epi32(a, _mm_setzero_si128());						// Pack 32-bit down to 16-bit
			a = _mm_slli_epi16(a, 1);											// Shift the A bit back to where it needs to be
			
			// Assemble the RGB colors, pack the 32-bit color into a signed 16-bit color, then por the alpha bit back in.
			color5551 = _mm_or_si128(_mm_or_si128(r, g), b);
			color5551 = _mm_packs_epi32(color5551, _mm_setzero_si128());
			color5551 = _mm_or_si128(color5551, a);
			
			_mm_storel_epi64((__m128i *)(dstRGBA5551 + i), color5551);
		}
		
		for (; i < pixCount; i++)
		{
			dstRGBA5551[i] = R6G6B6TORGB15(srcFramebuffer[i].r, srcFramebuffer[i].g, srcFramebuffer[i].b) | ((srcFramebuffer[i].a == 0) ? 0x0000 : 0x8000);
		}
	}
	
	return RENDER3DERROR_NOERR;
}

Render3DError Render3D_SSE2::ClearFramebuffer(const GFX3D_State &renderState)
{
	Render3DError error = RENDER3DERROR_NOERR;
	
	FragmentColor clearColor;
	clearColor.r =  renderState.clearColor & 0x1F;
	clearColor.g = (renderState.clearColor >> 5) & 0x1F;
	clearColor.b = (renderState.clearColor >> 10) & 0x1F;
	clearColor.a = (renderState.clearColor >> 16) & 0x1F;
	
	FragmentAttributes clearFragment;
	clearFragment.opaquePolyID = (renderState.clearColor >> 24) & 0x3F;
	//special value for uninitialized translucent polyid. without this, fires in spiderman2 dont display
	//I am not sure whether it is right, though. previously this was cleared to 0, as a guess,
	//but in spiderman2 some fires with polyid 0 try to render on top of the background
	clearFragment.translucentPolyID = kUnsetTranslucentPolyID;
	clearFragment.depth = renderState.clearDepth;
	clearFragment.stencil = 0;
	clearFragment.isTranslucentPoly = 0;
	clearFragment.isFogged = BIT15(renderState.clearColor);
	
	if (renderState.enableClearImage)
	{
		//the lion, the witch, and the wardrobe (thats book 1, suck it you new-school numberers)
		//uses the scroll registers in the main game engine
		const u16 *__restrict clearColorBuffer = (u16 *__restrict)MMU.texInfo.textureSlotAddr[2];
		const u16 *__restrict clearDepthBuffer = (u16 *__restrict)MMU.texInfo.textureSlotAddr[3];
		const u16 scrollBits = T1ReadWord(MMU.ARM9_REG, 0x356); //CLRIMAGE_OFFSET
		const u8 xScroll = scrollBits & 0xFF;
		const u8 yScroll = (scrollBits >> 8) & 0xFF;
				
		if (xScroll == 0 && yScroll == 0)
		{
			const __m128i depthBitMask_vec128 = _mm_set1_epi16(0x7FFF);
			const __m128i fogBufferBitMask_vec128 = _mm_set1_epi16(BIT(15));
			const __m128i opaquePolyID_vec128 = _mm_set1_epi8(clearFragment.opaquePolyID);
			
			for (size_t i = 0; i < GPU_FRAMEBUFFER_NATIVE_WIDTH * GPU_FRAMEBUFFER_NATIVE_HEIGHT; i += 16)
			{
				// Copy the colors to the color buffer. Since we can only copy 8 elements at once,
				// we need to load-store twice.
				_mm_store_si128( (__m128i *)(this->clearImageColor16Buffer + i + 8), _mm_load_si128((__m128i *)(clearColorBuffer + i + 8)) );
				_mm_store_si128( (__m128i *)(this->clearImageColor16Buffer + i), _mm_load_si128((__m128i *)(clearColorBuffer + i)) );
				
				// Write the depth values to the depth buffer.
				__m128i clearDepthHi_vec128 = _mm_load_si128((__m128i *)(clearDepthBuffer + i + 8));
				__m128i clearDepthLo_vec128 = _mm_load_si128((__m128i *)(clearDepthBuffer + i));
				clearDepthHi_vec128 = _mm_and_si128(clearDepthHi_vec128, depthBitMask_vec128);
				clearDepthLo_vec128 = _mm_and_si128(clearDepthLo_vec128, depthBitMask_vec128);
				
				this->clearImageDepthBuffer[i+15] = dsDepthToD24_LUT[_mm_extract_epi16(clearDepthHi_vec128, 7)];
				this->clearImageDepthBuffer[i+14] = dsDepthToD24_LUT[_mm_extract_epi16(clearDepthHi_vec128, 6)];
				this->clearImageDepthBuffer[i+13] = dsDepthToD24_LUT[_mm_extract_epi16(clearDepthHi_vec128, 5)];
				this->clearImageDepthBuffer[i+12] = dsDepthToD24_LUT[_mm_extract_epi16(clearDepthHi_vec128, 4)];
				this->clearImageDepthBuffer[i+11] = dsDepthToD24_LUT[_mm_extract_epi16(clearDepthHi_vec128, 3)];
				this->clearImageDepthBuffer[i+10] = dsDepthToD24_LUT[_mm_extract_epi16(clearDepthHi_vec128, 2)];
				this->clearImageDepthBuffer[i+ 9] = dsDepthToD24_LUT[_mm_extract_epi16(clearDepthHi_vec128, 1)];
				this->clearImageDepthBuffer[i+ 8] = dsDepthToD24_LUT[_mm_extract_epi16(clearDepthHi_vec128, 0)];
				this->clearImageDepthBuffer[i+ 7] = dsDepthToD24_LUT[_mm_extract_epi16(clearDepthLo_vec128, 7)];
				this->clearImageDepthBuffer[i+ 6] = dsDepthToD24_LUT[_mm_extract_epi16(clearDepthLo_vec128, 6)];
				this->clearImageDepthBuffer[i+ 5] = dsDepthToD24_LUT[_mm_extract_epi16(clearDepthLo_vec128, 5)];
				this->clearImageDepthBuffer[i+ 4] = dsDepthToD24_LUT[_mm_extract_epi16(clearDepthLo_vec128, 4)];
				this->clearImageDepthBuffer[i+ 3] = dsDepthToD24_LUT[_mm_extract_epi16(clearDepthLo_vec128, 3)];
				this->clearImageDepthBuffer[i+ 2] = dsDepthToD24_LUT[_mm_extract_epi16(clearDepthLo_vec128, 2)];
				this->clearImageDepthBuffer[i+ 1] = dsDepthToD24_LUT[_mm_extract_epi16(clearDepthLo_vec128, 1)];
				this->clearImageDepthBuffer[i+ 0] = dsDepthToD24_LUT[_mm_extract_epi16(clearDepthLo_vec128, 0)];
				
				// Write the fog flags to the fog flag buffer.
				clearDepthHi_vec128 = _mm_load_si128((__m128i *)(clearDepthBuffer + i + 8));
				clearDepthLo_vec128 = _mm_load_si128((__m128i *)(clearDepthBuffer + i));
				clearDepthHi_vec128 = _mm_and_si128(clearDepthHi_vec128, fogBufferBitMask_vec128);
				clearDepthLo_vec128 = _mm_and_si128(clearDepthLo_vec128, fogBufferBitMask_vec128);
				clearDepthHi_vec128 = _mm_srli_epi16(clearDepthHi_vec128, 15);
				clearDepthLo_vec128 = _mm_srli_epi16(clearDepthLo_vec128, 15);
				
				_mm_store_si128((__m128i *)(this->clearImageFogBuffer + i), _mm_packus_epi16(clearDepthLo_vec128, clearDepthHi_vec128));
				
				// The one is easy. Just set the values in the polygon ID buffer.
				_mm_store_si128((__m128i *)(this->clearImagePolyIDBuffer + i), opaquePolyID_vec128);
			}
		}
		else
		{
			const __m128i addrOffset = _mm_set_epi16(7, 6, 5, 4, 3, 2, 1, 0);
			const __m128i addrRolloverMask = _mm_set1_epi16(0x00FF);
			const __m128i opaquePolyID_vec128 = _mm_set1_epi8(clearFragment.opaquePolyID);
			
			for (size_t dstIndex = 0, iy = 0; iy < GPU_FRAMEBUFFER_NATIVE_HEIGHT; iy++)
			{
				const size_t y = ((iy + yScroll) & 0xFF) << 8;
				__m128i y_vec128 = _mm_set1_epi16(y);
				
				for (size_t ix = 0; ix < GPU_FRAMEBUFFER_NATIVE_WIDTH; dstIndex += 8, ix += 8)
				{
					__m128i addr_vec128 = _mm_set1_epi16(ix + xScroll);
					addr_vec128 = _mm_add_epi16(addr_vec128, addrOffset);
					addr_vec128 = _mm_and_si128(addr_vec128, addrRolloverMask);
					addr_vec128 = _mm_or_si128(addr_vec128, y_vec128);
					
					this->clearImageColor16Buffer[dstIndex+7] = clearColorBuffer[_mm_extract_epi16(addr_vec128, 7)];
					this->clearImageColor16Buffer[dstIndex+6] = clearColorBuffer[_mm_extract_epi16(addr_vec128, 6)];
					this->clearImageColor16Buffer[dstIndex+5] = clearColorBuffer[_mm_extract_epi16(addr_vec128, 5)];
					this->clearImageColor16Buffer[dstIndex+4] = clearColorBuffer[_mm_extract_epi16(addr_vec128, 4)];
					this->clearImageColor16Buffer[dstIndex+3] = clearColorBuffer[_mm_extract_epi16(addr_vec128, 3)];
					this->clearImageColor16Buffer[dstIndex+2] = clearColorBuffer[_mm_extract_epi16(addr_vec128, 2)];
					this->clearImageColor16Buffer[dstIndex+1] = clearColorBuffer[_mm_extract_epi16(addr_vec128, 1)];
					this->clearImageColor16Buffer[dstIndex+0] = clearColorBuffer[_mm_extract_epi16(addr_vec128, 0)];
					
					this->clearImageDepthBuffer[dstIndex+7] = dsDepthToD24_LUT[clearDepthBuffer[_mm_extract_epi16(addr_vec128, 7)] & 0x7FFF];
					this->clearImageDepthBuffer[dstIndex+6] = dsDepthToD24_LUT[clearDepthBuffer[_mm_extract_epi16(addr_vec128, 6)] & 0x7FFF];
					this->clearImageDepthBuffer[dstIndex+5] = dsDepthToD24_LUT[clearDepthBuffer[_mm_extract_epi16(addr_vec128, 5)] & 0x7FFF];
					this->clearImageDepthBuffer[dstIndex+4] = dsDepthToD24_LUT[clearDepthBuffer[_mm_extract_epi16(addr_vec128, 4)] & 0x7FFF];
					this->clearImageDepthBuffer[dstIndex+3] = dsDepthToD24_LUT[clearDepthBuffer[_mm_extract_epi16(addr_vec128, 3)] & 0x7FFF];
					this->clearImageDepthBuffer[dstIndex+2] = dsDepthToD24_LUT[clearDepthBuffer[_mm_extract_epi16(addr_vec128, 2)] & 0x7FFF];
					this->clearImageDepthBuffer[dstIndex+1] = dsDepthToD24_LUT[clearDepthBuffer[_mm_extract_epi16(addr_vec128, 1)] & 0x7FFF];
					this->clearImageDepthBuffer[dstIndex+0] = dsDepthToD24_LUT[clearDepthBuffer[_mm_extract_epi16(addr_vec128, 0)] & 0x7FFF];
					
					this->clearImageFogBuffer[dstIndex+7] = BIT15( clearDepthBuffer[_mm_extract_epi16(addr_vec128, 7)] );
					this->clearImageFogBuffer[dstIndex+6] = BIT15( clearDepthBuffer[_mm_extract_epi16(addr_vec128, 6)] );
					this->clearImageFogBuffer[dstIndex+5] = BIT15( clearDepthBuffer[_mm_extract_epi16(addr_vec128, 5)] );
					this->clearImageFogBuffer[dstIndex+4] = BIT15( clearDepthBuffer[_mm_extract_epi16(addr_vec128, 4)] );
					this->clearImageFogBuffer[dstIndex+3] = BIT15( clearDepthBuffer[_mm_extract_epi16(addr_vec128, 3)] );
					this->clearImageFogBuffer[dstIndex+2] = BIT15( clearDepthBuffer[_mm_extract_epi16(addr_vec128, 2)] );
					this->clearImageFogBuffer[dstIndex+1] = BIT15( clearDepthBuffer[_mm_extract_epi16(addr_vec128, 1)] );
					this->clearImageFogBuffer[dstIndex+0] = BIT15( clearDepthBuffer[_mm_extract_epi16(addr_vec128, 0)] );
					
					_mm_storel_epi64((__m128i *)(this->clearImagePolyIDBuffer + dstIndex), opaquePolyID_vec128);
				}
			}
		}
		
		error = this->ClearUsingImage(this->clearImageColor16Buffer, this->clearImageDepthBuffer, this->clearImageFogBuffer, this->clearImagePolyIDBuffer);
		if (error != RENDER3DERROR_NOERR)
		{
			error = this->ClearUsingValues(clearColor, clearFragment);
		}
	}
	else
	{
		error = this->ClearUsingValues(clearColor, clearFragment);
	}
	
	return error;
}

#endif // ENABLE_SSE2

template Render3DError Render3D::TextureUpscale<2>(const u32 *src, size_t &outTexWidth, size_t &outTexHeight);
template Render3DError Render3D::TextureUpscale<4>(const u32 *src, size_t &outTexWidth, size_t &outTexHeight);
