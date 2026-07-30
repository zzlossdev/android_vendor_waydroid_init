/*
 * Copyright (C) 2026 The WayDroid-ATV Project
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once
#include <string>
#include <map>
#include <vector>
#include <fstream>

class WaydroidProp {
    private:
        std::fstream propFile;

        std::map<std::string, std::string> properties = {
            { "gralloc.gbm.device",          "/dev/dri/renderD128" },
            { "sys.use_memfd",               "true"                },
            { "waydroid.background_start",   "false"               },
            { "waydroid.pulse_runtime_path", "/run/xdg/pulse"      },
            { "waydroid.wayland_display",    "wayland-0"           },
            { "waydroid.xdg_runtime_dir",    "/run/xdg"            },
        };

    public:
        WaydroidProp(const std::vector<std::string> &props);
        std::string getProperty(const std::string &prop) const;
        void setProperty(const std::string &prop, const std::string &value);
        void saveProperties();
};
