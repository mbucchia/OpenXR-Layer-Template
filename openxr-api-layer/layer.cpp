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
    using namespace xr::math;

    // The IPD we want to force the application to use.
    constexpr float IPDOverride = 0.09f; // 9cm should make everything look small!

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

            XrResult result = m_bypassApiLayer ? m_xrGetInstanceProcAddr(instance, name, function)
                                               : OpenXrApi::xrGetInstanceProcAddr(instance, name, function);

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
                if (*systemId != m_systemId) {
                    XrSystemProperties systemProperties{XR_TYPE_SYSTEM_PROPERTIES};
                    CHECK_XRCMD(OpenXrApi::xrGetSystemProperties(instance, *systemId, &systemProperties));
                    TraceLoggingWrite(g_traceProvider, "xrGetSystem", TLArg(systemProperties.systemName, "SystemName"));
                    Log(fmt::format("Using OpenXR system: {}\n", systemProperties.systemName));
                }

                // Remember the XrSystemId to use.
                m_systemId = *systemId;
            }

            TraceLoggingWrite(g_traceProvider, "xrGetSystem", TLArg((int)*systemId, "SystemId"));

            return result;
        }

        XrResult xrLocateViews(XrSession session,
                               const XrViewLocateInfo* viewLocateInfo,
                               XrViewState* viewState,
                               uint32_t viewCapacityInput,
                               uint32_t* viewCountOutput,
                               XrView* views) override {
            if (viewLocateInfo->type != XR_TYPE_VIEW_LOCATE_INFO || viewState->type != XR_TYPE_VIEW_STATE) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            TraceLoggingWrite(g_traceProvider,
                              "xrLocateViews",
                              TLXArg(session, "Session"),
                              TLArg(xr::ToCString(viewLocateInfo->viewConfigurationType), "ViewConfigurationType"),
                              TLArg(viewLocateInfo->displayTime, "DisplayTime"),
                              TLXArg(viewLocateInfo->space, "Space"),
                              TLArg(viewCapacityInput, "ViewCapacityInput"));

            // Invoke the real implementation.
            const XrResult result =
                OpenXrApi::xrLocateViews(session, viewLocateInfo, viewState, viewCapacityInput, viewCountOutput, views);

            TraceLoggingWrite(g_traceProvider, "xrLocateViews", TLArg(*viewCountOutput, "ViewCountOutput"));

            if (XR_SUCCEEDED(result) && viewCapacityInput) {
                // If this is a stereoscopic view, apply our IPD override.
                if (viewLocateInfo->viewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) {
                    assert(*viewCountOutput == xr::StereoView::Count);

                    const bool isDeactivateKeyPressed = GetAsyncKeyState(VK_END) < 0;
                    if (!isDeactivateKeyPressed) {
                        // Patch the views with our IPD before returning to the application.
                        // Store the actual IPD as reported by the runtime so we can restore it later in xrEndFrame().
                        m_lastSeenIPD = overrideIPD(
                            views[xr::StereoView::Left].pose, views[xr::StereoView::Right].pose, IPDOverride);
                    } else {
                        m_lastSeenIPD.reset();
                    }
                }

                for (uint32_t i = 0; i < *viewCountOutput; i++) {
                    TraceLoggingWrite(
                        g_traceProvider, "xrLocateViews", TLArg(viewState->viewStateFlags, "ViewStateFlags"));
                    TraceLoggingWrite(g_traceProvider,
                                      "xrLocateViews",
                                      TLArg(xr::ToString(views[i].pose).c_str(), "Pose"),
                                      TLArg(xr::ToString(views[i].fov).c_str(), "Fov"));
                }
            }

            return result;
        }

        XrResult xrEndFrame(XrSession session, const XrFrameEndInfo* frameEndInfo) override {
            if (frameEndInfo->type != XR_TYPE_FRAME_END_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            TraceLoggingWrite(g_traceProvider,
                              "xrEndFrame",
                              TLXArg(session, "Session"),
                              TLArg(frameEndInfo->displayTime, "DisplayTime"),
                              TLArg(xr::ToCString(frameEndInfo->environmentBlendMode), "EnvironmentBlendMode"));

            // We will need to create copies of some structures, because they are passed const from the application so
            // we cannot modify them in-place.
            XrFrameEndInfo chainFrameEndInfo = *frameEndInfo;
            std::vector<XrCompositionLayerProjection> projAllocator;
            projAllocator.reserve(frameEndInfo->layerCount);
            std::vector<std::array<XrCompositionLayerProjectionView, 2>> projViewsAllocator;
            projViewsAllocator.reserve(frameEndInfo->layerCount);
            std::vector<const XrCompositionLayerBaseHeader*> layersPtrAllocator;
            layersPtrAllocator.reserve(frameEndInfo->layerCount);

            for (uint32_t i = 0; i < frameEndInfo->layerCount; i++) {
                if (!frameEndInfo->layers[i]) {
                    return XR_ERROR_LAYER_INVALID;
                }

                TraceLoggingWrite(g_traceProvider,
                                  "xrEndFrame_Layer",
                                  TLArg(xr::ToCString(frameEndInfo->layers[i]->type), "Type"),
                                  TLArg(frameEndInfo->layers[i]->layerFlags, "Flags"),
                                  TLXArg(frameEndInfo->layers[i]->space, "Space"));

                // Patch the IPD back for all projection layers with stereoscopic views.
                if (frameEndInfo->layers[i]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                    const XrCompositionLayerProjection* proj =
                        reinterpret_cast<const XrCompositionLayerProjection*>(frameEndInfo->layers[i]);

                    if (proj->viewCount == xr::StereoView::Count && m_lastSeenIPD) {
                        // Create our copies of the structures we will modify.
                        projAllocator.emplace_back(*proj);
                        auto& patchedProj = projAllocator.back();

                        projViewsAllocator.emplace_back(
                            std::array<XrCompositionLayerProjectionView, 2>{proj->views[0], proj->views[1]});
                        auto& patchedProjViews = projViewsAllocator.back();

                        // Restore the original IPD, otherwise the OpenXR runtime will reproject the altered IPD
                        // into the real IPD.
                        overrideIPD(patchedProjViews[xr::StereoView::Left].pose,
                                    patchedProjViews[xr::StereoView::Right].pose,
                                    m_lastSeenIPD.value());

                        for (uint32_t eye = 0; eye < xr::StereoView::Count; eye++) {
                            TraceLoggingWrite(
                                g_traceProvider,
                                "xrEndFrame_Projection",
                                TLArg(eye, "Index"),
                                TLXArg(patchedProjViews[eye].subImage.swapchain, "Swapchain"),
                                TLArg(patchedProjViews[eye].subImage.imageArrayIndex, "ImageArrayIndex"),
                                TLArg(xr::ToString(patchedProjViews[eye].subImage.imageRect).c_str(), "ImageRect"),
                                TLArg(xr::ToString(patchedProjViews[eye].pose).c_str(), "Pose"),
                                TLArg(xr::ToString(patchedProjViews[eye].fov).c_str(), "Fov"));
                        }

                        patchedProj.views = projViewsAllocator.back().data();

                        // Take our modified projection layer.
                        layersPtrAllocator.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&patchedProj));
                    } else {
                        // Take the unmodified projection layer.
                        layersPtrAllocator.push_back(frameEndInfo->layers[i]);
                    }
                } else {
                    // Take the unmodified layer.
                    layersPtrAllocator.push_back(frameEndInfo->layers[i]);
                }
            }

            // Use our newly formed list of layers.
            chainFrameEndInfo.layers = layersPtrAllocator.data();
            assert(chainFrameEndInfo.layerCount == (uint32_t)layersPtrAllocator.size());

            return OpenXrApi::xrEndFrame(session, &chainFrameEndInfo);
        }

      private:
        float overrideIPD(XrPosef& leftEye, XrPosef& rightEye, float IPD) const {
            const XrVector3f vec = leftEye.position - rightEye.position;
            const XrVector3f center = leftEye.position + (vec * 0.5f);
            const XrVector3f offset = Normalize(vec) * (IPD * 0.5f);
            leftEye.position = center - offset;
            rightEye.position = center + offset;

            return Length(vec);
        }

        bool m_bypassApiLayer{false};
        XrSystemId m_systemId{XR_NULL_SYSTEM_ID};
        std::optional<float> m_lastSeenIPD{};
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
