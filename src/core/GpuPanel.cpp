#include "GpuPanel.h"

#include <windows.h>
#include <cstring>
#include "../common/Log.h"

namespace DXL {
namespace {

// 浮点 backbuffer 上"白"该是多少。
//
// scRGB（R16G16B16A16_FLOAT）里 1.0 = 80 nits，直接写 1.0 的字在 HDR 画面上暗得
// 像灰的。4.0 大约 320 nits，接近 SDR 白在 HDR 下的常规映射。
// **这是个约定值，不是测出来的** —— 真要准得读游戏的 HDR 元数据，不值得。
// 而且这条路本身还没在浮点 backbuffer 的真游戏上跑过（见 docs/LESSONS.md 的未验证清单）。
constexpr float FLOAT_WHITE_SCALE = 4.0f;

// float -> half。只用来写面板颜色，不需要处理 inf/nan/非规格化。
uint16_t ToHalf(float value) noexcept {
	uint32_t bits = 0;
	memcpy(&bits, &value, sizeof(bits));
	const uint32_t sign = (bits >> 16) & 0x8000u;
	const int exponent = int((bits >> 23) & 0xFF) - 127 + 15;
	const uint32_t mantissa = (bits >> 13) & 0x3FFu;
	if (exponent <= 0) return uint16_t(sign);
	if (exponent >= 31) return uint16_t(sign | 0x7C00u);
	return uint16_t(sign | (uint32_t(exponent) << 10) | mantissa);
}

}  // namespace

uint32_t GpuPanel::BytesPerPixelFor(DXGI_FORMAT format) noexcept {
	switch (format) {
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
	case DXGI_FORMAT_R10G10B10A2_UNORM:
		return 4;
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
		return 8;
	default:
		return 0;
	}
}

bool GpuPanel::Initialize(ID3D12Device* device, DXGI_FORMAT backbufferFormat,
	uint32_t maxWidth, uint32_t maxHeight, const wchar_t* debugName) noexcept {
	Release();
	_format = backbufferFormat;
	_bytesPerPixel = BytesPerPixelFor(backbufferFormat);
	_maxWidth = maxWidth;
	_maxHeight = maxHeight;
	if (!device || !_bytesPerPixel || !maxWidth || !maxHeight) {
		D5_LOG_WARN(L"面板：backbuffer 格式 %u 不在支持列表里，这个功能关掉。"
			L"（支持 RGBA8 / BGRA8 / RGB10A2 / RGBA16F）", (unsigned)backbufferFormat);
		return false;
	}

	_rowPitch = (maxWidth * _bytesPerPixel + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
		~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);

	D3D12_HEAP_PROPERTIES heap{};
	heap.Type = D3D12_HEAP_TYPE_UPLOAD;
	D3D12_RESOURCE_DESC desc{};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width = uint64_t(_rowPitch) * maxHeight;
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = DXGI_FORMAT_UNKNOWN;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	const HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
		&desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&_upload));
	if (FAILED(hr)) {
		D5_LOG_WARN(L"面板：上传堆创建失败 0x%08X，这个功能关掉。", hr);
		return false;
	}
	// 每个资源都 SetName —— 出错时调试层点名 "Unnamed" 就说明不是我们的
	_upload->SetName(debugName ? debugName : L"D5Q.GpuPanel.Upload");
	return true;
}

void GpuPanel::Release() noexcept {
	if (_upload) { _upload->Release(); _upload = nullptr; }
	_width = _height = 0;
	_rowPitch = 0;
	_bytesPerPixel = 0;
}

