// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>

#include <fbl/unique_fd.h>
#include <zircon/device/device.h>

namespace {

constexpr char kUsageMessage[] = R"""(
Attempts to unbind (remove) a device from the system.

unbind device

WARNING: In general this is not a safe operation and removing a device may
result in system instability or even a completely unusable system.
)""";

struct Config {
    const char* path;
};

bool GetOptions(int argc, char** argv, Config* config) {
    while (true) {
        struct option options[] = {
            {"help", no_argument, nullptr, 'h'},
            {nullptr, 0, nullptr, 0},
        };
        int opt_index;
        int c = getopt_long(argc, argv, "h", options, &opt_index);
        if (c < 0) {
            break;
        }
        switch (c) {
        case 'h':
            return false;
        }
    }
    if (argc == optind + 1) {
        config->path = argv[optind];
        return true;
    }
    return false;
}

bool ValidateOptions(const Config& config) {
    if (!config.path) {
        printf("Device path needed\n");
        printf("%s\n", kUsageMessage);
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Config config = {};
    if (!GetOptions(argc, argv, &config)) {
        printf("%s\n", kUsageMessage);
        return -1;
    }

    if (!ValidateOptions(config)) {
        return -1;
    }

    fbl::unique_fd device(open(config.path, O_WRONLY));
    if (!device) {
        printf("Unable to open device\n");
        return -1;
    }

    if (ioctl_device_unbind(device.get()) < 0) {
        printf("Failed to unbind device\n");
        return -1;
    }

    printf("Command sent. The device may be gone now\n");
    return 0;
}
