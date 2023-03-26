// MIT License
//
// << insert your own copyright here >>
//
// Based on https://github.com/mbucchia/OpenXR-Layer-Template.
// Copyright(c) 2022-2023 Matthieu Bucchianeri
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

#include "pch.h"

#include "layer.h"
#include <log.h>
#include <util.h>

#include "capture.h"

#pragma comment(lib, "d3dcompiler.lib")

namespace openxr_api_layer {

    using namespace log;
    using namespace utils;
    using namespace xr::math;

    // Our API layer implement these extensions, and their specified version.
    const std::vector<std::pair<std::string, uint32_t>> advertisedExtensions = {};

    // Initialize these vectors with arrays of extensions to block and implicitly request for the instance.
    const std::vector<std::string> blockedExtensions = {};
    const std::vector<std::string> implicitExtensions = {};

    // The process to start to create the overlay window.
    const std::wstring OverlayProcessName = L"WindowsFormsApp.exe";

    // A shader that makes a color transparent.
    const std::string_view TransparencyShaderHlsl =
        R"_(
cbuffer config : register(b0) {
    float3 TransparentColor;
    float Transparency;
};
Texture2D in_texture : register(t0);
RWTexture2D<float4> out_texture : register(u0);

[numthreads(8, 8, 1)]
void main(uint2 pos : SV_DispatchThreadID)
{
    float alpha = (all(in_texture[pos].rgb == TransparentColor)) ? Transparency : 1.f;
    out_texture[pos] = float4(in_texture[pos].rgb, alpha);
}
    )_";

    struct TransparencyShaderConstants {
        XrVector3f transparentColor;
        float transparencyLevel;
    };

    // https://stackoverflow.com/questions/7956519/how-to-kill-processes-by-name-win32-api
    void killProcessByName(const std::wstring& filename) {
        HANDLE hSnapShot = CreateToolhelp32Snapshot(TH32CS_SNAPALL, NULL);
        PROCESSENTRY32 pEntry{sizeof(pEntry)};
        BOOL hRes = Process32First(hSnapShot, &pEntry);
        while (hRes) {
            const std::wstring_view exeFile(pEntry.szExeFile);
            if (exeFile == filename) {
                HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, 0, (DWORD)pEntry.th32ProcessID);
                if (hProcess != NULL) {
                    TerminateProcess(hProcess, 9);
                    CloseHandle(hProcess);
                }
            }
            hRes = Process32Next(hSnapShot, &pEntry);
        }
        CloseHandle(hSnapShot);
    }

    // This class implements our API layer.
    class OpenXrLayer : public openxr_api_layer::OpenXrApi {
      public:
        OpenXrLayer() = default;
        ~OpenXrLayer() = default;

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrGetInstanceProcAddr
        XrResult xrGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function) override {
            TraceLoggingWrite(g_traceProvider,
                              "xrGetInstanceProcAddr",
                              TLXArg(instance, "Instance"),
                              TLArg(name, "Name"),
                              TLArg(m_bypassApiLayer, "Bypass"));

            XrResult result;
            if (!m_bypassApiLayer) {
                result = OpenXrApi::xrGetInstanceProcAddr(instance, name, function);

                // Required to call this method for housekeeping.
                if (m_compositionFrameworkFactory) {
                    m_compositionFrameworkFactory->xrGetInstanceProcAddr_post(instance, name, function);
                }
            } else {
                result = m_xrGetInstanceProcAddr(instance, name, function);
            }

            TraceLoggingWrite(g_traceProvider, "xrGetInstanceProcAddr", TLPArg(*function, "Function"));

            return result;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrCreateInstance
        XrResult xrCreateInstance(const XrInstanceCreateInfo* createInfo) override {
            if (createInfo->type != XR_TYPE_INSTANCE_CREATE_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            // Needed to resolve the requested function pointers.
            OpenXrApi::xrCreateInstance(createInfo);

            // Dump the application name, OpenXR runtime information and other useful things for debugging.
            TraceLoggingWrite(g_traceProvider,
                              "xrCreateInstance",
                              TLArg(xr::ToString(createInfo->applicationInfo.apiVersion).c_str(), "ApiVersion"),
                              TLArg(createInfo->applicationInfo.applicationName, "ApplicationName"),
                              TLArg(createInfo->applicationInfo.applicationVersion, "ApplicationVersion"),
                              TLArg(createInfo->applicationInfo.engineName, "EngineName"),
                              TLArg(createInfo->applicationInfo.engineVersion, "EngineVersion"),
                              TLArg(createInfo->createFlags, "CreateFlags"));
            Log(fmt::format("Application: {}\n", createInfo->applicationInfo.applicationName));

            // Here there can be rules to disable the API layer entirely (based on applicationName for example).
            // m_bypassApiLayer = ...

            if (m_bypassApiLayer) {
                Log(fmt::format("{} layer will be bypassed\n", LayerName));
                return XR_SUCCESS;
            }

            for (uint32_t i = 0; i < createInfo->enabledApiLayerCount; i++) {
                TraceLoggingWrite(
                    g_traceProvider, "xrCreateInstance", TLArg(createInfo->enabledApiLayerNames[i], "ApiLayerName"));
            }
            for (uint32_t i = 0; i < createInfo->enabledExtensionCount; i++) {
                TraceLoggingWrite(
                    g_traceProvider, "xrCreateInstance", TLArg(createInfo->enabledExtensionNames[i], "ExtensionName"));
            }

            XrInstanceProperties instanceProperties = {XR_TYPE_INSTANCE_PROPERTIES};
            CHECK_XRCMD(OpenXrApi::xrGetInstanceProperties(GetXrInstance(), &instanceProperties));
            const auto runtimeName = fmt::format("{} {}.{}.{}",
                                                 instanceProperties.runtimeName,
                                                 XR_VERSION_MAJOR(instanceProperties.runtimeVersion),
                                                 XR_VERSION_MINOR(instanceProperties.runtimeVersion),
                                                 XR_VERSION_PATCH(instanceProperties.runtimeVersion));
            TraceLoggingWrite(g_traceProvider, "xrCreateInstance", TLArg(runtimeName.c_str(), "RuntimeName"));
            Log(fmt::format("Using OpenXR runtime: {}\n", runtimeName));

            // Initialize the composition framework factory.
            m_compositionFrameworkFactory = graphics::createCompositionFrameworkFactory(
                *createInfo, GetXrInstance(), m_xrGetInstanceProcAddr, graphics::CompositionApi::D3D11);

            // Terminate any prior overlay window process.
            killProcessByName(OverlayProcessName);

            return XR_SUCCESS;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrGetSystem
        XrResult xrGetSystem(XrInstance instance, const XrSystemGetInfo* getInfo, XrSystemId* systemId) override {
            if (getInfo->type != XR_TYPE_SYSTEM_GET_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            TraceLoggingWrite(g_traceProvider,
                              "xrGetSystem",
                              TLXArg(instance, "Instance"),
                              TLArg(xr::ToCString(getInfo->formFactor), "FormFactor"));

            const XrResult result = OpenXrApi::xrGetSystem(instance, getInfo, systemId);
            if (XR_SUCCEEDED(result) && getInfo->formFactor == XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY) {
                static bool wasSystemNameLogged = false;
                if (!wasSystemNameLogged) {
                    XrSystemProperties systemProperties{XR_TYPE_SYSTEM_PROPERTIES};
                    CHECK_XRCMD(OpenXrApi::xrGetSystemProperties(instance, *systemId, &systemProperties));
                    TraceLoggingWrite(g_traceProvider, "xrGetSystem", TLArg(systemProperties.systemName, "SystemName"));
                    Log(fmt::format("Using OpenXR system: {}\n", systemProperties.systemName));
                    wasSystemNameLogged = true;
                }
            }

            TraceLoggingWrite(g_traceProvider, "xrGetSystem", TLArg((int)*systemId, "SystemId"));

            return result;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrEndFrame
        XrResult xrEndFrame(XrSession session, const XrFrameEndInfo* frameEndInfo) override {
            if (frameEndInfo->type != XR_TYPE_FRAME_END_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            TraceLoggingWrite(g_traceProvider,
                              "xrEndFrame",
                              TLXArg(session, "Session"),
                              TLArg(frameEndInfo->displayTime, "DisplayTime"),
                              TLArg(frameEndInfo->layerCount, "LayerCount"));

            XrFrameEndInfo chainFrameEndInfo = *frameEndInfo;
            std::vector<const XrCompositionLayerBaseHeader*> layers(
                chainFrameEndInfo.layers, chainFrameEndInfo.layers + chainFrameEndInfo.layerCount);
            XrCompositionLayerQuad overlay{XR_TYPE_COMPOSITION_LAYER_QUAD};

            // Handle the overlay.
            graphics::ICompositionFramework* composition =
                m_compositionFrameworkFactory->getCompositionFramework(session);
            if (composition) {
                SessionData* sessionData = composition->getSessionData<SessionData>();

                // First time: initialize the resources for the session.
                if (!sessionData) {
                    // Allocate storage for the state.
                    composition->setSessionData(std::move(std::make_unique<SessionData>(*this, composition)));
                    sessionData = composition->getSessionData<SessionData>();
                }

                // Refresh the content of the overlay.
                if (sessionData->captureOverlayWindow(composition)) {
                    // Place the overlay.
                    overlay.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                    overlay.subImage = sessionData->overlaySwapchain->getSubImage();
                    overlay.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                    overlay.pose = sessionData->overlayPose;
                    overlay.space = sessionData->localSpace;
                    overlay.size = sessionData->overlaySize;

                    // Append our overlay quad layer.
                    layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&overlay));
                }
            }

            chainFrameEndInfo.layers = layers.data();
            chainFrameEndInfo.layerCount = (uint32_t)layers.size();

            return OpenXrApi::xrEndFrame(session, &chainFrameEndInfo);
        }

      private:
        struct SessionData : graphics::ICompositionSessionData {
            SessionData(OpenXrApi& openxr, graphics::ICompositionFramework* composition) : m_openxr(openxr) {
                // Create the resources for the transparency shader
                ID3D11Device* const device = composition->getCompositionDevice()->getNativeDevice<graphics::D3D11>();
                const auto compileShader =
                    [&](const std::string_view& code, const std::string_view& entry, ID3D11ComputeShader** shader) {
                        ComPtr<ID3DBlob> shaderBytes;
                        ComPtr<ID3DBlob> errMsgs;
                        DWORD flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS;
#ifdef _DEBUG
                        flags |= D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_DEBUG;
#else
                        flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

                        const HRESULT hr = D3DCompile(code.data(),
                                                      code.size(),
                                                      nullptr,
                                                      nullptr,
                                                      nullptr,
                                                      entry.data(),
                                                      "cs_5_0",
                                                      flags,
                                                      0,
                                                      shaderBytes.ReleaseAndGetAddressOf(),
                                                      errMsgs.ReleaseAndGetAddressOf());
                        if (FAILED(hr)) {
                            std::string errMsg((const char*)errMsgs->GetBufferPointer(), errMsgs->GetBufferSize());
                            ErrorLog("D3DCompile failed %X: %s\n", hr, errMsg.c_str());
                            CHECK_HRESULT(hr, "D3DCompile failed");
                        }
                        CHECK_HRCMD(device->CreateComputeShader(
                            shaderBytes->GetBufferPointer(), shaderBytes->GetBufferSize(), nullptr, shader));
                    };

                compileShader(TransparencyShaderHlsl, "main", m_transparencyShader.ReleaseAndGetAddressOf());

                // Pick the color to make transparent.
                TransparencyShaderConstants transparencyParams{};
                transparencyParams.transparentColor = {255 / 255.f, 0 / 255.f, 255 / 255.f};
                D3D11_BUFFER_DESC desc{};
                desc.ByteWidth = sizeof(transparencyParams);
                desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                desc.Usage = D3D11_USAGE_DYNAMIC;
                desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                D3D11_SUBRESOURCE_DATA data{};
                data.pSysMem = &transparencyParams;
                CHECK_HRCMD(device->CreateBuffer(&desc, &data, m_transparencyConstants.ReleaseAndGetAddressOf()));

                // Create a head-locked reference space.
                XrReferenceSpaceCreateInfo createViewSpaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
                createViewSpaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
                createViewSpaceInfo.poseInReferenceSpace = Pose::Identity();
                CHECK_XRCMD(m_openxr.xrCreateReferenceSpace(
                    composition->getSessionHandle(), &createViewSpaceInfo, &localSpace));

                // Pick an initial pose.
                overlayPose = Pose::Translation({0, 0, -1});
                overlaySize = {1.f, 1.f};
            }

            ~SessionData() override {
                if (m_overlayProcessInfo.dwProcessId != 0) {
                    PostThreadMessage(m_overlayProcessInfo.dwThreadId, WM_QUIT, 0, 0);
                }

                if (localSpace != XR_NULL_HANDLE) {
                    m_openxr.xrDestroySpace(localSpace);
                }
            }

            bool captureOverlayWindow(graphics::ICompositionFramework* composition) {
                // See if the overlay process is already started.
                if (m_overlayProcessInfo.dwProcessId != 0) {
                    if (!WaitForSingleObject(m_overlayProcessInfo.hProcess, 0)) {
                        // Destroy all resources for the process.
                        m_captureWindow.reset();
                        overlaySwapchain.reset();
                        CloseHandle(m_overlayProcessInfo.hThread);
                        CloseHandle(m_overlayProcessInfo.hProcess);

                        // Mark as finished.
                        m_overlayProcessInfo = {};
                    }
                }

                // Start the process if needed.
                if (m_overlayProcessInfo.dwProcessId == 0) {
                    STARTUPINFO info = {sizeof(info)};
                    if (!CreateProcess((dllHome / OverlayProcessName).wstring().c_str(),
                                       nullptr,
                                       nullptr,
                                       nullptr,
                                       FALSE,
                                       0,
                                       nullptr,
                                       nullptr,
                                       &info,
                                       &m_overlayProcessInfo)) {
                        static bool logged = false;
                        if (!logged) {
                            Log(fmt::format("Failed to start overlay process: {}\n", GetLastError()));
                            logged = true;
                        }
                        return false;
                    }
                }

                // Find the window to duplicate.
                struct WindowLookup {
                    DWORD processId;
                    HWND windowToDuplicate{0};
                } windowLookup;
                windowLookup.processId = m_overlayProcessInfo.dwProcessId;
                EnumWindows(
                    [](HWND hwnd, LPARAM lParam) {
                        WindowLookup* lookup = (WindowLookup*)lParam;

                        if (hwnd == nullptr || hwnd == GetShellWindow() || !IsWindowVisible(hwnd) ||
                            GetAncestor(hwnd, GA_ROOT) != hwnd || GetWindowLongPtr(hwnd, GWL_STYLE) & WS_DISABLED) {
                            return TRUE;
                        }

                    // Here we demonstrate 2 ways to capture a window:
#if 1
                        // 1) By process ID that we started above.
                        DWORD processId;
                        GetWindowThreadProcessId(hwnd, &processId);
                        if (processId != lookup->processId) {
                            return TRUE;
                        }
#else
                        // 2) By window title.
                        wchar_t text[256];
                        if (GetWindowText(hwnd, text, sizeof(text) / sizeof(wchar_t)) == 0) {
                            return TRUE;
                        }
                        std::wstring_view windowTitle(text);
                        // TODO: Put the title of the window you want to capture!
                        if (windowTitle != L"OverlayForm") {
                            return TRUE;
                        }
#endif
                        lookup->windowToDuplicate = hwnd;
                        return FALSE;
                    },
                    (LPARAM)&windowLookup);

                if (!windowLookup.windowToDuplicate) {
                    m_captureWindow.reset();
                    overlaySwapchain.reset();
                    return false;
                }

                ID3D11Device* const device = composition->getCompositionDevice()->getNativeDevice<graphics::D3D11>();

                // Open the shared surface.
                if (!m_captureWindow) {
                    // Here we demonstrate 2 ways to capture a window:
#if 0
                    // 1) Using DWM internal API.
                    m_captureWindow =
                        std::make_shared<capture::CaptureWindowDWM>(device, windowLookup.windowToDuplicate);
#else
                    // 2) Using WinRT API.
                    m_captureWindow =
                        std::make_shared<capture::CaptureWindowWinRT>(device, windowLookup.windowToDuplicate);
#endif
                }
                ID3D11Texture2D* const windowSurface = m_captureWindow->getSurface();
                if (!windowSurface) {
                    return false;
                }

                // (re)Create the swapchain if needed.
                D3D11_TEXTURE2D_DESC windowSharedSurfaceDesc;
                windowSurface->GetDesc(&windowSharedSurfaceDesc);
                if (!overlaySwapchain ||
                    overlaySwapchain->getInfoOnCompositionDevice().width != windowSharedSurfaceDesc.Width ||
                    overlaySwapchain->getInfoOnCompositionDevice().height != windowSharedSurfaceDesc.Height) {
                    // Create a swapchain for the overlay.
                    XrSwapchainCreateInfo overlaySwapchainInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
                    overlaySwapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT;
                    overlaySwapchainInfo.arraySize = 1;
                    overlaySwapchainInfo.width = windowSharedSurfaceDesc.Width;
                    overlaySwapchainInfo.height = windowSharedSurfaceDesc.Height;
                    overlaySwapchainInfo.format =
                        composition->getCompositionDevice()->translateFromGenericFormat(DXGI_FORMAT_R8G8B8A8_UNORM);
                    overlaySwapchainInfo.mipCount = 1;
                    overlaySwapchainInfo.sampleCount = 1;
                    overlaySwapchainInfo.faceCount = 1;
                    overlaySwapchain = composition->createSwapchain(
                        overlaySwapchainInfo, graphics::SwapchainMode::Write | graphics::SwapchainMode::Submit);

                    // Keep aspect ratio.
                    overlaySize.height =
                        overlaySize.width * ((float)overlaySwapchainInfo.height / overlaySwapchainInfo.width);
                }

                // Copy the most recent window content into the swapchain.
                graphics::ISwapchainImage* const acquiredImage = overlaySwapchain->acquireImage();
                {
                    ID3D11DeviceContext* const context =
                        composition->getCompositionDevice()->getNativeContext<graphics::D3D11>();
                    ID3D11Texture2D* const surface =
                        acquiredImage->getTextureForWrite()->getNativeTexture<graphics::D3D11>();

                    // Create ephemeral resources to run our transparency shader.
                    ComPtr<ID3D11ShaderResourceView> srv;
                    {
                        D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
                        desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                        desc.Format = windowSharedSurfaceDesc.Format;
                        desc.Texture2D.MipLevels = 1;
                        CHECK_HRCMD(
                            device->CreateShaderResourceView(windowSurface, &desc, srv.ReleaseAndGetAddressOf()));
                    }
                    ComPtr<ID3D11UnorderedAccessView> uav;
                    {
                        D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};
                        desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
                        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                        desc.Texture2D.MipSlice = 0;
                        CHECK_HRCMD(device->CreateUnorderedAccessView(surface, &desc, uav.ReleaseAndGetAddressOf()));
                    }

                    // Copy while doing transparency.
                    context->CSSetShader(m_transparencyShader.Get(), nullptr, 0);
                    context->CSSetShaderResources(0, 1, srv.GetAddressOf());
                    context->CSSetConstantBuffers(0, 1, m_transparencyConstants.GetAddressOf());
                    context->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
                    context->Dispatch((unsigned int)std::ceil(windowSharedSurfaceDesc.Width / 8),
                                      (unsigned int)std::ceil(windowSharedSurfaceDesc.Height / 8),
                                      1);

                    // Unbind all resources to avoid D3D validation errors.
                    {
                        context->CSSetShader(nullptr, nullptr, 0);
                        ID3D11ShaderResourceView* nullSRV[] = {nullptr};
                        context->CSSetShaderResources(0, 1, nullSRV);
                        ID3D11Buffer* nullCBV[] = {nullptr};
                        context->CSSetConstantBuffers(0, 1, nullCBV);
                        ID3D11UnorderedAccessView* nullUAV[] = {nullptr};
                        context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
                    }
                }
                overlaySwapchain->releaseImage();
                overlaySwapchain->commitLastReleasedImage();

                return true;
            }

            XrSpace localSpace{XR_NULL_HANDLE};
            std::shared_ptr<graphics::ISwapchain> overlaySwapchain;
            XrPosef overlayPose;
            XrExtent2Df overlaySize;

          private:
            OpenXrApi& m_openxr;

            ComPtr<ID3D11ComputeShader> m_transparencyShader;
            PROCESS_INFORMATION m_overlayProcessInfo{};
            std::shared_ptr<capture::ICaptureWindow> m_captureWindow;
            ComPtr<ID3D11Buffer> m_transparencyConstants;
        };

        bool m_bypassApiLayer{false};

        std::shared_ptr<graphics::ICompositionFrameworkFactory> m_compositionFrameworkFactory;
    };

    // This method is required by the framework to instantiate your OpenXrApi implementation.
    OpenXrApi* GetInstance() {
        if (!g_instance) {
            g_instance = std::make_unique<OpenXrLayer>();
        }
        return g_instance.get();
    }

} // namespace openxr_api_layer

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        TraceLoggingRegister(openxr_api_layer::log::g_traceProvider);
        break;

    case DLL_PROCESS_DETACH:
        TraceLoggingUnregister(openxr_api_layer::log::g_traceProvider);
        break;

    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
        break;
    }
    return TRUE;
}
