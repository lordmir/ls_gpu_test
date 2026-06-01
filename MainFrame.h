#ifndef MAIN_FRAME_H
#define MAIN_FRAME_H

#include <wx/wx.h>
#include <memory>
#include <landstalker/main/GameData.h>

class MyFrame : public wxFrame {
public:
    MyFrame(std::shared_ptr<Landstalker::GameData> gd);
};

#endif // MAIN_FRAME_H
