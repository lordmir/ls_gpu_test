#version 120
uniform sampler2D u_atlas;
uniform sampler2D u_palettes;
uniform float u_pal_row;
void main() {
    float idx = texture2D(u_atlas, gl_TexCoord[0].xy).r * 255.0;
    if (idx < 0.5) discard;
    gl_FragColor = texture2D(u_palettes, vec2((idx + 0.5)/16.0, u_pal_row));
}
