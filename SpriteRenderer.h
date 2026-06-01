#ifndef SPRITE_RENDERER_H
#define SPRITE_RENDERER_H

#include <vector>
#include <map>
#include <memory>
#include <string>
#include <landstalker/main/GameData.h>
#include "SpriteInstance.h"

#ifdef __WXMSW__
#include <windows.h>
#include <GL/gl.h>
#else
#include <GL/gl.h>
#endif

class SpriteRenderer {
public:
    SpriteRenderer(std::shared_ptr<Landstalker::GameData> gd);
    ~SpriteRenderer();

    void Init();
    void Render(const std::vector<SpriteInstance>& instances, float cam_x, float cam_y);

private:
    void InitTexture();
    void InitShaders();
    GLuint CreateShader(const std::string& vs_path, const std::string& fs_path);
    std::string LoadShaderFile(const std::string& path);

    std::shared_ptr<Landstalker::GameData> m_gd;
    GLuint m_texture_id;
    GLuint m_pal_texture_id;
    GLuint m_shader_program;
    std::map<uint8_t, SpriteAnimationSet> m_sprite_meta;
    int m_tex_w, m_tex_h;
};

#endif // SPRITE_RENDERER_H
