// MIT License
//
// Copyright(c) 2022-2023 Matthieu Bucchianeri
//
// Borrows code from StereoKit: https://github.com/StereoKit/StereoKit/
// Copyright (c) 2019 Nick Klingensmith
//
// Borrows code from https://github.com/robmikh/Win32CaptureSample
// Copyright (c) 2019 Robert Mikhayelyan
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this softwareand associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright noticeand this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

namespace openxr_api_layer::capture {

    namespace {

        // Alternative to windows.graphics.directx.direct3d11.interop.h
        extern "C" {
        HRESULT __stdcall CreateDirect3D11DeviceFromDXGIDevice(::IDXGIDevice* dxgiDevice,
                                                               ::IInspectable** graphicsDevice);

        HRESULT __stdcall CreateDirect3D11SurfaceFromDXGISurface(::IDXGISurface* dgxiSurface,
                                                                 ::IInspectable** graphicsSurface);
        }

        // https://gist.github.com/kennykerr/15a62c8218254bc908de672e5ed405fa
        struct __declspec(uuid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1")) IDirect3DDXGIInterfaceAccess : ::IUnknown {
            virtual HRESULT __stdcall GetInterface(GUID const& id, void** object) = 0;
        };

    } // namespace

    struct ICaptureWindow {
        virtual ~ICaptureWindow() = default;

        virtual ID3D11Texture2D* getSurface() = 0;
    };

    struct CaptureWindowWinRT : ICaptureWindow {
        CaptureWindowWinRT(ID3D11Device* device, HWND window) {
            ComPtr<IDXGIDevice> dxgiDevice;
            CHECK_HRCMD(device->QueryInterface(IID_PPV_ARGS(dxgiDevice.ReleaseAndGetAddressOf())));
            ComPtr<IInspectable> object;
            CHECK_HRCMD(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), object.GetAddressOf()));
            CHECK_HRCMD(
                object->QueryInterface(winrt::guid_of<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>(),
                                       winrt::put_abi(m_interopDevice)));

            auto interop_factory = winrt::get_activation_factory<winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
                                                                 IGraphicsCaptureItemInterop>();
            CHECK_HRCMD(interop_factory->CreateForWindow(
                window,
                winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
                winrt::put_abi(m_item)));

            m_framePool = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
                m_interopDevice,
                static_cast<winrt::Windows::Graphics::DirectX::DirectXPixelFormat>(DXGI_FORMAT_R8G8B8A8_UNORM),
                2,
                m_item.Size());
            m_session = m_framePool.CreateCaptureSession(m_item);
            m_session.StartCapture();
        }

        ~CaptureWindowWinRT() override {
            m_session.Close();
            m_framePool.Close();
        }

        ID3D11Texture2D* getSurface() override {
            auto frame = m_framePool.TryGetNextFrame();
            if (frame != nullptr) {
                ComPtr<ID3D11Texture2D> surface;
                auto access = frame.Surface().as<IDirect3DDXGIInterfaceAccess>();
                CHECK_HRCMD(access->GetInterface(winrt::guid_of<ID3D11Texture2D>(),
                                                 reinterpret_cast<void**>(surface.ReleaseAndGetAddressOf())));

                m_lastCapturedFrame = frame;
                m_lastCapturedSurface = surface;
            }

            return m_lastCapturedSurface.Get();
        }

      private:
        winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice m_interopDevice;
        winrt::Windows::Graphics::Capture::GraphicsCaptureItem m_item{nullptr};
        winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool m_framePool{nullptr};
        winrt::Windows::Graphics::Capture::GraphicsCaptureSession m_session{nullptr};
        winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame m_lastCapturedFrame{nullptr};
        ComPtr<ID3D11Texture2D> m_lastCapturedSurface;
    };

    struct CaptureWindowDWM : ICaptureWindow {
        CaptureWindowDWM(ID3D11Device* device, HWND window) {
            typedef BOOL(WINAPI * pfnGetDxSharedSurface)(HANDLE hHandle,
                                                         HANDLE * phSurface,
                                                         LUID * pAdapterLuid,
                                                         ULONG * pFmtWindow,
                                                         ULONG * pPresentFlags,
                                                         ULONGLONG * pWin32kUpdateId);
            pfnGetDxSharedSurface DwmGetDxSharedSurface{nullptr};
            DwmGetDxSharedSurface =
                (pfnGetDxSharedSurface)GetProcAddress(LoadLibrary(L"user32.dll"), "DwmGetDxSharedSurface");

            HANDLE handle = nullptr;
            LUID luid = {0};
            ULONG format = 0;
            ULONG flags = 0;
            ULONGLONG update_id = 0;
            CHECK_MSG(DwmGetDxSharedSurface(window, &handle, &luid, &format, &flags, &update_id),
                      "Failed to get window surface\n");
            CHECK_HRCMD(
                device->OpenSharedResource(handle, IID_PPV_ARGS(m_windowSharedSurface.ReleaseAndGetAddressOf())));
        }

        ID3D11Texture2D* getSurface() override {
            return m_windowSharedSurface.Get();
        }

      private:
        ComPtr<ID3D11Texture2D> m_windowSharedSurface;
    };

} // namespace openxr_api_layer::capture
