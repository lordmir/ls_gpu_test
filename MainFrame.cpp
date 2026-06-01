#include "MainFrame.h"
#include "GLCanvas.h"

MyFrame::MyFrame(std::shared_ptr<Landstalker::GameData> gd)
    : wxFrame(nullptr, wxID_ANY, "Landstalker GPU Stress Test (Scale 2x + Pan)", wxDefaultPosition, wxSize(1024, 768))
{
    CreateStatusBar();
    new MyGLCanvas(this, gd);
}
