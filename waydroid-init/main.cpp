/*
 * Copyright (C) 2026 The WayDroid-ATV Project
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string>
#include <system_error>
#include <filesystem>
#include <cerrno>
#include <cstring>
#include <unistd.h>
#include <sys/mount.h>
#include <android-base/properties.h>

#include "log.h"
#include "cpu_utils.h"
#include "dalvik_heap.h"
#include "gpu_utils.h"
#include "hwcodecs.h"
#include "settings.h"
#include "ipconfig.h"
#include "utils.h"

#define LOG_TAG "waydroid-init"

#define DMABUF_SYSTEM_HEAP "/dev/dma_heap/system"
#define EMPTY_VINTF_XML "/vendor/etc/vintf/manifest.disabled/empty_vintf.xml"
#define EMPTY_PERMISSION_XML "/vendor/etc/permissions.disabled/empty_permission.xml"
#define WAYDROID_HIDL_XML "/vendor/etc/vintf/manifest/manifest_waydroid.xml"
#define WAYDROID_SETTINGS_FILE "/data/misc/waydroid_settings"

using namespace android;
using namespace std;

inline static void stopService(const string &service) {
    base::SetProperty("ctl.stop", service);
}

inline static void startService(const string &service) {
    base::SetProperty("ctl.start", service);
}

int main(int argc, char **argv) {
    bool             overrideProps = base::GetBoolProperty("ro.waydroid.override_props", true);
    WaydroidSettings settings      = WaydroidSettings(WAYDROID_SETTINGS_FILE, overrideProps);
    error_code       error;

    Log::init(LOG_TAG);
    base::SetProperty("waydroid.init.start", "1");

    // On Halium-based setups the device provides its own GPU/EGL/gralloc
    // stack through vendor blobs (marked by the presence of /vendor_extra)
    if (filesystem::exists("/vendor_extra")) {
        Log::info("Halium vendor (/vendor_extra) detected; skipping GPU auto-detection");
        base::SetProperty("waydroid.init.done", "1");
        return 0;
    }

    if (settings.getBoolSetting("ro.waydroid.running_in_docker")) {
        Log::info("Running in Docker");

        // Generate ipconfig.txt for networking
        if (!IPConfig::generate()) Log::warn("Failed to generate network config");
    }

    bool   swRendering = settings.getBoolSetting("ro.waydroid.software_rendering");
    string renderNode  = settings.getSetting("gralloc.gbm.device");

    if (!swRendering) {
        if (filesystem::exists(renderNode)) {
            Log::info("Using GPU device {}", renderNode);
        } else {
            Log::err("GPU device ({}) does not exist!", renderNode);
            swRendering = true;
        }
    }

    GpuUtils gpuUtils(renderNode, swRendering);

    if (gpuUtils.gpuKernelDriverName.starts_with("nvidia")) {
        Log::warn("Unsupported NVIDIA kernel driver detected");
        settings.updateSetting("ro.waydroid.unsupported_nvidia_kmd", "1");
    }

    settings.updateSetting("ro.hardware.egl",     gpuUtils.driverInfo.eglImpl);
    settings.updateSetting("ro.hardware.gralloc", gpuUtils.getGralloc());

    if (gpuUtils.driverInfo.vkDriverName.has_value()) {
        settings.updateSetting("ro.hardware.vulkan", gpuUtils.driverInfo.vkDriverName.value());
    } else {
        Log::info("Vulkan support is missing");
        settings.updateSetting("ro.hardware.vulkan", "");

        Utils::createBindMount(EMPTY_PERMISSION_XML, "/vendor/etc/permissions/android.hardware.vulkan.level.xml", MS_RDONLY);
        Utils::createBindMount(EMPTY_PERMISSION_XML, "/vendor/etc/permissions/android.hardware.vulkan.version.xml", MS_RDONLY);
        Utils::createBindMount(EMPTY_PERMISSION_XML, "/vendor/etc/permissions/android.hardware.vulkan.compute.xml", MS_RDONLY);
    }

    if (gpuUtils.driverInfo.driverName == "default") {
        // Only default gralloc is supported when using software rendering
        settings.updateSetting("ro.hardware.gralloc", "default");
    } else {
        // Configure c2.ffmpeg
        if (gpuUtils.driverInfo.driverName == "vmwgfx") {
            settings.updateSetting("media.sf.hwaccel", "0");
            settings.updateSetting("debug.ffmpeg-codec2.hwaccel.drm", "0");
            settings.updateSetting("debug.ffmpeg-codec2.pixel_format", "RGB_565");
        } else if (settings.getSetting("ro.hardware.gralloc") == "minigbm_gbm_mesa") {
            settings.updateSetting("media.sf.hwaccel", "1");
            settings.updateSetting("debug.ffmpeg-codec2.hwaccel.drm", "1");
            settings.updateSetting("debug.ffmpeg-codec2.pixel_format", "RGBX_8888");
        } else {
            settings.updateSetting("media.sf.hwaccel", "1");
            settings.updateSetting("debug.ffmpeg-codec2.hwaccel.drm", "1");
            settings.updateSetting("debug.ffmpeg-codec2.pixel_format", "YUV_420");
        }

        settings.updateSetting("ro.waydroid.hwcodecs", HWCodecs(renderNode).getAvailHWCodecs());
    }

    // Set dalvik props based on memory size
    DalvikHeap::setDalvikHeap();

    // Set cgroup cpuset values based on available CPU cores
    CpuUtils cpuinfo;

    settings.updateSetting("ro.cgroup2.cpuset.default", cpuinfo.allCores);
    settings.updateSetting("ro.cgroup2.cpuset.efficient", cpuinfo.efficientCores);

    // Start gralloc
    string grallocImpl = settings.getSetting("ro.hardware.gralloc");

    if (grallocImpl == "default") {
        Log::info("Using software rendering");
        settings.updateSetting("ro.surface_flinger.supports_background_blur", "0");
    } else {
        stopService("vendor.gralloc-2-0");

        if (grallocImpl == "minigbm") {
            startService("vendor.graphics.allocator-4-0");
        } else if (grallocImpl.starts_with("minigbm_")) {
            startService("vendor.graphics.allocator-4-0-" + grallocImpl.substr(8));
        } else {
            startService("vendor.graphics.allocator-4-0-" + grallocImpl);
        }
    }

    // Start hwcomposer
    string hwcomposer = settings.getSetting("ro.hardware.hwcomposer");

    if (hwcomposer == "drm_minigbm") {
        Log::info("Using drm_hwcomposer");
        Utils::createBindMount(EMPTY_VINTF_XML, WAYDROID_HIDL_XML, MS_RDONLY);

        stopService("vendor.hwcomposer-2-1");
        startService("vendor.hwcomposer-2-4");
    } else if (hwcomposer == "redroid") {
        Log::info("Using redroid hwcomposer");
        Utils::createBindMount(EMPTY_VINTF_XML, WAYDROID_HIDL_XML, MS_RDONLY);
    } else {
        Log::info("Using Waydroid hwcomposer");
        settings.updateSetting("ro.hardware.hwcomposer", "waydroid");
    }

    if (!filesystem::exists(DMABUF_SYSTEM_HEAP)) {
        Log::warn("DMA-BUF system heap does not exist, video playback might not work properly");
        Log::warn("Falling back to gralloc");
        settings.updateSetting("debug.stagefright.c2-poolmask", "0xfc0000");
    }

    settings.loadSettings();
    base::SetProperty("waydroid.init.done", "1");

    return 0;
}
