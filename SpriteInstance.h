#ifndef SPRITE_INSTANCE_H
#define SPRITE_INSTANCE_H

#include <cstdint>
#include <landstalker/rooms/Entity.h>

struct FrameMetadata {
    float u0, v0, u1, v1;
    int width, height;
    int origin_x, origin_y;
};

struct SpriteAnimationSet {
    std::map<int, std::vector<FrameMetadata>> animations;
};

struct SpriteInstance {
    uint8_t entity_id;
    float x, y;
    float dx, dy;
    float scale;
    float anim_timer;
    float anim_speed;
    Landstalker::Orientation orientation;
};

#endif // SPRITE_INSTANCE_H
