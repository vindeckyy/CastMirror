#include "castcore/wgc_window_source.h"

#if defined(_WIN32)

#include "castcore/logger.h"
#include "castcore/pixel_convert.h"

#include <d3d11.h>
#include <dxgi.h>
#include <inspectable.h>
#include <windows.graphics.capture.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <windows.foundation.h>
#include <wrl/client.h>

#include <chrono>
#include <cstring>
#include <mutex>

namespace castcore {

namespace {

using Microsoft::WRL::ComPtr;
namespace Capture = ABI::Windows::Graphics::Capture;
namespace DX = ABI::Windows::Graphics::DirectX;
namespace D3D = ABI::Windows::Graphics::DirectX::Direct3D11;

// WinRT entry points are resolved at run time so the app still starts on a Windows
// build that lacks them; window capture then falls back to the desktop crop.
struct WinRt {
  using RoGetActivationFactoryFn = HRESULT(WINAPI*)(HSTRING, REFIID, void**);
  using WindowsCreateStringFn = HRESULT(WINAPI*)(PCNZWCH, UINT32, HSTRING*);
  using WindowsDeleteStringFn = HRESULT(WINAPI*)(HSTRING);
  using CreateD3DDeviceFn = HRESULT(WINAPI*)(IDXGIDevice*, IInspectable**);

  RoGetActivationFactoryFn get_factory = nullptr;
  WindowsCreateStringFn create_string = nullptr;
  WindowsDeleteStringFn delete_string = nullptr;
  CreateD3DDeviceFn create_d3d_device = nullptr;

  bool Load() {
    HMODULE combase = LoadLibraryW(L"combase.dll");
    HMODULE d3d11 = LoadLibraryW(L"d3d11.dll");
    if (!combase || !d3d11) return false;
    get_factory = reinterpret_cast<RoGetActivationFactoryFn>(
        GetProcAddress(combase, "RoGetActivationFactory"));
    create_string =
        reinterpret_cast<WindowsCreateStringFn>(GetProcAddress(combase, "WindowsCreateString"));
    delete_string =
        reinterpret_cast<WindowsDeleteStringFn>(GetProcAddress(combase, "WindowsDeleteString"));
    create_d3d_device = reinterpret_cast<CreateD3DDeviceFn>(
        GetProcAddress(d3d11, "CreateDirect3D11DeviceFromDXGIDevice"));
    return get_factory && create_string && delete_string && create_d3d_device;
  }

  // Activation factory for a runtime class, or null.
  template <typename T>
  ComPtr<T> Factory(const wchar_t* runtime_class) const {
    HSTRING name = nullptr;
    if (FAILED(create_string(runtime_class, static_cast<UINT32>(wcslen(runtime_class)), &name)))
      return nullptr;
    ComPtr<T> factory;
    const HRESULT hr =
        get_factory(name, __uuidof(T), reinterpret_cast<void**>(factory.GetAddressOf()));
    delete_string(name);
    return SUCCEEDED(hr) ? factory : nullptr;
  }
};

}  // namespace

struct WgcWindowSource::Impl {
  WinRt winrt;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<D3D::IDirect3DDevice> winrt_device;
  ComPtr<Capture::IGraphicsCaptureItem> item;
  ComPtr<Capture::IDirect3D11CaptureFramePool> pool;
  ComPtr<Capture::IGraphicsCaptureSession> session;
  ComPtr<ID3D11Texture2D> staging;
  UINT staging_w = 0;
  UINT staging_h = 0;
  ABI::Windows::Graphics::SizeInt32 pool_size{};

