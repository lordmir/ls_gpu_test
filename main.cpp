#include <wx/wx.h>
#include <landstalker/main/GameData.h>
#include <landstalker/misc/Labels.h>
#include "MainFrame.h"
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

namespace {

void PreferX11BackendForGlew()
{
#if defined(__linux__)
    const char* display = std::getenv("DISPLAY");
    const char* session_type = std::getenv("XDG_SESSION_TYPE");
    if (!display || display[0] == '\0' || !session_type || std::strcmp(session_type, "wayland") != 0) {
        return;
    }

    setenv("GDK_BACKEND", "x11", 1);
    setenv("XDG_SESSION_TYPE", "x11", 1);
    unsetenv("WAYLAND_DISPLAY");
#endif
}

}

class MyApp : public wxApp {
public:
    virtual bool OnInit() {
        auto gd = std::make_shared<Landstalker::GameData>();
        std::vector<std::string> paths = {
            "landstalker_disasm/landstalker_us.asm",
            "../landstalker_disasm/landstalker_us.asm",
            "../../landstalker_disasm/landstalker_us.asm"
        };
        std::filesystem::path asm_path;
        bool found = false;
        for (const auto& p : paths) {
            if (std::filesystem::exists(p)) {
                asm_path = p;
                found = true;
                break;
            }
        }
        if (!found || !gd->Open(asm_path)) return false;
        Landstalker::Labels::InitDefaults();
        MyFrame* frame = new MyFrame(gd);
        frame->Show();
        return true;
    }
};

wxIMPLEMENT_APP_NO_MAIN(MyApp);

int main(int argc, char** argv)
{
    PreferX11BackendForGlew();
    return wxEntry(argc, argv);
}