void GpuPanel::ConvertPixels(const uint8_t* bgra, uint32_t srcRowPitch,
	uint8_t* dst, uint32_t dstRowPitch,
	uint32_t width, uint32_t height, DXGI_FORMAT format) noexcept {
	if (!bgra || !dst || !width || !height) return;
	for (uint32_t y = 0; y < height; ++y) {
		const uint8_t* src = bgra + size_t(y) * srcRowPitch;
		uint8_t* row = dst + size_t(y) * dstRowPitch;
		for (uint32_t x = 0; x < width; ++x) {
			// GDI 的 32bpp BI_RGB DIB 是 BGRA 顺序
			const uint8_t b = src[x * 4 + 0];
			const uint8_t g = src[x * 4 + 1];
			const uint8_t r = src[x * 4 + 2];
			switch (format) {
			case DXGI_FORMAT_B8G8R8A8_UNORM:
			case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
				row[x * 4 + 0] = b;
				row[x * 4 + 1] = g;
				row[x * 4 + 2] = r;
				row[x * 4 + 3] = 0xFF;
				break;
			case DXGI_FORMAT_R8G8B8A8_UNORM:
			case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
				row[x * 4 + 0] = r;
				row[x * 4 + 1] = g;
				row[x * 4 + 2] = b;
				row[x * 4 + 3] = 0xFF;
				break;
			case DXGI_FORMAT_R10G10B10A2_UNORM: {
				// 8 位往 10 位铺开要 <<2 **再补高两位**，不能只 <<2 ——
				// 那样 255 变成 1020 而不是 1023，纯白会差一点点。
				const uint32_t r10 = (uint32_t(r) << 2) | (r >> 6);
				const uint32_t g10 = (uint32_t(g) << 2) | (g >> 6);
				const uint32_t b10 = (uint32_t(b) << 2) | (b >> 6);
				const uint32_t packed = r10 | (g10 << 10) | (b10 << 20) | (3u << 30);
				memcpy(row + x * 4, &packed, 4);
				break;
			}
			case DXGI_FORMAT_R16G16B16A16_FLOAT: {
				const uint16_t halves[4] = {
					ToHalf(r / 255.0f * FLOAT_WHITE_SCALE),
					ToHalf(g / 255.0f * FLOAT_WHITE_SCALE),
					ToHalf(b / 255.0f * FLOAT_WHITE_SCALE),
					ToHalf(1.0f)
				};
				memcpy(row + x * 8, halves, sizeof(halves));
				break;
			}
			default:
				break;
			}
		}
	}
}

bool GpuPanel::Upload(const uint8_t* bgra, uint32_t width, uint32_t height) noexcept {
	if (!_upload || !bgra || !width || !height) return false;
	if (width > _maxWidth || height > _maxHeight) return false;

	void* mapped = nullptr;
	// 整个上传堆都不读，读范围给一个空区间（起止相同）
	D3D12_RANGE nothing{ 0, 0 };
	if (FAILED(_upload->Map(0, &nothing, &mapped)) || !mapped) return false;

	// 逐像素转换抽在 ConvertPixels 里 —— GpuPanel11（D3D11 后端）调同一份，
	// 10 位铺开 / half 编码这种最容易写错的东西全工程只此一处。
	ConvertPixels(bgra, _maxWidth * 4, static_cast<uint8_t*>(mapped), _rowPitch,
		width, height, _format);
	// 写范围给 nullptr = "我写了全部"，让驱动做该做的刷新
	_upload->Unmap(0, nullptr);
	_width = width;
	_height = height;
	return true;
}

void GpuPanel::Record(ID3D12GraphicsCommandList* list, ID3D12Resource* target,
	uint32_t destX, uint32_t destY) noexcept {
	if (!list || !target || !_upload || !_width || !_height) return;

	D3D12_TEXTURE_COPY_LOCATION source{};
	source.pResource = _upload;
	source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	source.PlacedFootprint.Offset = 0;
	source.PlacedFootprint.Footprint.Format = _format;
	source.PlacedFootprint.Footprint.Width = _width;
	source.PlacedFootprint.Footprint.Height = _height;
	source.PlacedFootprint.Footprint.Depth = 1;
	source.PlacedFootprint.Footprint.RowPitch = _rowPitch;

	D3D12_TEXTURE_COPY_LOCATION destination{};
	destination.pResource = target;
	destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	destination.SubresourceIndex = 0;

	D3D12_BOX box{ 0, 0, 0, _width, _height, 1 };
	list->CopyTextureRegion(&destination, destX, destY, 0, &source, &box);
}

}  // namespace DXL
