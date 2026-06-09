#include "SmatchetAndroidPlatform.h"

#include <android/asset_manager.h>
#include <android/configuration.h>

namespace smatchet {
namespace mobile {

std::vector<unsigned char> ReadApkAsset(AAssetManager* assetManager, const char* assetName) {
    std::vector<unsigned char> bytes;
    if (assetManager == nullptr || assetName == nullptr) {
        SLOGE("ReadApkAsset: null asset manager or name");
        return bytes;
    }

    AAsset* asset = AAssetManager_open(assetManager, assetName, AASSET_MODE_BUFFER);
    if (asset == nullptr) {
        SLOGE("ReadApkAsset: asset '%s' not found in APK", assetName);
        return bytes;
    }

    const off_t length = AAsset_getLength(asset);
    if (length > 0) {
        bytes.resize(static_cast<size_t>(length));
        const int read = AAsset_read(asset, bytes.data(), static_cast<size_t>(length));
        if (read != static_cast<int>(length)) {
            SLOGE("ReadApkAsset: short read for '%s' (%d of %ld)", assetName, read, static_cast<long>(length));
            bytes.clear();
        } else {
            SLOG("ReadApkAsset: loaded '%s' (%ld bytes)", assetName, static_cast<long>(length));
        }
    } else {
        SLOGE("ReadApkAsset: asset '%s' is empty", assetName);
    }

    AAsset_close(asset);
    return bytes;
}

float ResolveDensityScale(AConfiguration* config) {
    if (config == nullptr) {
        return 1.0f;
    }
    const int32_t density = AConfiguration_getDensity(config);
    // ACONFIGURATION_DENSITY_DEFAULT (0) / _NONE (0xffff) / _ANY (0xfffe) are not real dpi values.
    if (density <= 0 || density == ACONFIGURATION_DENSITY_NONE || density == ACONFIGURATION_DENSITY_ANY) {
        return 1.0f;
    }
    return static_cast<float>(density) / 160.0f;
}

} // namespace mobile
} // namespace smatchet
