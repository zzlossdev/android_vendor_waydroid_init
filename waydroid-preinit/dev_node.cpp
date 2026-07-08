/*
 * Copyright (C) 2026 The WayDroid-ATV Project
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <array>
#include <string>
#include <system_error>
#include <filesystem>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <mntent.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>

#include "log.h"
#include "utils.h"
#include "android-uid.h"
#include "dev_node.h"

namespace DevNode {
    static constexpr std::array<entryInfo, 10> entryPerms = {{
        { "/dev",                 AID_ROOT,   AID_SYSTEM, 01775 },
        { "/dev/device-mapper",   AID_ROOT,   AID_ROOT,   00600 },
        { "/dev/loop-control",    AID_ROOT,   AID_ROOT,   00600 },
        { "/dev/fuse",            AID_ROOT,   AID_ROOT,   00600 },
        { "/dev/tun",             AID_SYSTEM, AID_VPN,    00660 },
        { "/dev/uhid",            AID_UHID,   AID_UHID,   00660 },
        { "/dev/dma_heap/system", AID_SYSTEM, AID_SYSTEM, 00444 },
    }};

    static constexpr std::array<entryInfo, 4> entryPermsForPrefix = {{
        { "/dev/media",      AID_SYSTEM, AID_CAMERA,   0660 },
        { "/dev/video",      AID_SYSTEM, AID_CAMERA,   0660 },
        { "/dev/dri/card",   AID_ROOT,   AID_GRAPHICS, 0666 },
        { "/dev/dri/render", AID_ROOT,   AID_GRAPHICS, 0666 },
    }};

    static constexpr std::array<std::string, 3> hostBinderNodes = {
        "/dev/host_binder",
        "/dev/host_hwbinder",
    };

    void fixPermission() {
        std::error_code error;

        for (const auto &ent : entryPerms) {
            if (!std::filesystem::exists(ent.path)) {
                continue;
            } else if (!Utils::setFilePermission(ent.path, ent.owner, ent.group, ent.mode)) {
                Log::warn("Failed to setup file permission for {}", ent.path);
            }
        }

        for (const auto &content : std::filesystem::recursive_directory_iterator("/dev", error)) {
            for (const auto &ent : entryPermsForPrefix) {
                std::string path = content.path().string();

                if (!path.starts_with(ent.path)) {
                    continue;
                } else if (!Utils::setFilePermission(path, ent.owner, ent.group, ent.mode)) {
                    Log::warn("Failed to setup file permission for {}", path);
                }
            }
        }
    }

    void fixBindMountedNodes() {
        FILE          *mounts = fopen("/proc/mounts", "r");
        struct mntent *mntEntry;

        if (mounts == NULL) {
            Log::warn("Failed to open /proc/mounts: {}", strerror(errno));
            return;
        }

        while ((mntEntry = getmntent(mounts)) != NULL) {
            if (strncmp(mntEntry->mnt_dir, "/dev/", 5) == 0) {
                struct stat nodeInfo;

                // Skip binder nodes
                if (std::find(std::begin(binderNodes), std::end(binderNodes), mntEntry->mnt_dir) != std::end(binderNodes)) {
                    continue;
                }
                // Same for host binder nodes on Halium devices
                if (std::find(std::begin(hostBinderNodes), std::end(hostBinderNodes), mntEntry->mnt_dir) != std::end(hostBinderNodes)) {
                    continue;
                }

                if (stat(mntEntry->mnt_dir, &nodeInfo) == -1) {
                    Log::warn("stat() failed for {}: {}", mntEntry->mnt_dir, strerror(errno));
                    continue;
                }

                if (!(S_ISCHR(nodeInfo.st_mode) || S_ISBLK(nodeInfo.st_mode))) continue;

                Log::info("fixBindMountedNodes: unmount {}", mntEntry->mnt_dir);

                // Unmount first
                if (umount2(mntEntry->mnt_dir, MNT_DETACH) == -1) {
                    Log::warn("Failed to unmount {}: {}", mntEntry->mnt_dir, strerror(errno));
                    continue;
                }

                // Remove dummy file created by LXC
                if (remove(mntEntry->mnt_dir) == -1) {
                    Log::warn("Failed to remove {}: {}", mntEntry->mnt_dir, strerror(errno));
                    continue;
                }

                // Recreate node with mknod
                if (mknod(mntEntry->mnt_dir, nodeInfo.st_mode, nodeInfo.st_rdev) == -1) {
                    Log::warn("Failed to mknod {}: {}", mntEntry->mnt_dir, strerror(errno));
                    continue;
                }
            }
        }

        fclose(mounts);
    }
}
