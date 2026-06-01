#include "MapRenderer.h"
#include "GLLoader.h"
#include <landstalker/main/GameData.h>
#include <fstream>
#include <sstream>
#include <iostream>

using namespace Landstalker;

MapRenderer::MapRenderer(std::shared_ptr<GameData> gd)
    : m_gd(gd), m_map_shader_program(0), m_map_tex_id(0), m_fg_map_tex_id(0),
      m_blockset_tex_id(0), m_tileset_tex_id(0), m_room_pal_tex_id(0),
      m_room_w(0), m_room_h(0), m_room_left(0), m_room_top(0), m_current_room(0)
{
}

MapRenderer::~MapRenderer() {
    if (m_map_tex_id) glDeleteTextures(1, &m_map_tex_id);
    if (m_fg_map_tex_id) glDeleteTextures(1, &m_fg_map_tex_id);
    if (m_blockset_tex_id) glDeleteTextures(1, &m_blockset_tex_id);
    if (m_tileset_tex_id) glDeleteTextures(1, &m_tileset_tex_id);
    if (m_room_pal_tex_id) glDeleteTextures(1, &m_room_pal_tex_id);
}

void MapRenderer::Init() {
    InitShaders();
}

void MapRenderer::LoadRoom(uint16_t roomnum) {
    auto rd = m_gd->GetRoomData(); auto room = rd->GetRoom(roomnum); if (!room) return;
    auto map_entry = rd->GetMapForRoom(roomnum); if (!map_entry) return;
    auto map = map_entry->GetData(); auto tileset_entry = rd->GetTilesetForRoom(roomnum);
    if(!tileset_entry) return; auto tileset = tileset_entry->GetData();
    auto blockset = rd->GetCombinedBlocksetForRoom(roomnum); 
    auto palette_entry = rd->GetPaletteForRoom(roomnum); if(!palette_entry) return;
    auto pal = palette_entry->GetData();
    
    m_current_room = roomnum;
    m_room_w = map->GetWidth(); m_room_h = map->GetHeight();
    m_room_left = map->GetLeft(); m_room_top = map->GetTop();
    const auto& map_data = map->GetLayerData(Tilemap3D::Layer::BG);
    std::vector<uint8_t> map_rgba(m_room_w * m_room_h * 4, 0);
    for(size_t i=0; i<map_data.size(); ++i) {
        map_rgba[i*4 + 0] = map_data[i] & 0xFF;
        map_rgba[i*4 + 1] = (map_data[i] >> 8) & 0xFF;
    }
    if (m_map_tex_id) glDeleteTextures(1, &m_map_tex_id);
    glGenTextures(1, &m_map_tex_id); glBindTexture(GL_TEXTURE_2D, m_map_tex_id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, m_room_w, m_room_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, map_rgba.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    const auto& fg_map_data = map->GetLayerData(Tilemap3D::Layer::FG);
    std::vector<uint8_t> fg_map_rgba(m_room_w * m_room_h * 4, 0);
    for(size_t i=0; i<fg_map_data.size(); ++i) {
        fg_map_rgba[i*4 + 0] = fg_map_data[i] & 0xFF;
        fg_map_rgba[i*4 + 1] = (fg_map_data[i] >> 8) & 0xFF;
    }
    if (m_fg_map_tex_id) glDeleteTextures(1, &m_fg_map_tex_id);
    glGenTextures(1, &m_fg_map_tex_id); glBindTexture(GL_TEXTURE_2D, m_fg_map_tex_id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, m_room_w, m_room_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, fg_map_rgba.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    std::vector<uint8_t> bs_data(2 * blockset->size() * 2 * 4, 0);
    for(size_t i=0; i<blockset->size(); ++i) {
        for(int ty=0; ty<2; ++ty) for(int tx=0; tx<2; ++tx) {
            uint16_t val = blockset->at(i).GetTile(tx, ty).GetTileValue();
            int base = (i * 2 + ty) * 2 * 4 + tx * 4;
            bs_data[base + 0] = val & 0xFF; bs_data[base + 1] = (val >> 8) & 0xFF;
        }
    }
    if (m_blockset_tex_id) glDeleteTextures(1, &m_blockset_tex_id);
    glGenTextures(1, &m_blockset_tex_id); glBindTexture(GL_TEXTURE_2D, m_blockset_tex_id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, blockset->size() * 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, bs_data.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    std::vector<uint8_t> ts_data;
    for(size_t i=0; i<tileset->GetTileCount(); ++i) {
        auto pix = tileset->GetTile(Tile(i)); ts_data.insert(ts_data.end(), pix.begin(), pix.end());
    }
    if (m_tileset_tex_id) glDeleteTextures(1, &m_tileset_tex_id);
    glGenTextures(1, &m_tileset_tex_id); glBindTexture(GL_TEXTURE_2D, m_tileset_tex_id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, 8, ts_data.size() / 8, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, ts_data.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    std::vector<uint8_t> rpal_data(16 * 4, 0);
    for(int i=0; i<16; ++i) {
        uint32_t c = pal->getRGBA(i);
        rpal_data[i*4 + 0] = (c >> 16) & 0xFF; rpal_data[i*4 + 1] = (c >> 8) & 0xFF;
        rpal_data[i*4 + 2] = (c >> 0) & 0xFF; rpal_data[i*4 + 3] = (c >> 24) & 0xFF;
    }
    if (m_room_pal_tex_id) glDeleteTextures(1, &m_room_pal_tex_id);
    glGenTextures(1, &m_room_pal_tex_id); glBindTexture(GL_TEXTURE_2D, m_room_pal_tex_id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 16, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, rpal_data.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

void MapRenderer::Render(float cam_x, float cam_y, bool show_heightmap) {
    // This 3x3 matrix defines the isometric transformation.
    // In Landstalker, moving 1 block along the map's X axis translates to (32, 16) pixels on screen.
    // Moving 1 block along the map's Y axis translates to (-32, 16) pixels on screen.
    // The (512, 100) is the global screen offset to center the map.
    float mat[9] = { 
        32.0f, 16.0f, 0.0f,   // Column 0: Map X -> Screen (X, Y)
       -32.0f, 16.0f, 0.0f,   // Column 1: Map Y -> Screen (X, Y)
       512.0f, 100.0f, 1.0f   // Column 2: Origin Offset
    };

    if (m_map_tex_id && m_map_shader_program) {
        // Activate our map shader and bind all required textures
        glUseProgram(m_map_shader_program);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, m_map_tex_id); glUniform1i(glGetUniformLocation(m_map_shader_program, "u_map"), 0);
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, m_blockset_tex_id); glUniform1i(glGetUniformLocation(m_map_shader_program, "u_blockset"), 1);
        glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, m_tileset_tex_id); glUniform1i(glGetUniformLocation(m_map_shader_program, "u_tileset"), 2);
        glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, m_room_pal_tex_id); glUniform1i(glGetUniformLocation(m_map_shader_program, "u_palette"), 3);
        glUniform2f(glGetUniformLocation(m_map_shader_program, "u_map_size"), (float)m_room_w, (float)m_room_h);
        
        glUniform1i(glGetUniformLocation(m_map_shader_program, "u_num_tiles"), (int)(m_gd->GetRoomData()->GetTilesetForRoom(m_current_room)->GetData()->GetTileCount()));
        glUniform1i(glGetUniformLocation(m_map_shader_program, "u_num_blocks"), (int)(m_gd->GetRoomData()->GetCombinedBlocksetForRoom(m_current_room)->size()));
        
        // --- Draw Background Layer ---
        // We iterate through every block in the room grid and draw a square quad for each.
        // The shader will handle the complex mapping of these quads into the isometric diamond shapes.
        glBegin(GL_QUADS);
        for (int y = 0; y < m_room_h; ++y) {
            for (int x = 0; x < m_room_w; ++x) {
                // Apply the isometric matrix to get the screen position for this block (x, y)
                float px = mat[0] * x + mat[3] * y + mat[6];
                float py = mat[1] * x + mat[4] * y + mat[7];
                
                // Draw a 32x32 square. We pass the map coordinates (x, y) as texture coordinates
                // so the fragment shader knows which part of the map it's rendering.
                glTexCoord2f((float)x, (float)y); glVertex2f(px, py);
                glTexCoord2f((float)x + 1.0f, (float)y); glVertex2f(px + 32.0f, py);
                glTexCoord2f((float)x + 1.0f, (float)y + 1.0f); glVertex2f(px + 32.0f, py + 32.0f);
                glTexCoord2f((float)x, (float)y + 1.0f); glVertex2f(px, py + 32.0f);
            }
        }
        glEnd();

        // --- Draw Foreground Layer ---
        if (m_fg_map_tex_id) {
            glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, m_fg_map_tex_id);
            glBegin(GL_QUADS);
            for (int y = 0; y < m_room_h; ++y) {
                for (int x = 0; x < m_room_w; ++x) {
                    // The foreground layer is offset by -32px in X (1 block) in Landstalker's engine
                    float px = mat[0] * x + mat[3] * y + mat[6] - 32.0f;
                    float py = mat[1] * x + mat[4] * y + mat[7];
                    glTexCoord2f((float)x, (float)y); glVertex2f(px, py);
                    glTexCoord2f((float)x + 1.0f, (float)y); glVertex2f(px + 32.0f, py);
                    glTexCoord2f((float)x + 1.0f, (float)y + 1.0f); glVertex2f(px + 32.0f, py + 32.0f);
                    glTexCoord2f((float)x, (float)y + 1.0f); glVertex2f(px, py + 32.0f);
                }
            }
            glEnd();
        }

        // --- Draw Heightmap Overlay ---
        if (show_heightmap) {
            // Disable shaders and textures to draw raw colored primitives
            glUseProgram(0);
            glDisable(GL_TEXTURE_2D);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            
            auto map = m_gd->GetRoomData()->GetMapForRoom(m_current_room)->GetData();
            for (int y = 0; y < m_room_h; ++y) {
                for (int x = 0; x < m_room_w; ++x) {
                    float px = mat[0] * x + mat[3] * y + mat[6];
                    float py = mat[1] * x + mat[4] * y + mat[7];
                    
                    // Fetch height from the 64x64 global room heightmap
                    uint8_t h = map->GetHeight({x + m_room_left, y + m_room_top});
                    if (h != 0xFF) {
                        float cx = px + 16.0f;
                        float cy = py + 16.0f;
                        
                        // Draw a semi-transparent green diamond
                        glColor4f(0.0f, 1.0f, 0.0f, 0.4f);
                        glBegin(GL_QUADS);
                        glVertex2f(cx, cy - 8.0f); glVertex2f(cx + 16.0f, cy);
                        glVertex2f(cx, cy + 8.0f); glVertex2f(cx - 16.0f, cy);
                        glEnd();
                        
                        // Draw a solid grey outline
                        glColor4f(0.5f, 0.5f, 0.5f, 1.0f);
                        glLineWidth(1.0f);
                        glBegin(GL_LINE_LOOP);
                        glVertex2f(cx, cy - 8.0f); glVertex2f(cx + 16.0f, cy);
                        glVertex2f(cx, cy + 8.0f); glVertex2f(cx - 16.0f, cy);
                        glEnd();
                    }
                }
            }
            glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
            glEnable(GL_TEXTURE_2D);
        }
        glUseProgram(0);
    }
}


void MapRenderer::InitShaders() {
    m_map_shader_program = CreateShader("shaders/map.vert", "shaders/map.frag");
}

GLuint MapRenderer::CreateShader(const std::string& vs_path, const std::string& fs_path) {
    std::string vs_src_str = LoadShaderFile(vs_path);
    std::string fs_src_str = LoadShaderFile(fs_path);
    const char* vs_src = vs_src_str.c_str();
    const char* fs_src = fs_src_str.c_str();

    GLuint vs = glCreateShader(GL_VERTEX_SHADER); glShaderSource(vs, 1, &vs_src, nullptr); glCompileShader(vs);
    GLint status; glGetShaderiv(vs, GL_COMPILE_STATUS, &status);
    if (status == GL_FALSE) { char log[512]; glGetShaderInfoLog(vs, 512, nullptr, log); std::cerr << "VS Error (" << vs_path << "): " << log << std::endl; }
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER); glShaderSource(fs, 1, &fs_src, nullptr); glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &status);
    if (status == GL_FALSE) { char log[512]; glGetShaderInfoLog(fs, 512, nullptr, log); std::cerr << "FS Error (" << fs_path << "): " << log << std::endl; }
    GLuint prog = glCreateProgram(); glAttachShader(prog, vs); glAttachShader(prog, fs); glLinkProgram(prog);
    glGetProgramiv(prog, GL_LINK_STATUS, &status);
    if (status == GL_FALSE) { char log[512]; glGetProgramInfoLog(prog, 512, nullptr, log); std::cerr << "Link Error: " << log << std::endl; }
    return prog;
}

std::string MapRenderer::LoadShaderFile(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        std::cerr << "Failed to open shader file: " << path << std::endl;
        return "";
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
