#version 120

// u_map: Texture containing the 2D grid of block IDs (R=low byte, G=high byte)
uniform sampler2D u_map;
// u_blockset: Texture containing tile indices for each 16x16 block (2x2 tiles)
uniform sampler2D u_blockset;
// u_tileset: Large atlas containing the raw 8x8 pixel data for all tiles
uniform sampler2D u_tileset;
// u_palette: The 16-color palette for the current room
uniform sampler2D u_palette;

// Global dimensions used to normalize texture coordinates
uniform vec2 u_map_size;
uniform int u_num_tiles;
uniform int u_num_blocks;

void main() {
    // map_pos is the coordinate in "blocks" (e.g. 5.5 is halfway through the 6th block)
    vec2 map_pos = gl_TexCoord[0].xy;
    
    // 1. Identify which block we are in (discrete integer index)
    vec2 block_idx = clamp(floor(map_pos), vec2(0.0), u_map_size - 1.0);
    
    // 2. Identify where we are INSIDE that block (0.0 to 0.999)
    // We clamp to 0.999 to prevent precision errors from wrapping at the edges
    vec2 fract_pos = clamp(fract(map_pos), 0.0, 0.999);
    
    // Each block in Landstalker is 16x16 pixels
    vec2 pix_in_block = fract_pos * 16.0;
    
    // 3. Fetch the Block ID from the map texture
    // The Map texture is essentially a 2D array of 16-bit integers
    vec4 b_data = texture2D(u_map, (block_idx + 0.5) / u_map_size);
    float block_id = floor(b_data.r * 255.5) + floor(b_data.g * 255.5) * 256.0;
    
    // 4. Identify which of the 4 tiles (2x2) in the block we are hitting
    // tx, ty are 0 or 1
    float tx = clamp(floor(pix_in_block.x / 8.0), 0.0, 1.0);
    float ty = clamp(floor(pix_in_block.y / 8.0), 0.0, 1.0);
    
    // 5. Fetch the Tile Index (and flip flags) from the blockset texture
    // The Blockset texture is 2 pixels wide (tx) and (u_num_blocks * 2) pixels high (block_id * 2 + ty)
    vec4 t_data = texture2D(u_blockset, vec2((tx + 0.5)/2.0, (block_id * 2.0 + ty + 0.5)/float(u_num_blocks * 2)));
    float val = floor(t_data.r * 255.5) + floor(t_data.g * 255.5) * 256.0;
    
    // Lower 11 bits are the tile index
    float t_idx = mod(val, 2048.0);
    // Upper bits contain horizontal and vertical flip flags
    bool hflip = mod(floor(val / 2048.0), 2.0) > 0.5;
    bool vflip = mod(floor(val / 4096.0), 2.0) > 0.5;
    
    // 6. Identify the specific 8x8 pixel coordinate within the tile
    vec2 pix_in_tile = floor(mod(pix_in_block, 8.0));
    
    // Apply flipping if requested by the block data
    if (hflip) pix_in_tile.x = 7.0 - pix_in_tile.x;
    if (vflip) pix_in_tile.y = 7.0 - pix_in_tile.y;
    
    // 7. Fetch the Palette Index for this pixel from the tileset atlas
    // Tileset is 8 pixels wide and (u_num_tiles * 8) pixels high
    float c_idx = texture2D(u_tileset, vec2((pix_in_tile.x + 0.5)/8.0, (t_idx * 8.0 + pix_in_tile.y + 0.5)/float(u_num_tiles * 8))).r * 255.0;
    
    // 8. Handle Transparency
    // In Landstalker, color index 0 is always transparent
    if (c_idx < 0.5) discard;
    
    // 9. Final Color Lookup from the 1D room palette
    gl_FragColor = texture2D(u_palette, vec2((c_idx + 0.5)/16.0, 0.5));
}

