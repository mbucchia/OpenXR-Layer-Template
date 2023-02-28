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

namespace openxr_api_layer {

    using namespace log;
    using namespace utils;
    using namespace xr::math;

    // Our API layer implement these extensions, and their specified version.
    const std::vector<std::pair<std::string, uint32_t>> advertisedExtensions = {};

    // Initialize these vectors with arrays of extensions to block and implicitly request for the instance.
    const std::vector<std::string> blockedExtensions = {};
    const std::vector<std::string> implicitExtensions = {};

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
            std::vector<const XrCompositionLayerBaseHeader*> layers;
            XrCompositionLayerQuad overlay{XR_TYPE_COMPOSITION_LAYER_QUAD};

            // Handle the overlay.
            graphics::ICompositionFramework* composition =
                m_compositionFrameworkFactory->getCompositionFramework(session);
            if (composition) {
                CompositionData* compositionData = composition->getSessionData<CompositionData>();

                // First time: initialize the resources for the session.
                if (!compositionData) {
                    // Allocate storage for the state.
                    composition->setSessionData(std::move(std::make_unique<CompositionData>(*this)));
                    compositionData = composition->getSessionData<CompositionData>();

                    // Create a swapchain for the overlay.
                    XrSwapchainCreateInfo overlaySwapchainInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
                    overlaySwapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
                    overlaySwapchainInfo.arraySize = 1;
                    overlaySwapchainInfo.width = 512;
                    overlaySwapchainInfo.height = 512;
                    overlaySwapchainInfo.format =
                        composition->getPreferredSwapchainFormatOnApplicationDevice(overlaySwapchainInfo.usageFlags);
                    overlaySwapchainInfo.mipCount = overlaySwapchainInfo.sampleCount = overlaySwapchainInfo.faceCount =
                        1;
                    compositionData->overlaySwapchain = composition->createSwapchain(
                        overlaySwapchainInfo, graphics::SwapchainMode::Write | graphics::SwapchainMode::Submit);

                    // Create a head-locked reference space.
                    XrReferenceSpaceCreateInfo createViewSpaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
                    createViewSpaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
                    createViewSpaceInfo.poseInReferenceSpace = Pose::Identity();
                    CHECK_XRCMD(OpenXrApi::xrCreateReferenceSpace(
                        composition->getSessionHandle(), &createViewSpaceInfo, &compositionData->viewSpace));
                }

                // Draw the overlay content.
                const XrSwapchainCreateInfo& swapchainInfo =
                    compositionData->overlaySwapchain->getInfoOnCompositionDevice();
                graphics::ISwapchainImage* const acquiredImage = compositionData->overlaySwapchain->acquireImage();
                {
                    ID3D11Device* const device =
                        composition->getCompositionDevice()->getNativeDevice<graphics::D3D11>();
                    ID3D11DeviceContext* const context =
                        composition->getCompositionDevice()->getNativeContext<graphics::D3D11>();
                    ID3D11Texture2D* const surface =
                        acquiredImage->getTextureForWrite()->getNativeTexture<graphics::D3D11>();

                    // Create an ephemeral render target view for the drawing.
                    ComPtr<ID3D11RenderTargetView> rtv;
                    D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
                    rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                    rtvDesc.Format = (DXGI_FORMAT)swapchainInfo.format;
                    rtvDesc.Texture2D.MipSlice = D3D11CalcSubresource(0, 0, 1);
                    CHECK_HRCMD(device->CreateRenderTargetView(surface, &rtvDesc, rtv.ReleaseAndGetAddressOf()));

                    // Draw to the surface.
                    // We keep the drawing code very simple for the sake of the exercise, but really any D3D11 technique
                    // could be used.
                    ComPtr<ID3D11DeviceContext1> context1;
                    CHECK_HRCMD(context->QueryInterface(context1.ReleaseAndGetAddressOf()));
                    context1->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);

                    const float background[4] = {0.0f, 0.0f, 0.0f, 0.2f};
                    const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
                    const float green[4] = {0.0f, 1.0f, 0.0f, 1.0f};
                    context1->ClearRenderTargetView(rtv.Get(), background);

                    D3D11_RECT rect1;
                    rect1.left = 10;
                    rect1.top = 10;
                    rect1.right = swapchainInfo.width / 2 - 10;
                    rect1.bottom = swapchainInfo.height - 10;
                    context1->ClearView(rtv.Get(), red, &rect1, 1);

                    D3D11_RECT rect2;
                    rect2.left = swapchainInfo.width / 2 + 10;
                    rect2.top = 10;
                    rect2.right = swapchainInfo.width - 10;
                    rect2.bottom = swapchainInfo.height - 10;
                    context1->ClearView(rtv.Get(), green, &rect2, 1);

                    ID3D11RenderTargetView* nullRTV = nullptr;
                    context1->OMSetRenderTargets(1, &nullRTV, nullptr);
                }
                compositionData->overlaySwapchain->releaseImage();
                compositionData->overlaySwapchain->commitLastReleasedImage();

                overlay.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                overlay.subImage = compositionData->overlaySwapchain->getSubImage();

                // Place the overlay.
                // - Head-locked, since we are using XR_REFERENCE_SPACE_TYPE_VIEW;
                // - 1m in front of the user, facing the user (no rotation);
                // - 0.8m x 0.6m dimensions.
                overlay.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                overlay.pose = Pose::Translation({0.0f, 0.0f, -1.0f});
                overlay.space = compositionData->viewSpace;
                overlay.size.width = 0.8f;
                overlay.size.height = 0.6f;

                // Append our overlay quad layer.
                layers.assign(chainFrameEndInfo.layers, chainFrameEndInfo.layers + chainFrameEndInfo.layerCount);
                layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&overlay));
                chainFrameEndInfo.layers = layers.data();
                chainFrameEndInfo.layerCount = (uint32_t)layers.size();
            }

            return OpenXrApi::xrEndFrame(session, &chainFrameEndInfo);
        }

      private:
        struct CompositionData : graphics::ICompositionSessionData {
            CompositionData(OpenXrApi& openxr) : m_openxr(openxr) {
            }

            ~CompositionData() override {
                if (viewSpace != XR_NULL_HANDLE) {
                    m_openxr.xrDestroySpace(viewSpace);
                }
            }

            XrSpace viewSpace{XR_NULL_HANDLE};
            std::shared_ptr<graphics::ISwapchain> overlaySwapchain;

          private:
            OpenXrApi& m_openxr;
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
