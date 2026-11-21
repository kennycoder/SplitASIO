#include "../asio_host.h"
#include "../config.h"
#include "../control_panel.h"

#include <objbase.h>
#include <string>
#include <windows.h>

namespace {

class StandalonePanelHost final : public ControlPanelHost {
public:
    StandalonePanelHost() : config_(loadBridgeConfig()) {}

    BridgeConfig getConfig() const override {
        return config_;
    }

    void applyConfig(const BridgeConfig& cfg) override {
        config_ = cfg;
        saveBridgeConfig(cfg);
    }

    void openInputDriverPanel() override {
        openChildPanel(config_.inputDriver);
    }

    void openOutputDriverPanel() override {
        openChildPanel(config_.outputDriver);
    }

    void resetXruns() override {}

    BridgeStatusSnapshot getStatus() const override {
        BridgeStatusSnapshot snap{};
        snap.streaming = false;
        snap.inputDriverName = config_.inputDriver;
        snap.outputDriverName = config_.outputDriver;
        snap.ring.targetFillFrames = static_cast<double>(config_.ringBufferFrames);
        return snap;
    }

private:
    static void openChildPanel(const std::string& driverName) {
        if (driverName.empty()) return;
        AsioChildDriver drv;
        std::string err;
        if (drv.open(driverName, GetDesktopWindow(), err)) {
            drv.raw()->controlPanel();
            drv.close();
        } else {
            MessageBoxA(nullptr, ("Could not open ASIO driver panel:\n" + err).c_str(),
                "SplitASIO Settings", MB_OK | MB_ICONWARNING);
        }
    }

    BridgeConfig config_;
};

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    StandalonePanelHost host;
    ControlPanel panel;
    panel.runModal(&host);

    CoUninitialize();
    return 0;
}
