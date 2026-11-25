#pragma once
// ScreenCapture.h — Windows Graphics Capture (монитор) для GuestAgent.exe
// Требования: Windows 10 1903+ (build 18362), C++17+, C++/WinRT (NuGet Microsoft.Windows.CppWinRT)
// Линковка: d3d11.lib, windowsapp.lib
// Вызывающий поток должен сделать winrt::init_apartment(winrt::apartment_type::multi_threaded).

#include <windows.h>
#include <d3d11_4.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <Windows.Graphics.Capture.Interop.h>
#include <Windows.Graphics.DirectX.Direct3D11.Interop.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace cap
{
    namespace wgc = winrt::Windows::Graphics::Capture;
    namespace wgd = winrt::Windows::Graphics::DirectX;
    namespace wgd3d = winrt::Windows::Graphics::DirectX::Direct3D11;

    struct Frame
    {
        uint64_t id = 0;                              // сквозной номер кадра
        int width = 0;
        int height = 0;
        int stride = 0;                               // байт в строке (= width * 4)
        std::chrono::steady_clock::time_point time;   // момент получения
        std::vector<uint8_t> bgra;                    // BGRA, width*height*4
    };

    class ScreenCapture
    {
    public:
        using FrameCallback = std::function<void(const std::shared_ptr<const Frame>&)>;

        ~ScreenCapture() { Stop(); }

        // maxFps = 0 -> без ограничения
        bool Start(int maxFps = 10, FrameCallback cb = nullptr)
        {
            if (m_running) return true;
            if (!wgc::GraphicsCaptureSession::IsSupported()) return false;

            m_cb = std::move(cb);
            m_minInterval = maxFps > 0 ? std::chrono::microseconds(1000000 / maxFps)
                : std::chrono::microseconds(0);

            if (!CreateDevice()) return false;

            // Основной монитор
            HMONITOR mon = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
            auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
            HRESULT hr = interop->CreateForMonitor(
                mon, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(m_item));
            if (FAILED(hr) || !m_item) return false;

            m_lastSize = m_item.Size();

            // FreeThreaded: событие приходит из пула потоков, окно/DispatcherQueue не нужны
            m_pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
                m_winrtDevice, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, m_lastSize);

            m_frameToken = m_pool.FrameArrived(
                [this](wgc::Direct3D11CaptureFramePool const& pool, winrt::Windows::Foundation::IInspectable const&)
                {
                    OnFrameArrived(pool);
                });

            m_session = m_pool.CreateCaptureSession(m_item);
            try { m_session.IsCursorCaptureEnabled(false); }
            catch (...) {}   // 2004+
            try { m_session.IsBorderRequired(false); }
            catch (...) {}         // Win11 / новые сборки
            m_session.StartCapture();

            m_running = true;
            return true;
        }

        void Stop()
        {
            if (!m_running.exchange(false)) return;
            if (m_pool) m_pool.FrameArrived(m_frameToken);
            if (m_session) { m_session.Close(); m_session = nullptr; }
            if (m_pool) { m_pool.Close();    m_pool = nullptr; }
            m_item = nullptr;
            std::lock_guard<std::mutex> lk(m_ctxMutex);
            m_staging.Reset();
            m_context.Reset();
            m_d3dDevice.Reset();
            m_winrtDevice = nullptr;
        }

        // Последний кадр (потокобезопасно). nullptr, если кадров ещё не было.
        std::shared_ptr<const Frame> Latest() const
        {
            std::lock_guard<std::mutex> lk(m_frameMutex);
            return m_latest;
        }

        bool UsingWarp() const { return m_usingWarp; }

    private:
        bool CreateDevice()
        {
            UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
            D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };

            HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                &m_d3dDevice, nullptr, &m_context);
            m_usingWarp = false;
            if (FAILED(hr))
            {
                // В VM без 3D-ускорения — программный рендер
                hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
                    levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                    &m_d3dDevice, nullptr, &m_context);
                m_usingWarp = true;
            }
            if (FAILED(hr)) return false;

            // Контекст используется из потока пула — включаем защиту
            Microsoft::WRL::ComPtr<ID3D11Multithread> mt;
            if (SUCCEEDED(m_d3dDevice.As(&mt))) mt->SetMultithreadProtected(TRUE);

            Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
            if (FAILED(m_d3dDevice.As(&dxgi))) return false;

            winrt::com_ptr<::IInspectable> insp;
            if (FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), insp.put()))) return false;
            m_winrtDevice = insp.as<wgd3d::IDirect3DDevice>();
            return true;
        }

        void OnFrameArrived(wgc::Direct3D11CaptureFramePool const& pool)
        {
            auto frame = pool.TryGetNextFrame();
            if (!frame || !m_running) return;

            // Ограничение FPS: лишние кадры просто отбрасываем
            auto now = std::chrono::steady_clock::now();
            if (m_minInterval.count() > 0 && (now - m_lastAccepted) < m_minInterval) return;
            m_lastAccepted = now;

            // Изменилось разрешение — пересоздаём пул, текущий кадр пропускаем
            auto content = frame.ContentSize();
            if (content.Width != m_lastSize.Width || content.Height != m_lastSize.Height)
            {
                m_lastSize = content;
                pool.Recreate(m_winrtDevice, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, content);
                return;
            }

            auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
            if (FAILED(access->GetInterface(IID_PPV_ARGS(&tex)))) return;

            D3D11_TEXTURE2D_DESC desc{};
            tex->GetDesc(&desc);

            auto out = std::make_shared<Frame>();
            {
                std::lock_guard<std::mutex> lk(m_ctxMutex);
                if (!m_context) return;

                if (!m_staging || m_stagingW != desc.Width || m_stagingH != desc.Height)
                {
                    D3D11_TEXTURE2D_DESC sd = desc;
                    sd.Usage = D3D11_USAGE_STAGING;
                    sd.BindFlags = 0;
                    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    sd.MiscFlags = 0;
                    m_staging.Reset();
                    if (FAILED(m_d3dDevice->CreateTexture2D(&sd, nullptr, &m_staging))) return;
                    m_stagingW = desc.Width;
                    m_stagingH = desc.Height;
                }

                m_context->CopyResource(m_staging.Get(), tex.Get());

                D3D11_MAPPED_SUBRESOURCE map{};
                if (FAILED(m_context->Map(m_staging.Get(), 0, D3D11_MAP_READ, 0, &map))) return;

                const int w = (std::min)((int)desc.Width, content.Width);
                const int h = (std::min)((int)desc.Height, content.Height);
                out->width = w;
                out->height = h;
                out->stride = w * 4;
                out->bgra.resize((size_t)out->stride * h);
                for (int y = 0; y < h; ++y)
                    memcpy(out->bgra.data() + (size_t)y * out->stride,
                        (const uint8_t*)map.pData + (size_t)y * map.RowPitch, out->stride);

                m_context->Unmap(m_staging.Get(), 0);
            }

            out->id = ++m_frameCounter;
            out->time = now;

            std::shared_ptr<const Frame> cf = out;
            {
                std::lock_guard<std::mutex> lk(m_frameMutex);
                m_latest = cf;
            }
            if (m_cb) m_cb(cf);
        }

        // D3D
        Microsoft::WRL::ComPtr<ID3D11Device>        m_d3dDevice;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
        Microsoft::WRL::ComPtr<ID3D11Texture2D>     m_staging;
        UINT m_stagingW = 0, m_stagingH = 0;
        wgd3d::IDirect3DDevice m_winrtDevice{ nullptr };
        std::mutex m_ctxMutex;
        bool m_usingWarp = false;

        // WGC
        wgc::GraphicsCaptureItem m_item{ nullptr };
        wgc::Direct3D11CaptureFramePool m_pool{ nullptr };
        wgc::GraphicsCaptureSession m_session{ nullptr };
        winrt::event_token m_frameToken{};
        winrt::Windows::Graphics::SizeInt32 m_lastSize{};

        // Состояние
        std::atomic<bool> m_running{ false };
        std::atomic<uint64_t> m_frameCounter{ 0 };
        std::chrono::microseconds m_minInterval{ 0 };
        std::chrono::steady_clock::time_point m_lastAccepted{};
        FrameCallback m_cb;

        mutable std::mutex m_frameMutex;
        std::shared_ptr<const Frame> m_latest;
    };

} // namespace cap