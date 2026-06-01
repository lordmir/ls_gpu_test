#include "GLCanvas.h"
#include "GLLoader.h"
#include <wx/dcclient.h>
#include <iostream>

using namespace Landstalker;

wxBEGIN_EVENT_TABLE(MyGLCanvas, wxGLCanvas)
    EVT_PAINT(MyGLCanvas::OnPaint)
    EVT_TIMER(wxID_ANY, MyGLCanvas::OnTimer)
    EVT_KEY_DOWN(MyGLCanvas::OnKeyDown)
wxEND_EVENT_TABLE()

MyGLCanvas::MyGLCanvas(wxWindow* parent, std::shared_ptr<GameData> gd)
    : wxGLCanvas(parent, wxID_ANY, nullptr, wxDefaultPosition, wxDefaultSize, wxFULL_REPAINT_ON_RESIZE),
      m_gd(gd), m_mapRenderer(gd), m_spriteRenderer(gd),
      m_frame_count(0), m_fps(0.0f), m_current_room(0), m_cam_x(0.0f), m_cam_y(0.0f), m_show_heightmap(false),
      m_timer(this), m_initialized(false)
{
    m_context = new wxGLContext(this);
    SetCurrent(*m_context);
    InitGLLoader();

    m_current_room = rand() % m_gd->GetRoomData()->GetRoomCount();
    m_fps_stopwatch.Start();
    m_timer.Start(16);
}

MyGLCanvas::~MyGLCanvas() {
    delete m_context;
}

void MyGLCanvas::LoadRoom(uint16_t roomnum) {
    m_current_room = roomnum;
    m_mapRenderer.LoadRoom(roomnum);
    m_instances.clear();
    auto sd = m_gd->GetSpriteData();
    auto entities = sd->GetRoomEntities(roomnum);
    float mat[9] = { 32.0f, 16.0f, 0.0f, -32.0f, 16.0f, 0.0f, 512.0f, 100.0f, 1.0f };
    for (const auto& e : entities) {
        float ex_block = (float(e.GetX()) - m_mapRenderer.GetRoomLeft() * 256.0f) / 256.0f;
        float ey_block = (float(e.GetY()) - m_mapRenderer.GetRoomTop() * 256.0f) / 256.0f;
        float ez_block = float(e.GetZ()) / 256.0f;
        float px = mat[0] * ex_block + mat[3] * ey_block + mat[6];
        float py = mat[1] * ex_block + mat[4] * ey_block + mat[7] - ez_block * 32.0f;
        m_instances.push_back({e.GetType(), px, py, 0.0f, 0.0f, 2.0f, 0.0f, 1.0f, e.GetOrientation()});
    }
    m_cam_x = 0.0f; m_cam_y = 0.0f;
    m_room_stopwatch.Start();
}

void MyGLCanvas::OnTimer(wxTimerEvent&) {
    auto sd = m_gd->GetSpriteData();
    for (auto& inst : m_instances) {
        uint8_t sid = sd->GetSpriteFromEntity(inst.entity_id);
        auto flags = sd->GetSpriteAnimationFlags(sid); auto anims = sd->GetSpriteAnimations(sid);
        bool has_away = !flags.do_not_rotate && !sd->IsEntityItem(inst.entity_id);
        int towards = 0, away = 0;
        if (flags.has_full_animations) { towards = 1; away = 1; }
        if (has_away) { towards = towards * 2 + 1; away = away * 2; }
        int aid = (inst.dy < 0) ? away : towards;
        if (aid >= (int)anims.size()) aid = 0;
        const auto& frames = sd->GetSpriteAnimationFrames(anims[aid]);
        if (!frames.empty() && !sd->IsEntityItem(inst.entity_id)) { 
            inst.anim_timer += inst.anim_speed / 60.0f * 12.0f; 
            if (inst.anim_timer >= frames.size()) inst.anim_timer -= frames.size(); 
        }
    }
    m_frame_count++;
    if (m_fps_stopwatch.Time() >= 1000) {
        m_fps = (m_frame_count*1000.0f)/m_fps_stopwatch.Time(); m_frame_count=0; m_fps_stopwatch.Start();
        auto* f = wxDynamicCast(GetParent(), wxFrame);
        if(f) {
            auto name = m_gd->GetRoomData()->GetRoomDisplayName(m_current_room);
            f->SetStatusText(wxString::Format("FPS: %.2f | Entities: %zu | Room: %d (%ls) | Cam: %.0f, %.0f | HM: %s", 
                m_fps, m_instances.size(), m_current_room, name.c_str(), m_cam_x, m_cam_y, m_show_heightmap ? "ON" : "OFF"));
        }
    }
    Refresh();
}

void MyGLCanvas::OnKeyDown(wxKeyEvent& evt) {
    float speed = 20.0f;
    uint16_t room_count = m_gd->GetRoomData()->GetRoomCount();
    switch(evt.GetKeyCode()) {
        case WXK_LEFT: m_cam_x += speed; break;
        case WXK_RIGHT: m_cam_x -= speed; break;
        case WXK_UP: m_cam_y += speed; break;
        case WXK_DOWN: m_cam_y -= speed; break;
        case WXK_PAGEUP: LoadRoom((m_current_room + 1) % room_count); break;
        case WXK_PAGEDOWN: LoadRoom(m_current_room > 0 ? m_current_room - 1 : room_count - 1); break;
        case 'h':
        case 'H': m_show_heightmap = !m_show_heightmap; break;
    }
    Refresh();
}

void MyGLCanvas::OnPaint(wxPaintEvent&) {
    // This is the entry point for drawing. wxPaintDC is a helper that ensures 
    // the windowing system knows we are drawing.
    wxPaintDC dc(this);
    // Set the current OpenGL context to this window.
    SetCurrent(*m_context);
    
    // Perform one-time initialization of shaders and textures
    if (!m_initialized) {
        m_mapRenderer.Init();
        m_spriteRenderer.Init();
        LoadRoom(m_current_room);
        m_initialized = true;
    }
    
    // Set up the viewport and simple orthographic projection
    int w, h; GetClientSize(&w, &h);
    glViewport(0, 0, w, h); 
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); 
    // This sets up a 2D coordinate system matching the window size in pixels
    glOrtho(0, w, h, 0, -1, 1);
    
    // Clear the screen to a dark blue color
    glMatrixMode(GL_MODELVIEW); glLoadIdentity(); 
    glTranslatef(m_cam_x, m_cam_y, 0.0f); 
    glClearColor(0.1f, 0.1f, 0.15f, 1.0f); 
    glClear(GL_COLOR_BUFFER_BIT);

    // Delegate the actual drawing to specialized renderer classes
    // 1. Draw the isometric map background and foreground
    m_mapRenderer.Render(m_cam_x, m_cam_y, m_show_heightmap);
    // 2. Draw the animated sprites on top
    m_spriteRenderer.Render(m_instances, m_cam_x, m_cam_y);

    // Display the buffer on screen (double buffering)
    SwapBuffers();
}

