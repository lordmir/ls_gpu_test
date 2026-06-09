#include "GLCanvasLayerEditMode.h"

GLCanvasLayerEditMode::GLCanvasLayerEditMode(MyGLCanvas& canvas)
    : m_canvas(canvas)
{
}

bool GLCanvasLayerEditMode::HandleKeyDown(wxKeyEvent& evt)
{
    bool ctrl = evt.ControlDown();

    switch (evt.GetKeyCode()) {
        case '1':
            m_canvas.SetEditorMode(MyGLCanvas::EditorMode::Room);
            m_canvas.Refresh();
            return true;
        case '2':
            m_canvas.SetEditorMode(MyGLCanvas::EditorMode::Heightmap);
            m_canvas.Refresh();
            return true;
        case '3':
            m_canvas.SetEditorMode(MyGLCanvas::EditorMode::BackgroundLayer);
            m_canvas.Refresh();
            return true;
        case '4':
            m_canvas.SetEditorMode(MyGLCanvas::EditorMode::ForegroundLayer);
            m_canvas.Refresh();
            return true;
        case 'i':
        case 'I':
            m_canvas.m_background_show_block_ids = !m_canvas.m_background_show_block_ids;
            m_canvas.Refresh();
            return true;
        case 'b':
        case 'B':
            if (m_canvas.m_editor_mode == MyGLCanvas::EditorMode::ForegroundLayer) {
                m_canvas.m_foreground_show_background_underlay = !m_canvas.m_foreground_show_background_underlay;
                m_canvas.Refresh();
            }
            return true;
        case WXK_ESCAPE:
            m_canvas.ClearBackgroundClipboard();
            m_canvas.Refresh();
            return true;
        case 'c':
        case 'C':
            m_canvas.CopySelectedBackgroundBlock();
            m_canvas.Refresh();
            return true;
        case WXK_SPACE:
            m_canvas.PasteSelectedBackgroundBlock();
            m_canvas.Refresh();
            return true;
        case 'w':
        case 'W':
            m_canvas.MoveBackgroundSelection(0, -1);
            m_canvas.Refresh();
            return true;
        case 'a':
        case 'A':
            m_canvas.MoveBackgroundSelection(-1, 0);
            m_canvas.Refresh();
            return true;
        case 's':
        case 'S':
            m_canvas.MoveBackgroundSelection(0, 1);
            m_canvas.Refresh();
            return true;
        case 'd':
        case 'D':
            m_canvas.MoveBackgroundSelection(1, 0);
            m_canvas.Refresh();
            return true;
        case WXK_LEFT:
            m_canvas.PanCameraByStep(1, 0);
            m_canvas.Refresh();
            return true;
        case WXK_RIGHT:
            m_canvas.PanCameraByStep(-1, 0);
            m_canvas.Refresh();
            return true;
        case WXK_UP:
            m_canvas.PanCameraByStep(0, 1);
            m_canvas.Refresh();
            return true;
        case WXK_DOWN:
            m_canvas.PanCameraByStep(0, -1);
            m_canvas.Refresh();
            return true;
        case '+':
        case '=':
        case WXK_NUMPAD_ADD:
            if (ctrl) {
                int w = 0;
                int h = 0;
                m_canvas.GetClientSize(&w, &h);
                m_canvas.ChangeZoomStep(1, float(w) * 0.5f, float(h) * 0.5f);
            }
            m_canvas.Refresh();
            return true;
        case '-':
        case WXK_NUMPAD_SUBTRACT:
            if (ctrl) {
                int w = 0;
                int h = 0;
                m_canvas.GetClientSize(&w, &h);
                m_canvas.ChangeZoomStep(-1, float(w) * 0.5f, float(h) * 0.5f);
            }
            m_canvas.Refresh();
            return true;
    }

    return true;
}

void GLCanvasLayerEditMode::HandleMouseMove(const wxMouseEvent& evt)
{
    m_canvas.SelectBackgroundCellAt(evt.GetPosition());
    m_canvas.m_heightmapRenderer.ClearHover();
    m_canvas.SetCursor(wxCursor(wxCURSOR_ARROW));
    m_canvas.Refresh();
}

void GLCanvasLayerEditMode::HandleLeftDown(const wxMouseEvent& evt)
{
    m_canvas.SelectBackgroundCellAt(evt.GetPosition());
    m_canvas.PasteSelectedBackgroundBlock();
    m_canvas.Refresh();
}

void GLCanvasLayerEditMode::HandleRightDown(const wxMouseEvent& evt)
{
    m_canvas.SelectBackgroundCellAt(evt.GetPosition());
    m_canvas.CopySelectedBackgroundBlock();
    m_canvas.Refresh();
}

void GLCanvasLayerEditMode::Render(int width, int height)
{
    if (m_canvas.m_editor_mode == MyGLCanvas::EditorMode::BackgroundLayer) {
        m_canvas.m_mapRenderer.RenderBackgroundOnly();
    } else {
        if (m_canvas.m_foreground_show_background_underlay) {
            m_canvas.m_mapRenderer.RenderBackgroundWithOpacity(0.25f);
        }
        m_canvas.m_mapRenderer.RenderForegroundOnly();
    }

    if (m_canvas.m_background_has_selection && m_canvas.m_background_clipboard_valid) {
        uint16_t current_block = m_canvas.SelectedBackgroundBlockId();
        if (current_block != m_canvas.m_background_clipboard_block_id) {
            m_canvas.m_mapRenderer.RenderBlockGhost(
                m_canvas.m_background_clipboard_block_id,
                m_canvas.m_background_selected_x,
                m_canvas.m_background_selected_y,
                0.45f,
                m_canvas.CurrentEditLayer());
        }
    }

    m_canvas.RenderBackgroundEditorOverlay(width, height);
    m_canvas.SwapBuffers();
}
