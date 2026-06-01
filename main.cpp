#include <wx/wx.h>
#include <landstalker/main/GameData.h>
#include <landstalker/misc/Labels.h>
#include "MainFrame.h"
#include <filesystem>
#include <vector>

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

wxIMPLEMENT_APP(MyApp);