  ~Impl() {
    // Closing releases the capture (and the yellow border) promptly instead of
    // whenever the wrappers happen to be destroyed.
    ComPtr<ABI::Windows::Foundation::IClosable> closable;
    if (session && SUCCEEDED(session.As(&closable))) closable->Close();
    closable.Reset();
    if (pool && SUCCEEDED(pool.As(&closable))) closable->Close();
    session.Reset();
    pool.Reset();
    item.Reset();
    winrt_device.Reset();
    staging.Reset();
    context.Reset();
    device.Reset();
  }
};

WgcWindowSource::WgcWindowSource(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
WgcWindowSource::~WgcWindowSource() = default;

std::unique_ptr<WgcWindowSource> WgcWindowSource::Create(HWND hwnd) {
  if (!hwnd || !IsWindow(hwnd)) return nullptr;
  auto impl = std::make_unique<Impl>();
  if (!impl->winrt.Load()) {
    LOG_INFO << "Windows.Graphics.Capture is not available; window capture will crop the desktop "
                "instead";
    return nullptr;
  }
  // WinRT objects need a multithreaded apartment on every thread that touches them.
  // Keeping the process MTA alive once is safer than pairing CoInitialize and
  // CoUninitialize around the objects: tearing the apartment down while WinRT still
  // has work in flight crashed the process.
  static std::once_flag mta_once;
  std::call_once(mta_once, [] {
    CO_MTA_USAGE_COOKIE cookie = nullptr;
    CoIncrementMTAUsage(&cookie);
  });

  HRESULT hr = D3D11CreateDevice(nullptr,
                                 D3D_DRIVER_TYPE_HARDWARE,
                                 nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                 nullptr,
                                 0,
                                 D3D11_SDK_VERSION,
                                 &impl->device,
                                 nullptr,
                                 &impl->context);
  if (FAILED(hr)) {
    LOG_WARN << "WGC: could not create a D3D11 device: hr=0x" << std::hex << hr << std::dec;
    return nullptr;
  }
  ComPtr<IDXGIDevice> dxgi_device;
  ComPtr<IInspectable> inspectable;
  if (FAILED(impl->device.As(&dxgi_device)) ||
      FAILED(impl->winrt.create_d3d_device(dxgi_device.Get(), &inspectable)) ||
      FAILED(inspectable.As(&impl->winrt_device))) {
    LOG_WARN << "WGC: could not wrap the D3D11 device for WinRT";
    return nullptr;
  }

  auto interop = impl->winrt.Factory<IGraphicsCaptureItemInterop>(
      L"Windows.Graphics.Capture.GraphicsCaptureItem");
  if (!interop) return nullptr;
  hr = interop->CreateForWindow(hwnd,
                                __uuidof(Capture::IGraphicsCaptureItem),
                                reinterpret_cast<void**>(impl->item.GetAddressOf()));
  if (FAILED(hr) || !impl->item) {
    LOG_INFO << "WGC: this window cannot be captured directly: hr=0x" << std::hex << hr << std::dec;
    return nullptr;
  }
  if (FAILED(impl->item->get_Size(&impl->pool_size)) || impl->pool_size.Width <= 0 ||
      impl->pool_size.Height <= 0) {
    return nullptr;
  }

  auto statics = impl->winrt.Factory<Capture::IDirect3D11CaptureFramePoolStatics2>(
      L"Windows.Graphics.Capture.Direct3D11CaptureFramePool");
  if (!statics) return nullptr;
  hr = statics->CreateFreeThreaded(impl->winrt_device.Get(),
                                   DX::DirectXPixelFormat_B8G8R8A8UIntNormalized,
                                   2,
                                   impl->pool_size,
                                   &impl->pool);
  if (FAILED(hr) || !impl->pool) return nullptr;
  if (FAILED(impl->pool->CreateCaptureSession(impl->item.Get(), &impl->session)) || !impl->session)
    return nullptr;

  // The engine draws the pointer itself (it is a setting), so WGC must not draw a
  // second one. Older builds lack the option; the pointer is then drawn twice.
  ComPtr<Capture::IGraphicsCaptureSession2> session2;
  if (SUCCEEDED(impl->session.As(&session2))) session2->put_IsCursorCaptureEnabled(0);

  if (FAILED(impl->session->StartCapture())) return nullptr;
  // WGC only delivers a frame when the window changes. A window that has just
  // been shown, or that is idle, could otherwise leave the first frame blank until
  // something repaints it, so ask for one repaint now.
  RedrawWindow(
      hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW | RDW_FRAME);
  LOG_INFO << "Capturing window with Windows.Graphics.Capture (" << impl->pool_size.Width << "x"
           << impl->pool_size.Height << ")";
  return std::unique_ptr<WgcWindowSource>(new WgcWindowSource(std::move(impl)));
}

bool WgcWindowSource::TryGetFrame(CapturedVideoFrame* out) {
  Impl& s = *impl_;
  ComPtr<Capture::IDirect3D11CaptureFrame> frame;
  if (FAILED(s.pool->TryGetNextFrame(&frame)) || !frame) return false;
  // Several frames may be queued if the caller was slow; only the newest matters.
  for (;;) {
    ComPtr<Capture::IDirect3D11CaptureFrame> newer;
    if (FAILED(s.pool->TryGetNextFrame(&newer)) || !newer) break;
    frame = std::move(newer);
  }

  ABI::Windows::Graphics::SizeInt32 content{};
  if (FAILED(frame->get_ContentSize(&content)) || content.Width < 2 || content.Height < 2)
    return false;

  ComPtr<D3D::IDirect3DSurface> surface;
  ComPtr<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess> access;
  ComPtr<ID3D11Texture2D> texture;
  if (FAILED(frame->get_Surface(&surface)) || FAILED(surface.As(&access)) ||
      FAILED(access->GetInterface(IID_PPV_ARGS(&texture)))) {
    return false;
  }
  D3D11_TEXTURE2D_DESC desc{};
  texture->GetDesc(&desc);

  if (!s.staging || s.staging_w != desc.Width || s.staging_h != desc.Height) {
    D3D11_TEXTURE2D_DESC staging_desc = desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.MiscFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    s.staging.Reset();
    if (FAILED(s.device->CreateTexture2D(&staging_desc, nullptr, &s.staging))) return false;
    s.staging_w = desc.Width;
    s.staging_h = desc.Height;
  }
  s.context->CopyResource(s.staging.Get(), texture.Get());

  // 4:2:0 needs even dimensions; drop a trailing odd row or column rather than
  // scale. The texture can also be larger than the content (after a shrink).
  const int width = static_cast<int>(std::min<UINT>(content.Width, desc.Width)) & ~1;
  const int height = static_cast<int>(std::min<UINT>(content.Height, desc.Height)) & ~1;
  bool ok = false;
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (width > 0 && height > 0 &&
      SUCCEEDED(s.context->Map(s.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
    out->width = width;
    out->height = height;
    out->stride = width * 4;
    out->data.resize(static_cast<size_t>(out->stride) * height);
    out->timestamp = std::chrono::steady_clock::now();
    out->source_lost = false;
    ok = ConvertToBgra8(static_cast<const uint8_t*>(mapped.pData),
                        mapped.RowPitch,
                        SourcePixelFormat::kBgra8,
                        width,
                        height,
                        out->data.data(),
                        static_cast<size_t>(out->stride));
    s.context->Unmap(s.staging.Get(), 0);
  }

  // The window was resized: the pool must be rebuilt at the new size or later
  // frames arrive scaled into the old one.
  if (content.Width != s.pool_size.Width || content.Height != s.pool_size.Height) {
    s.pool_size = content;
    s.pool->Recreate(
        s.winrt_device.Get(), DX::DirectXPixelFormat_B8G8R8A8UIntNormalized, 2, content);
  }
  return ok;
}

}  // namespace castcore

#endif  // _WIN32
