#ifndef GL_CANVAS_H
#define GL_CANVAS_H

#include <wx/wx.h>
#include <wx/glcanvas.h>
#include <wx/stopwatch.h>
#include <memory>
#include <vector>
#include <map>
#include <landstalker/main/GameData.h>
#include "SpriteInstance.h"
#include "MapRenderer.h"
#include "SpriteRenderer.h"

class MyGLCanvas : public wxGLCanvas {
public:
    MyGLCanvas(wxWindow* parent, std::shared_ptr<Landstalker::GameData> gd);
    virtual ~MyGLCanvas();

    void LoadRoom(uint16_t roomnum);

private:
    void OnTimer(wxTimerEvent& evt);
    void OnKeyDown(wxKeyEvent& evt);
    void OnPaint(wxPaintEvent& evt);

    std::shared_ptr<Landstalker::GameData> m_gd;
    wxGLContext* m_context;
    
    MapRenderer m_mapRenderer;
    SpriteRenderer m_spriteRenderer;

    std::vector<SpriteInstance> m_instances;
    wxTimer m_timer;
    wxStopWatch m_fps_stopwatch;
    wxStopWatch m_room_stopwatch;
    long m_frame_count;
    float m_fps;
    bool m_show_heightmap;
    uint16_t m_current_room;
    float m_cam_x, m_cam_y;
    bool m_initialized;

    wxDECLARE_EVENT_TABLE();
};

#endif // GL_CANVAS_H
