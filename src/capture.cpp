#include "control.hpp"
#include <d3d11.h>
#include <dxgi1_6.h>
#include <wincodec.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.Imaging.h>
namespace dc
{
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;
using namespace winrt::Windows::Graphics::Imaging;
namespace
{
MIDL_INTERFACE("5B0D3235-4DBA-4D44-865E-8F1D0E4FD04D") MemoryBufferAccess : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetBuffer(BYTE * *bytes, UINT32 * capacity) = 0;
};
struct Frames
{
    std::mutex mutex;
    std::condition_variable ready;
    Direct3D11CaptureFrame frame{nullptr};
    bool stopped = false;
};
std::vector<uint8_t> png(SoftwareBitmap const &bitmap, int width, int height)
{
    auto lock = bitmap.LockBuffer(BitmapBufferAccessMode::Read);
    auto reference = lock.CreateReference();
    auto access = reference.as<MemoryBufferAccess>();
    BYTE *data{};
    UINT32 capacity{};
    winrt::check_hresult(access->GetBuffer(&data, &capacity));
    auto plane = lock.GetPlaneDescription(0);
    if (width <= 0 || height <= 0 || width > plane.Width || height > plane.Height)
        throw Error("CAPTURE_SIZE", "Invalid frame content size", "encoding");
    size_t rowSize = size_t(width) * 4;
    if (rowSize * height > 64 * 1024 * 1024)
        throw Error("CAPTURE_LIMIT", "Screenshot exceeds 64 MiB", "encoding");
    std::vector<uint8_t> pixels(rowSize * height);
    for (int y = 0; y < height; ++y)
    {
        int64_t start = int64_t(plane.StartIndex) + int64_t(y) * plane.Stride;
        if (start < 0 || uint64_t(start) + rowSize > capacity)
            throw Error("CAPTURE_BUFFER", "Invalid bitmap stride", "encoding");
        memcpy(pixels.data() + size_t(y) * rowSize, data + start, rowSize);
    }
    winrt::com_ptr<IWICImagingFactory> factory;
    winrt::check_hresult(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                          IID_PPV_ARGS(factory.put())));
    winrt::com_ptr<IStream> stream;
    winrt::check_hresult(CreateStreamOnHGlobal(nullptr, TRUE, stream.put()));
    winrt::com_ptr<IWICBitmapEncoder> encoder;
    winrt::check_hresult(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()));
    winrt::check_hresult(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache));
    winrt::com_ptr<IWICBitmapFrameEncode> frame;
    winrt::com_ptr<IPropertyBag2> options;
    winrt::check_hresult(encoder->CreateNewFrame(frame.put(), options.put()));
    winrt::check_hresult(frame->Initialize(options.get()));
    winrt::check_hresult(frame->SetSize(width, height));
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    winrt::check_hresult(frame->SetPixelFormat(&format));
    if (format != GUID_WICPixelFormat32bppBGRA)
        throw Error("PNG_FORMAT", "PNG encoder rejected BGRA", "encoding");
    winrt::check_hresult(frame->WritePixels(height, (UINT)rowSize, (UINT)pixels.size(), pixels.data()));
    winrt::check_hresult(frame->Commit());
    winrt::check_hresult(encoder->Commit());
    STATSTG stat{};
    winrt::check_hresult(stream->Stat(&stat, STATFLAG_NONAME));
    std::vector<uint8_t> encoded(size_t(stat.cbSize.QuadPart));
    LARGE_INTEGER zero{};
    winrt::check_hresult(stream->Seek(zero, STREAM_SEEK_SET, nullptr));
    ULONG n{};
    winrt::check_hresult(stream->Read(encoded.data(), (ULONG)encoded.size(), &n));
    if (n != encoded.size())
        throw Error("PNG_READ", "PNG stream incomplete", "encoding");
    return encoded;
}
bool hdr_enabled(HWND hwnd)
{
    winrt::com_ptr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.put()))))
        return false;
    HMONITOR target = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    for (UINT i = 0;; ++i)
    {
        winrt::com_ptr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i, adapter.put()) == DXGI_ERROR_NOT_FOUND)
            break;
        for (UINT j = 0;; ++j)
        {
            winrt::com_ptr<IDXGIOutput> output;
            if (adapter->EnumOutputs(j, output.put()) == DXGI_ERROR_NOT_FOUND)
                break;
            winrt::com_ptr<IDXGIOutput6> modern;
            if (SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(modern.put()))))
            {
                DXGI_OUTPUT_DESC1 d{};
                if (SUCCEEDED(modern->GetDesc1(&d)) && d.Monitor == target)
                    return d.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ||
                           d.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
            }
        }
    }
    return false;
}
json take(HWND hwnd, bool warp, unsigned timeout)
{
    // Capture hwnd with hardware or WARP; timeout bounds the frame wait in milliseconds.
    auto started = Clock::now();
    worker_event("waiting_frame");
    winrt::com_ptr<ID3D11Device> native;
    winrt::com_ptr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level{};
    winrt::check_hresult(D3D11CreateDevice(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE,
                                           nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                           D3D11_SDK_VERSION, native.put(), &level, context.put()));
    auto dxgi = native.as<IDXGIDevice>();
    winrt::com_ptr<::IInspectable> inspectable;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()));
    auto device = inspectable.as<IDirect3DDevice>();
    auto factory = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    GraphicsCaptureItem item{nullptr};
    winrt::check_hresult(
        factory->CreateForWindow(hwnd, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item)));
    auto pool = Direct3D11CaptureFramePool::CreateFreeThreaded(
        device, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, item.Size());
    auto session = pool.CreateCaptureSession(item);
    auto frames = std::make_shared<Frames>();
    auto token = pool.FrameArrived([frames](auto const &sender, auto const &) noexcept {
        try
        {
            auto next = sender.TryGetNextFrame();
            if (!next)
                return;
            std::lock_guard guard(frames->mutex);
            if (frames->stopped)
            {
                next.Close();
                return;
            }
            if (frames->frame)
                frames->frame.Close();
            frames->frame = next;
            frames->ready.notify_one();
        }
        catch (...)
        {
        }
    });
    auto cleanup = [&] {
        {
            std::lock_guard guard(frames->mutex);
            frames->stopped = true;
            if (frames->frame)
            {
                frames->frame.Close();
                frames->frame = nullptr;
            }
        }
        pool.FrameArrived(token);
        session.Close();
        pool.Close();
    };
    try
    {
        session.StartCapture();
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            Direct3D11CaptureFrame frame{nullptr};
            {
                std::unique_lock guard(frames->mutex);
                if (!frames->ready.wait_for(guard, std::chrono::milliseconds(timeout),
                                            [&] { return bool(frames->frame); }))
                    throw Error("FIRST_FRAME_TIMEOUT", "No window frame arrived", "waiting_frame");
                frame = std::exchange(frames->frame, nullptr);
            }
            auto size = frame.ContentSize();
            auto surface = frame.Surface();
            auto texture =
                surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            winrt::com_ptr<ID3D11Texture2D> nativeTexture;
            winrt::check_hresult(texture->GetInterface(IID_PPV_ARGS(nativeTexture.put())));
            D3D11_TEXTURE2D_DESC desc{};
            nativeTexture->GetDesc(&desc);
            if (size.Width > int(desc.Width) || size.Height > int(desc.Height))
            {
                frame.Close();
                pool.Recreate(device, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
                continue;
            }
            auto received = Clock::now();
            worker_event("converting");
            auto bitmap = SoftwareBitmap::CreateCopyFromSurfaceAsync(surface).get();
            auto converted = Clock::now();
            frame.Close();
            if (bitmap.BitmapPixelFormat() != BitmapPixelFormat::Bgra8)
                bitmap = SoftwareBitmap::Convert(bitmap, BitmapPixelFormat::Bgra8);
            worker_event("encoding");
            auto bytes = png(bitmap, size.Width, size.Height);
            bitmap.Close();
            cleanup();
            return {{"width", size.Width},
                    {"height", size.Height},
                    {"mime_type", "image/png"},
                    {"base64", base64(bytes)},
                    {"backend", warp ? "warp" : "hardware"},
                    {"timings_ms",
                     {{"frame", std::chrono::duration<double, std::milli>(received - started).count()},
                      {"conversion", std::chrono::duration<double, std::milli>(converted - received).count()},
                      {"encoding",
                       std::chrono::duration<double, std::milli>(Clock::now() - converted).count()}}}};
        }
        throw Error("CAPTURE_RESIZE", "Window kept resizing", "waiting_frame");
    }
    catch (...)
    {
        cleanup();
        throw;
    }
}
} // namespace
json capture_request(json const &req)
{
    // Capture the target in req using its optional timeout_ms and backend preference.
    auto hwnd = reinterpret_cast<HWND>(req.at("hwnd").get<uint64_t>());
    if (!IsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd))
        throw Error("CAPTURE_TARGET", "Capture target must be visible and not minimized");
    if (!GraphicsCaptureSession::IsSupported())
        throw Error("CAPTURE_UNSUPPORTED", "Windows Graphics Capture is unavailable");
    if (hdr_enabled(hwnd))
        throw Error("HDR_UNSUPPORTED", "First release supports SDR screenshots only");
    // req supplies target, backend and optional frame timeout in milliseconds.
    bool warp = req.value("warp", false);
    const auto timeout = req.value("timeout_ms", unsigned(2000));
    try
    {
        return take(hwnd, warp, timeout);
    }
    catch (winrt::hresult_error const &e)
    {
        auto hr = e.code().value;
        if (!warp && (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET ||
                      hr == DXGI_ERROR_UNSUPPORTED || hr == E_FAIL))
            return take(hwnd, true, timeout);
        throw;
    }
}
} // namespace dc
