#include "ui/framework/image_loader.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>

#include <cstring>
#include <cstdint>

#include "util/log.hpp"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace sscl::ui {

namespace {
// IWICStream 的内存视图（不持有 buffer 生命周期者禁用；这里 buffer 在调用期内有效）。
class MemoryStream : public IStream {
public:
    explicit MemoryStream(const std::vector<std::uint8_t>& bytes) : m_bytes(bytes) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (riid == IID_IUnknown || riid == IID_ISequentialStream || riid == IID_IStream) {
            *object = static_cast<IStream*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG refs = --m_refs;
        if (refs == 0) {
            delete this;
        }
        return refs;
    }

    HRESULT STDMETHODCALLTYPE Read(void* data, ULONG size, ULONG* read) override {
        const std::size_t remain = m_bytes.size() - m_pos;
        const std::size_t take = size < remain ? size : remain;
        if (take > 0) {
            memcpy(data, m_bytes.data() + m_pos, take);
            m_pos += take;
        }
        if (read != nullptr) {
            *read = static_cast<ULONG>(take);
        }
        return take < size ? S_FALSE : S_OK;
    }
    HRESULT STDMETHODCALLTYPE Write(const void*, ULONG, ULONG*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin,
                                   ULARGE_INTEGER* new_pos) override {
        std::int64_t target = static_cast<std::int64_t>(m_pos);
        if (origin == STREAM_SEEK_SET) target = move.QuadPart;
        else if (origin == STREAM_SEEK_CUR) target += move.QuadPart;
        else if (origin == STREAM_SEEK_END) target = static_cast<std::int64_t>(m_bytes.size()) + move.QuadPart;
        if (target < 0) {
            return STG_E_INVALIDFUNCTION;
        }
        m_pos = static_cast<std::size_t>(target);
        if (new_pos != nullptr) {
            new_pos->QuadPart = m_pos;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*,
                                     ULARGE_INTEGER*) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Revert() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE Stat(STATSTG* stat, DWORD) override {
        if (stat != nullptr) {
            memset(stat, 0, sizeof(*stat));
            stat->cbSize.QuadPart = m_bytes.size();
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Clone(IStream**) override { return E_NOTIMPL; }

private:
    ULONG m_refs = 1;
    const std::vector<std::uint8_t>& m_bytes;
    std::size_t m_pos = 0;
};
}  // namespace

Microsoft::WRL::ComPtr<ID2D1Bitmap> load_bitmap_from_bytes(ID2D1RenderTarget* target,
                                                           const std::vector<std::uint8_t>& bytes) {
    if (target == nullptr || bytes.empty()) {
        return nullptr;
    }
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(factory.GetAddressOf())))) {
        return nullptr;
    }
    MemoryStream* stream = new MemoryStream(bytes);
    Microsoft::WRL::ComPtr<IWICStream> wic_stream;
    HRESULT hr = factory->CreateStream(wic_stream.GetAddressOf());
    if (SUCCEEDED(hr)) {
        hr = wic_stream->InitializeFromIStream(stream);
    }
    stream->Release();  // wic_stream 持有引用（成功时）
    if (FAILED(hr)) {
        return nullptr;
    }
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromStream(wic_stream.Get(), nullptr,
                                          WICDecodeMetadataCacheOnLoad,
                                          decoder.GetAddressOf());
    if (FAILED(hr)) {
        return nullptr;
    }
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, frame.GetAddressOf());
    if (FAILED(hr)) {
        return nullptr;
    }
    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    hr = factory->CreateFormatConverter(converter.GetAddressOf());
    if (SUCCEEDED(hr)) {
        hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                                   WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom);
    }
    if (FAILED(hr)) {
        return nullptr;
    }
    Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
    hr = target->CreateBitmapFromWicBitmap(converter.Get(), nullptr, bitmap.GetAddressOf());
    if (FAILED(hr)) {
        sscl::util::log_warn("[ui] CreateBitmapFromWicBitmap failed");
        return nullptr;
    }
    return bitmap;
}

