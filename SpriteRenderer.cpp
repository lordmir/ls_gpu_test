#include "SpriteRenderer.h"
#include "GLLoader.h"
#include <landstalker/sprites/SpriteFrame.h>
#include <landstalker/palettes/Palette.h>
#include <landstalker/main/ImageBuffer.h>
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>

using namespace Landstalker;

SpriteRenderer::SpriteRenderer(std::shared_ptr<GameData> gd)
    : m_gd(gd), m_texture_id(0), m_pal_texture_id(0), m_shader_program(0), m_tex_w(0), m_tex_h(0)
{
}

SpriteRenderer::~SpriteRenderer() {
    if (m_texture_id) glDeleteTextures(1, &m_texture_id);
    if (m_pal_texture_id) glDeleteTextures(1, &m_pal_texture_id);
}

void SpriteRenderer::Init() {
    InitTexture();
    InitShaders();
}

void SpriteRenderer::Render(const std::vector<SpriteInstance>& instances, float cam_x, float cam_y) {
    if (m_texture_id && m_shader_program) {
        // Activate sprite shader and bind the atlas and palette textures
        glUseProgram(m_shader_program);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, m_texture_id); glUniform1i(glGetUniformLocation(m_shader_program, "u_atlas"), 0);
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, m_pal_texture_id); glUniform1i(glGetUniformLocation(m_shader_program, "u_palettes"), 1);
        
        // Sprites usually have transparency, so we enable blending
        glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        
        auto sd = m_gd->GetSpriteData();
        for (const auto& inst : instances) {
            uint8_t sid = sd->GetSpriteFromEntity(inst.entity_id);
            auto flags = sd->GetSpriteAnimationFlags(sid); auto anims = sd->GetSpriteAnimations(sid);
            
            // Logic to determine which animation set (towards vs away) to use based on orientation
            bool has_away = !flags.do_not_rotate && !sd->IsEntityItem(inst.entity_id);
            int towards = 0, away = 0;
            if (flags.has_full_animations) { towards = 1; away = 1; }
            if (has_away) { towards = towards * 2 + 1; away = away * 2; }
            int aid = (inst.orientation == Orientation::NE || inst.orientation == Orientation::NW) ? away : towards;
            if (aid >= (int)anims.size()) aid = 0;
            
            const auto& frames = m_sprite_meta[sid].animations[aid]; if (frames.empty()) continue;
            
            // Get the metadata for the current frame of animation
            int fidx = static_cast<int>(inst.anim_timer); if (fidx < 0 || fidx >= (int)frames.size()) fidx = 0;
            const auto& meta = frames[fidx];
            
            // Handle horizontal flipping for NW/SE orientations
            bool flip = flags.do_not_rotate ? false : (inst.orientation == Orientation::SE || inst.orientation == Orientation::NW);
            float sx = inst.x + (flip ? -(meta.origin_x + meta.width) : meta.origin_x) * inst.scale;
            float sy = inst.y + meta.origin_y * inst.scale, sw = meta.width * inst.scale, sh = meta.height * inst.scale;
            
            // Calculate UV coordinates for sampling the sprite from the large atlas
            float u0 = flip ? meta.u1 : meta.u0, u1 = flip ? meta.u0 : meta.u1;
            
            // Pass the palette row index (each entity has its own 16-color row in the palette texture)
            glUniform1f(glGetUniformLocation(m_shader_program, "u_pal_row"), (float(inst.entity_id) + 0.5f) / 256.0f);
            
            // Draw the sprite as a square quad
            glBegin(GL_QUADS); 
            glTexCoord2f(u0, meta.v0); glVertex2f(sx, sy); 
            glTexCoord2f(u1, meta.v0); glVertex2f(sx + sw, sy);
            glTexCoord2f(u1, meta.v1); glVertex2f(sx + sw, sy + sh); 
            glTexCoord2f(u0, meta.v1); glVertex2f(sx, sy + sh); 
            glEnd();
        }
        glUseProgram(0);
    }
}


