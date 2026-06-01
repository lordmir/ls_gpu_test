#ifndef MAP_RENDERER_H
#define MAP_RENDERER_H

#include <vector>
#include <memory>
#include <string>
#include <landstalker/main/GameData.h>

#ifdef __WXMSW__
#include <windows.h>
#include <GL/gl.h>
#else
#include <GL/gl.h>
#endif

class MapRenderer {
public:
    MapRenderer(std::shared_ptr<Landstalker::GameData> gd);
    ~MapRenderer();

    void Init();
    void LoadRoom(uint16_t roomnum);
    void Render(float cam_x, float cam_y, bool show_heightmap);

    int GetRoomWidth() const { return m_room_w; }
    int GetRoomHeight() const { return m_room_h; }
    int GetRoomLeft() const { return m_room_left; }
    int GetRoomTop() const { return m_room_top; }

private:
    void InitShaders();
    GLuint CreateShader(const std::string& vs_path, const std::string& fs_path);
    std::string LoadShaderFile(const std::string& path);

    std::shared_ptr<Landstalker::GameData> m_gd;
    GLuint m_map_shader_program;
    GLuint m_map_tex_id;
    GLuint m_fg_map_tex_id;
    GLuint m_blockset_tex_id;
    GLuint m_tileset_tex_id;
    GLuint m_room_pal_tex_id;

    int m_room_w, m_room_h, m_room_left, m_room_top;
    uint16_t m_current_room;
};

#endif // MAP_RENDERER_H
