/*
 * Copyright (C) 2026 The WayDroid-ATV Project
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <system_error>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <fnmatch.h>
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

    // Names that may show up in the owner/group fields of a ueventd.rc
    // permissions line; anything else has to be given numerically.
    static constexpr std::array<std::pair<std::string_view, uid_t>, 24> androidIds = {{
        { "audio",          AID_AUDIO          },
        { "bluetooth",      AID_BLUETOOTH      },
        { "camera",         AID_CAMERA         },
        { "drm",            AID_DRM            },
        { "drmrpc",         AID_DRMRPC         },
        { "graphics",       AID_GRAPHICS       },
        { "hsm",            AID_HSM            },
        { "input",          AID_INPUT          },
        { "log",            AID_LOG            },
        { "media",          AID_MEDIA          },
        { "mediacodec",     AID_MEDIA_CODEC    },
        { "mediadrm",       AID_MEDIA_DRM      },
        { "mtp",            AID_MTP            },
        { "nfc",            AID_NFC            },
        { "prng_seeder",    AID_PRNG_SEEDER    },
        { "radio",          AID_RADIO          },
        { "root",           AID_ROOT           },
        { "secure_element", AID_SECURE_ELEMENT },
        { "system",         AID_SYSTEM         },
        { "uhid",           AID_UHID           },
        { "usb",            AID_USB            },
        { "uwb",            AID_UWB            },
        { "vpn",            AID_VPN            },
        { "wifi",           AID_WIFI           },
    }};

    static std::vector<entryInfo> ueventdRules;

    static bool decodeId(const std::string &name, uid_t &id) {
        for (const auto &[knownName, knownId] : androidIds) {
            if (name == knownName) {
                id = knownId;
                return true;
            }
        }

        if (!name.empty() && name.find_first_not_of("0123456789") == std::string::npos) {
            id = static_cast<uid_t>(strtoul(name.c_str(), NULL, 10));
            return true;
        }

        return false;
    }

    // The device tells us what it wants its nodes to look like, so read the same
    // files ueventd does. We cannot leave it to ueventd itself: the nodes are
    // already there by the time it runs, and it only ever fixes up their owner.
    static void parseUeventdConfig(const std::string &path, unsigned depth = 0) {
        std::ifstream config(path);
        std::string   line;

        if (depth > 4) {
            Log::warn("Too many nested imports, ignoring {}", path);
            return;
        }

        if (!config.is_open()) return;

        while (std::getline(config, line)) {
            std::istringstream fields(line);
            std::string        name, mode, owner, group;

            if (!(fields >> name)) continue;

            if (name == "import") {
                if (fields >> mode) parseUeventdConfig(mode, depth + 1);
                continue;
            }

            if (!name.starts_with("/dev/")) continue;
            if (!(fields >> mode >> owner >> group)) continue;

            uid_t uid;
            gid_t gid;

            if (mode.find_first_not_of("01234567") != std::string::npos) {
                Log::warn("Bad mode \"{}\" for {}, ignoring", mode, name);
                continue;
            }

            if (!decodeId(owner, uid) || !decodeId(group, gid)) {
                Log::warn("Unknown owner or group for {}, ignoring", name);
                continue;
            }

            ueventdRules.push_back(
                { name, uid, gid, static_cast<mode_t>(strtoul(mode.c_str(), NULL, 8)) });
        }
    }

    // Matched the way ueventd matches: last rule wins, a trailing '*' is a
    // prefix and any other '*' is a glob.
    static const entryInfo *matchUeventdRule(const std::string &path) {
        for (auto it = ueventdRules.crbegin(); it != ueventdRules.crend(); ++it) {
            const std::string &name = it->path;
            const auto         wildcard = name.find('*');

            if (wildcard == std::string::npos) {
                if (path == name) return &*it;
            } else if (wildcard == name.length() - 1) {
                if (path.starts_with(std::string_view(name).substr(0, wildcard))) return &*it;
            } else if (fnmatch(name.c_str(), path.c_str(), FNM_PATHNAME) == 0) {
                return &*it;
            }
        }

        return NULL;
    }

    void fixPermission() {
        std::error_code error;

        parseUeventdConfig("/system/etc/ueventd.rc");

        for (const auto &ent : entryPerms) {
            if (!std::filesystem::exists(ent.path)) {
                continue;
            } else if (!Utils::setFilePermission(ent.path, ent.owner, ent.group, ent.mode)) {
                Log::warn("Failed to setup file permission for {}", ent.path);
            }
        }

        for (const auto &content : std::filesystem::recursive_directory_iterator("/dev", error)) {
            std::string path = content.path().string();

            // Binder nodes are still bind mounts, so touching them would change
            // the host's own nodes
            if (std::find(std::begin(binderNodes), std::end(binderNodes), path) != std::end(binderNodes) ||
                std::find(std::begin(hostBinderNodes), std::end(hostBinderNodes), path) != std::end(hostBinderNodes)) {
                continue;
            }

            if (const entryInfo *rule = matchUeventdRule(path)) {
                if (!Utils::setFilePermission(path, rule->owner, rule->group, rule->mode)) {
                    Log::warn("Failed to setup file permission for {}", path);
                }
                continue;
            }

            // Fall back to our own defaults for whatever the device says nothing about
            for (const auto &ent : entryPermsForPrefix) {
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