Microsoft::WRL::ComPtr<ID2D1Bitmap> load_bitmap_from_ico(ID2D1RenderTarget* target,
                                                         const std::vector<std::uint8_t>& bytes,
                                                         unsigned int desired_px) {
    if (target == nullptr || bytes.empty()) {
        return nullptr;
    }
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(factory.GetAddressOf())))) {
        return nullptr;
    }
    MemoryStream* stream = new MemoryStream(bytes);
    Microsoft::WRL::ComPtr<IWICStream> wic_stream;
    HRESULT hr = factory->CreateStream(wic_stream.GetAddressOf());
    if (SUCCEEDED(hr)) {
        hr = wic_stream->InitializeFromIStream(stream);
    }
    stream->Release();
    if (FAILED(hr)) {
        return nullptr;
    }
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromStream(wic_stream.Get(), nullptr,
                                          WICDecodeMetadataCacheOnLoad,
                                          decoder.GetAddressOf());
    if (FAILED(hr)) {
        return nullptr;
    }
    // ICO 多帧：选 ≥ desired_px 的最小帧；不足时取最大帧。
    UINT count = 0;
    decoder->GetFrameCount(&count);
    if (count == 0) {
        return nullptr;
    }
    UINT best_index = 0;
    UINT best_size = 0;
    for (UINT i = 0; i < count; ++i) {
        Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
        if (FAILED(decoder->GetFrame(i, frame.GetAddressOf()))) {
            continue;
        }
        UINT width = 0;
        UINT height = 0;
        frame->GetSize(&width, &height);
        const UINT side = width < height ? width : height;
        if (best_size == 0) {
            best_index = i;
            best_size = side;
        } else if (side >= desired_px && (best_size < desired_px || side < best_size)) {
            best_index = i;
            best_size = side;
        } else if (side > best_size && best_size < desired_px) {
            best_index = i;
            best_size = side;
        }
    }
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(best_index, frame.GetAddressOf()))) {
        return nullptr;
    }
    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    hr = factory->CreateFormatConverter(converter.GetAddressOf());
    if (SUCCEEDED(hr)) {
        hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                                   WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom);
    }
    if (FAILED(hr)) {
        return nullptr;
    }
    Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
    if (FAILED(target->CreateBitmapFromWicBitmap(converter.Get(), nullptr,
                                                 bitmap.GetAddressOf()))) {
        return nullptr;
    }
    return bitmap;
}

// D-122：PCL MySkin.xaml.vb::Load 的「是否有头发层」判定。
// 老版 64×32 皮肤没有第二层（(40,8) 区域是重复图案/纯色），直接叠加会把
// 错位色块盖到脸上；PCL 通过「三个背景角是否有透明像素」或「帽子区参考点
// 颜色是否与三个背景角都不同」来判断是否存在有效帽子层。
bool analyze_skin_layout(const std::vector<std::uint8_t>& bytes, SkinLayout* out) {
    if (out == nullptr || bytes.empty()) {
        return false;
    }
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(factory.GetAddressOf())))) {
        return false;
    }
    MemoryStream* stream = new MemoryStream(bytes);
    Microsoft::WRL::ComPtr<IWICStream> wic_stream;
    HRESULT hr = factory->CreateStream(wic_stream.GetAddressOf());
    if (SUCCEEDED(hr)) {
        hr = wic_stream->InitializeFromIStream(stream);
    }
    stream->Release();
    if (FAILED(hr)) {
        return false;
    }
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromStream(wic_stream.Get(), nullptr,
                                          WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf());
    if (FAILED(hr)) {
        return false;
    }
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.GetAddressOf()))) {
        return false;
    }
    UINT width = 0;
    UINT height = 0;
    if (FAILED(frame->GetSize(&width, &height)) || width < 32 || height < 32 ||
        width > 4096 || height > 4096) {
        return false;
    }
    // BGRA（非预乘）：A 在每像素第 4 字节，颜色比较不受预乘影响。
    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(converter.GetAddressOf())) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom))) {
        return false;
    }
    const UINT stride = width * 4;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(stride) * height, 0);
    if (FAILED(converter->CopyPixels(nullptr, stride, static_cast<UINT>(pixels.size()),
                                     pixels.data()))) {
        return false;
    }

    auto at = [&](int x, int y) -> std::uint32_t {
        if (x < 0 || y < 0 || x >= static_cast<int>(width) || y >= static_cast<int>(height)) {
            return 0;
        }
        const std::uint8_t* p =
            pixels.data() + (static_cast<std::size_t>(y) * width + x) * 4;
        return (static_cast<std::uint32_t>(p[3]) << 24) |
               (static_cast<std::uint32_t>(p[2]) << 16) |
               (static_cast<std::uint32_t>(p[1]) << 8) | static_cast<std::uint32_t>(p[0]);
    };
    const float scale = static_cast<float>(width) / 64.0f;
    const std::uint32_t corner1 = at(1, 1);
    const std::uint32_t corner2 = at(static_cast<int>(width) - 1, static_cast<int>(height) - 1);
    const std::uint32_t corner3 =
        at(static_cast<int>(width) - 2, static_cast<int>(height) / 2 - 2);
    const std::uint32_t hat_sample =
        at(static_cast<int>(41 * scale), static_cast<int>(9 * scale));
    const bool has_transparency =
        ((corner1 >> 24) == 0) || ((corner2 >> 24) == 0) || ((corner3 >> 24) == 0);
    const bool color_mismatch = corner1 != hat_sample && corner2 != hat_sample &&
                                corner3 != hat_sample;

    out->width = width;
    out->height = height;
    out->scale = scale;
    // 64×32 老皮肤也可能被判定为有帽子层，但 (40,8)-(48,16) 仍在图内，安全。
    out->hat_layer = (width >= 64 && height >= 32) &&
                     (has_transparency || color_mismatch);
    return true;
}

}  // namespace sscl::ui