void SpriteRenderer::InitTexture() {
    auto sd = m_gd->GetSpriteData();
    std::map<std::string, FrameMetadata> global_frame_cache;
    struct FrameToRender { std::string name; std::shared_ptr<SpriteFrameEntry> entry; };
    std::vector<FrameToRender> queue;
    for (int sid = 0; sid < 256; ++sid) {
        if (!sd->IsSprite(sid)) continue;
        for (const auto& fname : sd->GetSpriteFrames(sid)) {
            if (!global_frame_cache.count(fname)) {
                auto fe = sd->GetSpriteFrame(fname);
                if (fe) { queue.push_back({fname, fe}); global_frame_cache[fname] = {}; }
            }
        }
    }
    if (queue.empty()) return;
    m_tex_w = 2048; int cur_x = 0, cur_y = 0, row_h = 0;
    for (const auto& it : queue) {
        int fw = it.entry->GetData()->GetWidth(), fh = it.entry->GetData()->GetHeight();
        if (cur_x + fw + 1 > m_tex_w) { cur_x = 0; cur_y += row_h + 1; row_h = 0; }
        row_h = std::max(row_h, fh); cur_x += fw + 1;
    }
    int m_tex_h_local = 1; while(m_tex_h_local < cur_y + row_h + 1) m_tex_h_local <<= 1;
    m_tex_h = m_tex_h_local;
    std::vector<uint8_t> data(m_tex_w * m_tex_h, 0);
    cur_x = 0; cur_y = 0; row_h = 0;
    for (auto& it : queue) {
        auto f = it.entry->GetData(); int fw = f->GetWidth(), fh = f->GetHeight();
        if (cur_x + fw + 1 > m_tex_w) { cur_x = 0; cur_y += row_h + 1; row_h = 0; }
        FrameMetadata meta = {(float)cur_x/m_tex_w, (float)cur_y/m_tex_h, (float)(cur_x+fw)/m_tex_w, (float)(cur_y+fh)/m_tex_h, fw, fh, f->GetLeft(), f->GetTop()};
        global_frame_cache[it.name] = meta;
        ImageBuffer ib(fw, fh); ib.InsertSprite(-meta.origin_x, -meta.origin_y, 0, *f);
        const auto& pix = ib.GetPixels();
        for(int y=0; y<fh; ++y) for(int x=0; x<fw; ++x) data[(cur_y+y)*m_tex_w + (cur_x+x)] = pix[y*fw + x];
        row_h = std::max(row_h, fh); cur_x += fw + 1;
    }
    for (int sid = 0; sid < 256; ++sid) {
        if (!sd->IsSprite(sid)) continue;
        auto anims = sd->GetSpriteAnimations(sid);
        for (size_t aid=0; aid<anims.size(); ++aid) {
            for (const auto& fn : sd->GetSpriteAnimationFrames(anims[aid]))
                if (global_frame_cache.count(fn)) m_sprite_meta[sid].animations[aid].push_back(global_frame_cache[fn]);
        }
    }
    glGenTextures(1, &m_texture_id); glBindTexture(GL_TEXTURE_2D, m_texture_id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, m_tex_w, m_tex_h, 0, GL_RED, GL_UNSIGNED_BYTE, data.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    std::vector<uint8_t> pal_data(256 * 16 * 4, 0);
    for(int i=0; i<256; ++i) {
        if (sd->IsEntity(i)) {
            auto p = sd->GetEntityPalette(i);
            for(int j=0; j<16; ++j) {
                uint32_t c = p->getRGBA(j);
                pal_data[(i*16 + j)*4 + 0] = (c >> 16) & 0xFF; pal_data[(i*16 + j)*4 + 1] = (c >> 8) & 0xFF;
                pal_data[(i*16 + j)*4 + 2] = (c >> 0) & 0xFF; pal_data[(i*16 + j)*4 + 3] = (c >> 24) & 0xFF;
            }
        }
    }
    glGenTextures(1, &m_pal_texture_id); glBindTexture(GL_TEXTURE_2D, m_pal_texture_id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 16, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, pal_data.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

void SpriteRenderer::InitShaders() {
    m_shader_program = CreateShader("shaders/sprite.vert", "shaders/sprite.frag");
}

GLuint SpriteRenderer::CreateShader(const std::string& vs_path, const std::string& fs_path) {
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

std::string SpriteRenderer::LoadShaderFile(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        std::cerr << "Failed to open shader file: " << path << std::endl;
        return "";
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
