#include "GLCanvas.h"
#include "GLLoader.h"
#include "PixelFont.h"
#include <wx/dcclient.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <set>
#include <wx/log.h>
#include <utility>
#include <landstalker/misc/Utils.h>

using namespace Landstalker;

namespace {
int GLCanvasAttributes[] = {
    WX_GL_RGBA,
    WX_GL_DOUBLEBUFFER,
    WX_GL_STENCIL_SIZE, 8,
    0
};

float HitboxBaseToBlocks(uint8_t base) {
    return float(base) / 8.0f;
}

float HitboxHeightToBlocks(uint8_t height) {
    return float(height) / 16.0f;
}

float HitboxDrawOffset(float hitbox_base) {
    return hitbox_base < 1.5f ? 0.0f : 0.5f;
}

float EntityFrontDepthKey(const SpriteInstance& inst) {
    float center_x = inst.map_x + inst.hitbox_offset;
    float center_y = inst.map_y + inst.hitbox_offset;
    float half_base = inst.hitbox_base * 0.5f;
    return center_x + center_y + half_base * 2.0f;
}

struct EntityBounds {
    float min_x;
    float min_y;
    float max_x;
    float max_y;
    float min_z;
    float max_z;
    float back_depth;
    float front_depth;
};

EntityBounds GetEntityBounds(const SpriteInstance& inst) {
    float center_x = inst.map_x + inst.hitbox_offset;
    float center_y = inst.map_y + inst.hitbox_offset;
    float half_base = inst.hitbox_base * 0.5f;
    float min_x = center_x - half_base;
    float min_y = center_y - half_base;
    float max_x = center_x + half_base;
    float max_y = center_y + half_base;
    return {
        min_x,
        min_y,
        max_x,
        max_y,
        inst.map_z,
        inst.map_z + std::max(inst.hitbox_height, 0.125f),
        min_x + min_y,
        max_x + max_y
    };
}

float OpacityForIndex(int idx) {
    static constexpr float opacities[] = {1.0f, 0.5f, 0.0f};
    return opacities[idx % 3];
}

constexpr std::array<float, 3> kZoomSteps = {0.5f, 1.0f, 2.0f};

const char* OcclusionModeName(int idx) {
    static constexpr const char* names[] = {"TOP", "GHOST", "HIDE"};
    return names[idx % 3];
}

std::pair<GLint, GLint> EntityOcclusionDebugDepthRange(const SpriteInstance& inst)
{
    float center_x = inst.map_x + inst.hitbox_offset;
    float center_y = inst.map_y + inst.hitbox_offset;
    float half_base = inst.hitbox_base * 0.5f;
    float min_depth = (center_x - half_base) + (center_y - half_base);
    float max_depth = (center_x + half_base) + (center_y + half_base) + 25.0f;
    int rear_edge_padding = std::abs(inst.map_z - inst.floor_z) <= 0.01f ? 1 : 0;
    return {
        std::clamp(static_cast<int>(std::ceil(min_depth)) + rear_edge_padding, 0, 255),
        std::clamp(static_cast<int>(std::ceil(max_depth)), 0, 255)
    };
}

struct RoomInfoRow {
    std::string label;
    std::string value;
    uint16_t room;
};

std::string HexFlagWord(uint16_t value)
{
    std::ostringstream out;
    out << std::uppercase << std::hex << std::setw(4) << std::setfill('0') << value;
    return out.str();
}

std::string RoomDisplayText(const std::shared_ptr<GameData>& gd, uint16_t room)
{
    auto rd = gd ? gd->GetRoomData() : nullptr;
    if (!rd || room >= rd->GetRoomCount()) {
        return {};
    }

    std::ostringstream out;
    out << room << " " << wstr_to_utf8(rd->GetRoomDisplayName(room));
    return out.str();
}

std::vector<RoomInfoRow> BuildRoomInfoRows(const std::shared_ptr<GameData>& gd, uint16_t room)
{
    std::vector<RoomInfoRow> rows;
    auto rd = gd ? gd->GetRoomData() : nullptr;
    if (!rd || room >= rd->GetRoomCount()) {
        return rows;
    }

    auto add_destination_row = [&](const std::string& label, uint16_t destination_room) {
        if (destination_room >= rd->GetRoomCount()) {
            return;
        }
        rows.push_back({label, RoomDisplayText(gd, destination_room), destination_room});
    };

    if (rd->HasFallDestination(room)) {
        add_destination_row("Fall Destination", rd->GetFallDestination(room));
    }
    if (rd->HasClimbDestination(room)) {
        add_destination_row("Climb Destination", rd->GetClimbDestination(room));
    }

    for (const auto& transition : rd->GetTransitions(room)) {
        uint16_t other_room = transition.src_rm == room ? transition.dst_rm : transition.src_rm;
        if (other_room >= rd->GetRoomCount()) {
            continue;
        }

        rows.push_back({
            std::string{"Transition when Flag "} + HexFlagWord(transition.flag) + " is " + (transition.src_rm == room ? "SET" : "CLEAR"),
            RoomDisplayText(gd, other_room),
            other_room
        });
    }

    return rows;
}

float WheelSteps(const wxMouseEvent& evt) {
    int delta = evt.GetWheelDelta();
    if (delta == 0) {
        return evt.GetWheelRotation() > 0 ? 1.0f : -1.0f;
    }
    return static_cast<float>(evt.GetWheelRotation()) / static_cast<float>(delta);
}

struct PickPoint {
    float x;
    float y;
};

struct PickRect {
    float min_x;
    float min_y;
    float max_x;
    float max_y;
};

enum class TileSwapRegionPart {
    TilemapSource = 0,
    TilemapDestination = 1,
    HeightmapSource = 2,
    HeightmapDestination = 3
};

struct TileSwapRegionGeometry {
    int flat_index;
    int swap_index;
    TileSwapRegionPart part;
    TileSwap swap;
    std::vector<PickPoint> points;
    std::vector<PickPoint> fill_points;
    PickRect bounds;
    bool segments;
    PickPoint resize_handle;
};

struct TileSwapRegionMetrics {
    int x;
    int y;
    int width;
    int height;
};

struct DoorGeometry {
    int index;
    Door door;
    bool valid;
    std::vector<PickPoint> cell_points;
    std::vector<PickPoint> map_points;
    PickRect bounds;
};

PickPoint ProjectEntityGridPoint(const SpriteInstance& inst, float x, float y, float z)
{
    float grid_x = x - inst.room_left;
    float grid_y = y - inst.room_top;
    return {
        32.0f * grid_x - 32.0f * grid_y + 512.0f,
        16.0f * grid_x + 16.0f * grid_y + 100.0f - inst.z_extent * z
    };
}

PickPoint ProjectWarpGridPoint(const WarpInstance& warp, float x, float y, float z);

PickPoint ScreenToMapPoint(float world_x, float world_y, float z, float room_left, float room_top, float z_extent = 32.0f)
{
    float a = (world_x - 512.0f) / 32.0f;
    float b = (world_y - 100.0f + z_extent * z) / 16.0f;
    return {
        (a + b) * 0.5f + room_left,
        (b - a) * 0.5f + room_top
    };
}

PickRect EntityZControlRect(const SpriteInstance& inst)
{
    float center_x = inst.map_x + inst.hitbox_offset;
    float center_y = inst.map_y + inst.hitbox_offset;
    float top_z = inst.map_z + std::max(inst.hitbox_height, 0.125f);
    PickPoint top_center = ProjectEntityGridPoint(inst, center_x, center_y, top_z);
    constexpr float half_size = 6.0f;
    constexpr float y_offset = 14.0f;
    return {
        top_center.x - half_size,
        top_center.y - y_offset - half_size,
        top_center.x + half_size,
        top_center.y - y_offset + half_size
    };
}

PickRect RectAroundPoint(const PickPoint& point, float half_size)
{
    return {
        point.x - half_size,
        point.y - half_size,
        point.x + half_size,
        point.y + half_size
    };
}

PickRect WarpResizeControlRect(const WarpInstance& warp, int axis)
{
    float z = warp.floor_z;
    PickPoint point = axis == 1
        ? ProjectWarpGridPoint(warp, warp.x + warp.width, warp.y + warp.height * 0.5f, z)
        : ProjectWarpGridPoint(warp, warp.x + warp.width * 0.5f, warp.y + warp.height, z);
    return RectAroundPoint(point, 6.0f);
}

bool PointInRect(const PickPoint& point, const PickRect& rect)
{
    return point.x >= rect.min_x &&
           point.x <= rect.max_x &&
           point.y >= rect.min_y &&
           point.y <= rect.max_y;
}

PickRect TileSwapRegionResizeControlRect(const TileSwapRegionGeometry& region)
{
    return RectAroundPoint(region.resize_handle, 6.0f);
}

TileSwapRegionMetrics MetricsForTileSwapRegion(const TileSwap& swap, TileSwapRegionPart part)
{
    switch (part) {
        case TileSwapRegionPart::TilemapSource:
            return {swap.map.src_x, swap.map.src_y, swap.map.width, swap.map.height};
        case TileSwapRegionPart::TilemapDestination:
            return {swap.map.dst_x, swap.map.dst_y, swap.map.width, swap.map.height};
        case TileSwapRegionPart::HeightmapSource:
            return {swap.heightmap.src_x, swap.heightmap.src_y, swap.heightmap.width, swap.heightmap.height};
        case TileSwapRegionPart::HeightmapDestination:
            return {swap.heightmap.dst_x, swap.heightmap.dst_y, swap.heightmap.width, swap.heightmap.height};
    }
    return {0, 0, 1, 1};
}

PickPoint PolygonEdgeHandle(const std::vector<PickPoint>& points, int axis)
{
    if (points.empty()) {
        return {0.0f, 0.0f};
    }
    if (points.size() == 1) {
        return points.front();
    }

    float best_key = std::numeric_limits<float>::lowest();
    PickPoint best{0.0f, 0.0f};
    for (std::size_t i = 0; i < points.size(); ++i) {
        const PickPoint& a = points[i];
        const PickPoint& b = points[(i + 1) % points.size()];
        PickPoint midpoint{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
        float key = axis == 1 ? midpoint.x : midpoint.y;
        if (key > best_key) {
            best_key = key;
            best = midpoint;
        }
    }
    return best;
}

PickPoint TileSwapResizeHandlePoint(const std::vector<PickPoint>& points, TileSwap::Mode mode)
{
    if (points.empty()) {
        return {0.0f, 0.0f};
    }
    if (mode == TileSwap::Mode::FLOOR) {
        PickPoint best = points.front();
        for (const auto& point : points) {
            if (point.y > best.y || (point.y == best.y && point.x < best.x)) {
                best = point;
            }
        }
        return best;
    }
    PickRect bounds{
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest()
    };
    for (const auto& point : points) {
        bounds.min_x = std::min(bounds.min_x, point.x);
        bounds.min_y = std::min(bounds.min_y, point.y);
        bounds.max_x = std::max(bounds.max_x, point.x);
        bounds.max_y = std::max(bounds.max_y, point.y);
    }
    PickPoint target = mode == TileSwap::Mode::WALL_NW
        ? PickPoint{bounds.min_x, bounds.max_y}
        : PickPoint{bounds.max_x, bounds.max_y};
    PickPoint best = points.front();
    float best_dist = std::numeric_limits<float>::max();
    for (const auto& point : points) {
        float dx = point.x - target.x;
        float dy = point.y - target.y;
        float dist = dx * dx + dy * dy;
        if (dist < best_dist) {
            best_dist = dist;
            best = point;
        }
    }
    return best;
}

float ValidWarpWidth(float requested_width, float current_height)
{
    if (std::round(current_height) > 1.0f) {
        return 1.0f;
    }
    return std::clamp(std::round(requested_width), 1.0f, 3.0f);
}

float ValidWarpHeight(float requested_height, float current_width)
{
    if (std::round(current_width) > 1.0f) {
        return 1.0f;
    }
    return std::clamp(std::round(requested_height), 1.0f, 3.0f);
}

void ClampWarpToValidSize(WarpInstance& warp)
{
    float rounded_width = std::clamp(std::round(warp.width), 1.0f, 3.0f);
    float rounded_height = std::clamp(std::round(warp.height), 1.0f, 3.0f);
    if (rounded_width > 1.0f && rounded_height > 1.0f) {
        if (warp.width >= warp.height) {
            rounded_height = 1.0f;
        } else {
            rounded_width = 1.0f;
        }
    }
    warp.width = rounded_width;
    warp.height = rounded_height;
}

bool WarpResizeAxisUsable(const WarpInstance& warp, int axis)
{
    if (axis == 1) {
        return std::round(warp.height) <= 1.0f;
    }
    if (axis == 2) {
        return std::round(warp.width) <= 1.0f;
    }
    return false;
}

std::string HexWord(uint16_t value)
{
    constexpr char digits[] = "0123456789ABCDEF";
    std::string out;
    out.push_back(digits[(value >> 12) & 0x0F]);
    out.push_back(digits[(value >> 8) & 0x0F]);
    out.push_back(digits[(value >> 4) & 0x0F]);
    out.push_back(digits[value & 0x0F]);
    return out;
}

std::string HexByte(uint8_t value)
{
    constexpr char digits[] = "0123456789ABCDEF";
    std::string out;
    out.push_back(digits[(value >> 4) & 0x0F]);
    out.push_back(digits[value & 0x0F]);
    return out;
}

PickPoint ProjectWarpGridPoint(const WarpInstance& warp, float x, float y, float z)
{
    float grid_x = x - warp.room_left;
    float grid_y = y - warp.room_top;
    return {
        32.0f * grid_x - 32.0f * grid_y + 512.0f,
        16.0f * grid_x + 16.0f * grid_y + 100.0f - warp.z_extent * z
    };
}

PickPoint ProjectRoomGridPoint(float x, float y, float z, float room_left, float room_top, float z_extent = 32.0f)
{
    float grid_x = x - room_left;
    float grid_y = y - room_top;
    return {
        32.0f * grid_x - 32.0f * grid_y + 512.0f,
        16.0f * grid_x + 16.0f * grid_y + 100.0f - z_extent * z
    };
}

PickPoint ProjectHeightmapGridPoint(float x, float y, float z, float room_left, float room_top, float z_extent)
{
    float grid_x = x - room_left + 12.0f;
    float grid_y = y - room_top + 12.0f;
    return {
        32.0f * grid_x - 32.0f * grid_y + 512.0f,
        16.0f * grid_x + 16.0f * grid_y + 100.0f - z_extent * z
    };
}

PickPoint ScreenToHeightmapPoint(float world_x, float world_y, float room_left, float room_top)
{
    return ScreenToMapPoint(world_x, world_y, 0.0f, room_left - 12.0f, room_top - 12.0f);
}

PickRect BoundsForPoints(const std::vector<PickPoint>& points)
{
    PickRect bounds{
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest()
    };
    for (const auto& point : points) {
        bounds.min_x = std::min(bounds.min_x, point.x);
        bounds.min_y = std::min(bounds.min_y, point.y);
        bounds.max_x = std::max(bounds.max_x, point.x);
        bounds.max_y = std::max(bounds.max_y, point.y);
    }
    return bounds;
}

const char* TileSwapPartLabel(TileSwapRegionPart part)
{
    switch (part) {
        case TileSwapRegionPart::TilemapSource:
            return "MAP SRC";
        case TileSwapRegionPart::TilemapDestination:
            return "MAP DST";
        case TileSwapRegionPart::HeightmapSource:
            return "HM SRC";
        case TileSwapRegionPart::HeightmapDestination:
            return "HM DST";
    }
    return "";
}

const char* TileSwapShapeLabel(TileSwap::Mode mode)
{
    switch (mode) {
        case TileSwap::Mode::FLOOR:
            return "FLOOR";
        case TileSwap::Mode::WALL_NE:
            return "WNE";
        case TileSwap::Mode::WALL_NW:
            return "WNW";
    }
    return "UNK";
}

std::vector<TileSwapRegionGeometry> BuildTileSwapRegionGeometries(
    const std::shared_ptr<GameData>& gd,
    uint16_t room,
    const MapRenderer& map_renderer,
    float z_extent)
{
    std::vector<TileSwapRegionGeometry> out;
    auto rd = gd ? gd->GetRoomData() : nullptr;
    if (!rd) {
        return out;
    }
    auto swaps = rd->GetTileSwaps(room);
    if (swaps.empty()) {
        return out;
    }
    auto map_entry = rd->GetMapForRoom(room);
    auto tilemap = map_entry ? map_entry->GetData() : nullptr;
    const float room_left = static_cast<float>(map_renderer.GetRoomLeft());
    const float room_top = static_cast<float>(map_renderer.GetRoomTop());

    auto add_region = [&](int swap_index, TileSwapRegionPart part, const TileSwap& swap, std::vector<PickPoint> points, bool segments = false) {
        if (points.size() < 2) {
            return;
        }
        TileSwapRegionGeometry geom{};
        geom.flat_index = static_cast<int>(out.size());
        geom.swap_index = swap_index;
        geom.part = part;
        geom.swap = swap;
        geom.points = std::move(points);
        geom.fill_points = geom.points;
        geom.bounds = BoundsForPoints(geom.points);
        geom.segments = segments;
        geom.resize_handle = TileSwapResizeHandlePoint(geom.points, swap.mode);
        out.push_back(std::move(geom));
    };

    auto tilemap_points = [&](const TileSwap& swap, TileSwap::Region region) {
        std::vector<PickPoint> points;
        auto poly = swap.GetMapRegionPoly(TileSwap::Region::UNDEFINED, 1, 2);
        auto tile_offset = swap.GetTileOffset(region, tilemap, Tilemap3D::Layer::BG);
        PickPoint anchor = ProjectRoomGridPoint(
            room_left + static_cast<float>(tile_offset.first),
            room_top + static_cast<float>(tile_offset.second),
            0.0f,
            room_left,
            room_top);
        points.reserve(poly.size());
        for (const auto& point : poly) {
            points.push_back({
                anchor.x + static_cast<float>(point.first) * 32.0f,
                anchor.y + static_cast<float>(point.second) * 16.0f
            });
        }
        return points;
    };

    auto heightmap_region = [&](int swap_index, TileSwapRegionPart part, const TileSwap& swap, const TileSwap::CopyOp& op, bool source) {
        if (!tilemap) {
            return;
        }
        int x0 = source ? op.src_x : op.dst_x;
        int y0 = source ? op.src_y : op.dst_y;
        int x1 = x0 + op.width;
        int y1 = y0 + op.height;
        std::vector<PickPoint> segments;
        std::vector<PickPoint> x_handle_points;
        std::vector<PickPoint> y_handle_points;
        std::vector<PickPoint> left_handle_points;
        auto in_region = [&](int x, int y) {
            return x >= x0 && x < x1 && y >= y0 && y < y1;
        };
        auto height_at = [&](int x, int y) {
            if (x < 0 || y < 0 || x >= tilemap->GetHeightmapWidth() || y >= tilemap->GetHeightmapHeight()) {
                return 0.0f;
            }
            uint8_t z = tilemap->GetHeight({x, y});
            return z == 0xFF ? 0.0f : static_cast<float>(z);
        };
        auto center = [&](int x, int y) {
            return ProjectHeightmapGridPoint(
                static_cast<float>(x) + 0.5f,
                static_cast<float>(y) + 0.5f,
                height_at(x, y),
                room_left,
                room_top,
                z_extent);
        };
        auto offset = [](const PickPoint& point, float x, float y) {
            return PickPoint{point.x + x, point.y + y};
        };
        auto visual_corner = [&](int x, int y, float ox, float oy) {
            return offset(center(x, y), ox, oy);
        };
        std::vector<PickPoint> fill_points{
            visual_corner(x0, y0, 0.0f, -16.0f),
            visual_corner(x1 - 1, y0, 32.0f, 0.0f),
            visual_corner(x1 - 1, y1 - 1, 0.0f, 16.0f),
            visual_corner(x0, y1 - 1, -32.0f, 0.0f)
        };
        PickPoint visual_bottom_corner = fill_points[2];
        auto add_segment = [&](const PickPoint& a, const PickPoint& b, bool x_handle, bool y_handle, bool left_handle) {
            segments.push_back(a);
            segments.push_back(b);
            PickPoint midpoint{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
            if (x_handle) {
                x_handle_points.push_back(midpoint);
            }
            if (y_handle) {
                y_handle_points.push_back(midpoint);
            }
            if (left_handle) {
                left_handle_points.push_back(midpoint);
            }
        };
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                PickPoint c = center(x, y);
                PickPoint top = offset(c, 0.0f, -16.0f);
                PickPoint right = offset(c, 32.0f, 0.0f);
                PickPoint bottom = offset(c, 0.0f, 16.0f);
                PickPoint left = offset(c, -32.0f, 0.0f);
                if (!in_region(x, y - 1)) {
                    add_segment(top, right, false, false, false);
                }
                if (!in_region(x + 1, y)) {
                    add_segment(right, bottom, true, false, false);
                }
                if (!in_region(x, y + 1)) {
                    add_segment(bottom, left, false, true, false);
                }
                if (!in_region(x - 1, y)) {
                    add_segment(left, top, false, false, true);
                }
            }
        }
        if (segments.empty()) {
            return;
        }
        TileSwapRegionGeometry geom{};
        geom.flat_index = static_cast<int>(out.size());
        geom.swap_index = swap_index;
        geom.part = part;
        geom.swap = swap;
        geom.points = std::move(segments);
        geom.fill_points = std::move(fill_points);
        geom.bounds = BoundsForPoints(geom.points);
        PickRect fill_bounds = BoundsForPoints(geom.fill_points);
        geom.bounds.min_x = std::min(geom.bounds.min_x, fill_bounds.min_x);
        geom.bounds.min_y = std::min(geom.bounds.min_y, fill_bounds.min_y);
        geom.bounds.max_x = std::max(geom.bounds.max_x, fill_bounds.max_x);
        geom.bounds.max_y = std::max(geom.bounds.max_y, fill_bounds.max_y);
        geom.segments = true;
        auto average = [](const std::vector<PickPoint>& points, PickPoint fallback) {
            if (points.empty()) {
                return fallback;
            }
            PickPoint out{0.0f, 0.0f};
            for (const auto& point : points) {
                out.x += point.x;
                out.y += point.y;
            }
            out.x /= static_cast<float>(points.size());
            out.y /= static_cast<float>(points.size());
            return out;
        };
        (void)average;
        geom.resize_handle = visual_bottom_corner;
        out.push_back(std::move(geom));
    };

    for (std::size_t i = 0; i < swaps.size(); ++i) {
        const TileSwap& swap = swaps[i];
        add_region(static_cast<int>(i), TileSwapRegionPart::TilemapSource, swap, tilemap_points(swap, TileSwap::Region::SOURCE));
        add_region(static_cast<int>(i), TileSwapRegionPart::TilemapDestination, swap, tilemap_points(swap, TileSwap::Region::DESTINATION));
        heightmap_region(static_cast<int>(i), TileSwapRegionPart::HeightmapSource, swap, swap.heightmap, true);
        heightmap_region(static_cast<int>(i), TileSwapRegionPart::HeightmapDestination, swap, swap.heightmap, false);
    }
    return out;
}

std::vector<DoorGeometry> BuildDoorGeometries(
    const std::shared_ptr<GameData>& gd,
    uint16_t room,
    const MapRenderer& map_renderer,
    float z_extent,
    const std::shared_ptr<const Tilemap3D>& override_tilemap)
{
    std::vector<DoorGeometry> out;
    auto rd = gd ? gd->GetRoomData() : nullptr;
    if (!rd) {
        return out;
    }
    auto doors = rd->GetDoors(room);
    if (doors.empty()) {
        return out;
    }

    std::shared_ptr<const Tilemap3D> tilemap = override_tilemap;
    if (!tilemap) {
        auto map_entry = rd->GetMapForRoom(room);
        tilemap = map_entry ? map_entry->GetData() : nullptr;
    }
    if (!tilemap) {
        return out;
    }

    const float room_left = static_cast<float>(map_renderer.GetRoomLeft());
    const float room_top = static_cast<float>(map_renderer.GetRoomTop());

    auto height_at = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= tilemap->GetHeightmapWidth() || y >= tilemap->GetHeightmapHeight()) {
            return 0.0f;
        }
        uint8_t z = tilemap->GetHeight({x, y});
        return z == 0xFF ? 0.0f : static_cast<float>(z);
    };

    auto offset = [](const PickPoint& point, float x, float y) {
        return PickPoint{point.x + x, point.y + y};
    };

    for (std::size_t i = 0; i < doors.size(); ++i) {
        const Door& door = doors[i];
        int x = static_cast<int>(door.x);
        int y = static_cast<int>(door.y);
        PickPoint center = ProjectHeightmapGridPoint(
            static_cast<float>(x) + 0.5f,
            static_cast<float>(y) + 0.5f,
            height_at(x, y),
            room_left,
            room_top,
            z_extent);

        DoorGeometry geom{};
        geom.index = static_cast<int>(i);
        geom.door = door;
        geom.cell_points = {
            offset(center, 0.0f, -16.0f),
            offset(center, 32.0f, 0.0f),
            offset(center, 0.0f, 16.0f),
            offset(center, -32.0f, 0.0f)
        };

        auto [valid, poly] = door.GetMapRegionPoly(tilemap, 1, 2);
        geom.valid = valid;
        auto tile_offset = door.GetTileOffset(tilemap, Tilemap3D::Layer::BG);
        PickPoint anchor = ProjectRoomGridPoint(
            room_left + static_cast<float>(tile_offset.first),
            room_top + static_cast<float>(tile_offset.second),
            0.0f,
            room_left,
            room_top);
        geom.map_points.reserve(poly.size());
        for (const auto& point : poly) {
            geom.map_points.push_back({
                anchor.x + static_cast<float>(point.first) * 32.0f,
                anchor.y + static_cast<float>(point.second) * 16.0f
            });
        }

        geom.bounds = BoundsForPoints(geom.cell_points);
        if (!geom.map_points.empty()) {
            PickRect map_bounds = BoundsForPoints(geom.map_points);
            geom.bounds.min_x = std::min(geom.bounds.min_x, map_bounds.min_x);
            geom.bounds.min_y = std::min(geom.bounds.min_y, map_bounds.min_y);
            geom.bounds.max_x = std::max(geom.bounds.max_x, map_bounds.max_x);
            geom.bounds.max_y = std::max(geom.bounds.max_y, map_bounds.max_y);
        }
        out.push_back(std::move(geom));
    }

    return out;
}

float DistanceToSegment(const PickPoint& point, const PickPoint& a, const PickPoint& b)
{
    float dx = b.x - a.x;
    float dy = b.y - a.y;
    float len_sq = dx * dx + dy * dy;
    if (len_sq <= 0.0001f) {
        float px = point.x - a.x;
        float py = point.y - a.y;
        return std::sqrt(px * px + py * py);
    }
    float t = std::clamp(((point.x - a.x) * dx + (point.y - a.y) * dy) / len_sq, 0.0f, 1.0f);
    float closest_x = a.x + t * dx;
    float closest_y = a.y + t * dy;
    float px = point.x - closest_x;
    float py = point.y - closest_y;
    return std::sqrt(px * px + py * py);
}

bool PointNearPolyline(const PickPoint& point, const std::vector<PickPoint>& points, float threshold)
{
    if (points.size() < 2) {
        return false;
    }
    for (std::size_t i = 0; i < points.size(); ++i) {
        if (DistanceToSegment(point, points[i], points[(i + 1) % points.size()]) <= threshold) {
            return true;
        }
    }
    return false;
}

bool PointNearSegments(const PickPoint& point, const std::vector<PickPoint>& points, float threshold)
{
    if (points.size() < 2) {
        return false;
    }
    for (std::size_t i = 0; i + 1 < points.size(); i += 2) {
        if (DistanceToSegment(point, points[i], points[i + 1]) <= threshold) {
            return true;
        }
    }
    return false;
}

bool PointInPolygon(const PickPoint& point, const std::vector<PickPoint>& polygon)
{
    if (polygon.size() < 3) {
        return false;
    }
    bool inside = false;
    for (std::size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
        const PickPoint& a = polygon[i];
        const PickPoint& b = polygon[j];
        bool crosses = ((a.y > point.y) != (b.y > point.y)) &&
            (point.x < (b.x - a.x) * (point.y - a.y) / ((b.y - a.y) == 0.0f ? 0.0001f : (b.y - a.y)) + a.x);
        if (crosses) {
            inside = !inside;
        }
    }
    return inside;
}

bool PointInPolygonWinding(const PickPoint& point, const std::vector<PickPoint>& polygon)
{
    if (polygon.size() < 3) {
        return false;
    }
    int winding = 0;
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        const PickPoint& a = polygon[i];
        const PickPoint& b = polygon[(i + 1) % polygon.size()];
        float cross = (b.x - a.x) * (point.y - a.y) - (b.y - a.y) * (point.x - a.x);
        if (a.y <= point.y) {
            if (b.y > point.y && cross > 0.0f) {
                ++winding;
            }
        } else if (b.y <= point.y && cross < 0.0f) {
            --winding;
        }
    }
    return winding != 0;
}

void DrawDashedClosedPolyline(const std::vector<PickPoint>& points, float dash_len = 8.0f, float gap_len = 5.0f)
{
    if (points.size() < 2) {
        return;
    }
    glBegin(GL_LINES);
    for (std::size_t i = 0; i < points.size(); ++i) {
        PickPoint a = points[i];
        PickPoint b = points[(i + 1) % points.size()];
        float dx = b.x - a.x;
        float dy = b.y - a.y;
        float len = std::sqrt(dx * dx + dy * dy);
        if (len <= 0.0001f) {
            continue;
        }
        float ux = dx / len;
        float uy = dy / len;
        for (float pos = 0.0f; pos < len; pos += dash_len + gap_len) {
            float end = std::min(pos + dash_len, len);
            glVertex2f(a.x + ux * pos, a.y + uy * pos);
            glVertex2f(a.x + ux * end, a.y + uy * end);
        }
    }
    glEnd();
}

void DrawClosedPolyline(const std::vector<PickPoint>& points)
{
    if (points.size() < 2) {
        return;
    }
    glBegin(GL_LINE_LOOP);
    for (const auto& point : points) {
        glVertex2f(point.x, point.y);
    }
    glEnd();
}

void DrawSegments(const std::vector<PickPoint>& points)
{
    if (points.size() < 2) {
        return;
    }
    glBegin(GL_LINES);
    for (std::size_t i = 0; i + 1 < points.size(); i += 2) {
        glVertex2f(points[i].x, points[i].y);
        glVertex2f(points[i + 1].x, points[i + 1].y);
    }
    glEnd();
}

void DrawDashedSegments(const std::vector<PickPoint>& points, float dash_len = 8.0f, float gap_len = 5.0f)
{
    if (points.size() < 2) {
        return;
    }
    glBegin(GL_LINES);
    for (std::size_t i = 0; i + 1 < points.size(); i += 2) {
        PickPoint a = points[i];
        PickPoint b = points[i + 1];
        float dx = b.x - a.x;
        float dy = b.y - a.y;
        float len = std::sqrt(dx * dx + dy * dy);
        if (len <= 0.0001f) {
            continue;
        }
        float ux = dx / len;
        float uy = dy / len;
        for (float pos = 0.0f; pos < len; pos += dash_len + gap_len) {
            float end = std::min(pos + dash_len, len);
            glVertex2f(a.x + ux * pos, a.y + uy * pos);
            glVertex2f(a.x + ux * end, a.y + uy * end);
        }
    }
    glEnd();
}

float Cross(const PickPoint& a, const PickPoint& b, const PickPoint& c)
{
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

bool PointInQuad(const PickPoint& point, const std::array<PickPoint, 4>& quad)
{
    bool has_positive = false;
    bool has_negative = false;
    for (std::size_t i = 0; i < quad.size(); ++i) {
        float cross = Cross(quad[i], quad[(i + 1) % quad.size()], point);
        has_positive = has_positive || cross > 0.0f;
        has_negative = has_negative || cross < 0.0f;
        if (has_positive && has_negative) {
            return false;
        }
    }
    return true;
}

bool PointInEntityHitbox(const SpriteInstance& inst, const PickPoint& point, bool include_shadow = true)
{
    if (inst.hitbox_base <= 0.0f) {
        return false;
    }

    float center_x = inst.map_x + inst.hitbox_offset;
    float center_y = inst.map_y + inst.hitbox_offset;
    float half_base = inst.hitbox_base * 0.5f;
    float min_x = center_x - half_base;
    float min_y = center_y - half_base;
    float max_x = center_x + half_base;
    float max_y = center_y + half_base;
    float bottom_z = inst.map_z;
    float height = std::max(inst.hitbox_height, 0.125f);
    std::array<PickPoint, 4> bottom = {
        ProjectEntityGridPoint(inst, min_x, min_y, bottom_z),
        ProjectEntityGridPoint(inst, max_x, min_y, bottom_z),
        ProjectEntityGridPoint(inst, max_x, max_y, bottom_z),
        ProjectEntityGridPoint(inst, min_x, max_y, bottom_z)
    };
    std::array<PickPoint, 4> top = {
        ProjectEntityGridPoint(inst, min_x, min_y, bottom_z + height),
        ProjectEntityGridPoint(inst, max_x, min_y, bottom_z + height),
        ProjectEntityGridPoint(inst, max_x, max_y, bottom_z + height),
        ProjectEntityGridPoint(inst, min_x, max_y, bottom_z + height)
    };
    std::array<PickPoint, 4> shadow = {
        ProjectEntityGridPoint(inst, min_x, min_y, inst.floor_z),
        ProjectEntityGridPoint(inst, max_x, min_y, inst.floor_z),
        ProjectEntityGridPoint(inst, max_x, max_y, inst.floor_z),
        ProjectEntityGridPoint(inst, min_x, max_y, inst.floor_z)
    };

    if (PointInQuad(point, bottom) || PointInQuad(point, top) || (include_shadow && PointInQuad(point, shadow))) {
        return true;
    }

    for (std::size_t i = 0; i < bottom.size(); ++i) {
        std::array<PickPoint, 4> side = {
            bottom[i],
            bottom[(i + 1) % bottom.size()],
            top[(i + 1) % top.size()],
            top[i]
        };
        if (PointInQuad(point, side)) {
            return true;
        }
    }
    return false;
}

std::array<PickPoint, 4> WarpQuad(const WarpInstance& warp, float z_offset = 0.0f)
{
    float x0 = warp.x;
    float y0 = warp.y;
    float x1 = warp.x + warp.width;
    float y1 = warp.y + warp.height;
    float z = warp.floor_z + z_offset;
    return {
        ProjectWarpGridPoint(warp, x0, y0, z),
        ProjectWarpGridPoint(warp, x1, y0, z),
        ProjectWarpGridPoint(warp, x1, y1, z),
        ProjectWarpGridPoint(warp, x0, y1, z)
    };
}

bool PointInWarp(const WarpInstance& warp, const PickPoint& point)
{
    return PointInQuad(point, WarpQuad(warp));
}

WarpInstance MakeWarpInstance(
    const Landstalker::WarpList::Warp& warp,
    uint16_t current_room,
    uint32_t instance_id,
    float room_left,
    float room_top,
    float z_extent = 32.0f,
    uint32_t warp_key = 0,
    int side_override = 0)
{
    bool current_room_is_room1 = side_override == 0 ? warp.room1 == current_room : side_override == 1;
    WarpInstance inst{};
    inst.instance_id = instance_id;
    inst.warp_key = warp_key != 0 ? warp_key : instance_id;
    inst.warp = warp;
    inst.current_room_is_room1 = current_room_is_room1;
    inst.x = float(current_room_is_room1 ? warp.x1 : warp.x2);
    inst.y = float(current_room_is_room1 ? warp.y1 : warp.y2);
    inst.width = float(std::max<uint8_t>(warp.x_size, 1));
    inst.height = float(std::max<uint8_t>(warp.y_size, 1));
    inst.z_extent = z_extent;
    inst.room_left = room_left;
    inst.room_top = room_top;
    return inst;
}

bool EntityDrawOrder(const SpriteInstance& lhs, const SpriteInstance& rhs) {
    const float lhs_depth = EntityFrontDepthKey(lhs);
    const float rhs_depth = EntityFrontDepthKey(rhs);
    if (lhs_depth != rhs_depth) {
        return lhs_depth < rhs_depth;
    }

    if (std::abs(lhs.map_z - rhs.map_z) > 0.001f) {
        return lhs.map_z > rhs.map_z;
    }

    if (lhs.map_y != rhs.map_y) {
        return lhs.map_y < rhs.map_y;
    }

    return lhs.map_x < rhs.map_x;
}

bool EntityMustDrawBefore(const SpriteInstance& lhs, const SpriteInstance& rhs) {
    constexpr float epsilon = 0.001f;
    EntityBounds lhs_bounds = GetEntityBounds(lhs);
    EntityBounds rhs_bounds = GetEntityBounds(rhs);

    bool lhs_behind = lhs_bounds.max_x <= rhs_bounds.min_x + epsilon ||
                      lhs_bounds.max_y <= rhs_bounds.min_y + epsilon;
    bool rhs_behind = rhs_bounds.max_x <= lhs_bounds.min_x + epsilon ||
                      rhs_bounds.max_y <= lhs_bounds.min_y + epsilon;
    if (lhs_behind != rhs_behind) {
        return lhs_behind;
    }

    bool footprints_overlap = lhs_bounds.min_x < rhs_bounds.max_x - epsilon &&
                              lhs_bounds.max_x > rhs_bounds.min_x + epsilon &&
                              lhs_bounds.min_y < rhs_bounds.max_y - epsilon &&
                              lhs_bounds.max_y > rhs_bounds.min_y + epsilon;
    bool z_ranges_overlap = lhs_bounds.min_z < rhs_bounds.max_z - epsilon &&
                            lhs_bounds.max_z > rhs_bounds.min_z + epsilon;
    if (footprints_overlap && z_ranges_overlap &&
        std::abs(lhs_bounds.front_depth - rhs_bounds.front_depth) > epsilon) {
        return lhs_bounds.front_depth < rhs_bounds.front_depth;
    }
    if (footprints_overlap && !z_ranges_overlap && std::abs(lhs.map_z - rhs.map_z) > epsilon) {
        return lhs.map_z > rhs.map_z;
    }

    if (std::abs(lhs_bounds.front_depth - rhs_bounds.front_depth) > epsilon) {
        return lhs_bounds.front_depth < rhs_bounds.front_depth;
    }

    if (std::abs(lhs.map_z - rhs.map_z) > epsilon) {
        return lhs.map_z > rhs.map_z;
    }

    return EntityDrawOrder(lhs, rhs);
}

void SortEntitiesGeometrically(std::vector<SpriteInstance>& instances) {
    std::stable_sort(instances.begin(), instances.end(), EntityDrawOrder);

    const std::size_t count = instances.size();
    std::vector<std::vector<std::size_t>> after(count);
    std::vector<std::size_t> incoming(count, 0);
    for (std::size_t i = 0; i < count; ++i) {
        for (std::size_t j = i + 1; j < count; ++j) {
            bool i_before_j = EntityMustDrawBefore(instances[i], instances[j]);
            bool j_before_i = EntityMustDrawBefore(instances[j], instances[i]);
            if (i_before_j == j_before_i) {
                continue;
            }
            std::size_t before = i_before_j ? i : j;
            std::size_t later = i_before_j ? j : i;
            after[before].push_back(later);
            ++incoming[later];
        }
    }

    std::deque<std::size_t> ready;
    for (std::size_t i = 0; i < count; ++i) {
        if (incoming[i] == 0) {
            ready.push_back(i);
        }
    }

    std::vector<SpriteInstance> sorted;
    sorted.reserve(count);
    std::vector<bool> emitted(count, false);
    while (!ready.empty()) {
        std::size_t index = ready.front();
        ready.pop_front();
        if (emitted[index]) {
            continue;
        }
        emitted[index] = true;
        sorted.push_back(instances[index]);
        for (std::size_t later : after[index]) {
            if (--incoming[later] == 0) {
                ready.push_back(later);
            }
        }
    }

    for (std::size_t i = 0; i < count; ++i) {
        if (!emitted[i]) {
            sorted.push_back(instances[i]);
        }
    }
    instances = std::move(sorted);
}

std::set<uint32_t> FindCollidedEntities(const std::vector<SpriteInstance>& instances) {
    constexpr float epsilon = 0.001f;
    std::set<uint32_t> collided;
    for (std::size_t i = 0; i < instances.size(); ++i) {
        EntityBounds lhs = GetEntityBounds(instances[i]);
        for (std::size_t j = i + 1; j < instances.size(); ++j) {
            EntityBounds rhs = GetEntityBounds(instances[j]);
            bool overlap =
                lhs.min_x < rhs.max_x - epsilon &&
                lhs.max_x > rhs.min_x + epsilon &&
                lhs.min_y < rhs.max_y - epsilon &&
                lhs.max_y > rhs.min_y + epsilon &&
                lhs.min_z < rhs.max_z - epsilon &&
                lhs.max_z > rhs.min_z + epsilon;
            if (overlap) {
                collided.insert(instances[i].instance_id);
                collided.insert(instances[j].instance_id);
            }
        }
    }
    return collided;
}

void DrawStencilOverlay(float cam_x, float cam_y, int width, int height, GLint ref, GLint mask, float r, float g, float b, float a)
{
    glUseProgram(0);
    for (int i = 0; i <= 5; ++i) {
        glActiveTexture(GL_TEXTURE0 + i);
        glDisable(GL_TEXTURE_2D);
    }
    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0x00);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glStencilFunc(GL_EQUAL, ref, mask);
    glColor4f(r, g, b, a);
    glBegin(GL_QUADS);
    glVertex2f(-cam_x, -cam_y);
    glVertex2f(float(width) - cam_x, -cam_y);
    glVertex2f(float(width) - cam_x, float(height) - cam_y);
    glVertex2f(-cam_x, float(height) - cam_y);
    glEnd();
    glStencilMask(0xFF);
    glDisable(GL_STENCIL_TEST);
}

void DrawOverlayGlyph(char c, float x, float y, float scale)
{
    const auto* glyph = PixelFont::Glyph(c);
    if (!glyph) {
        return;
    }

    glBegin(GL_QUADS);
    for (int row = 0; row < PixelFont::kGlyphHeight; ++row) {
        uint8_t bits = (*glyph)[row];
        for (int col = 0; col < PixelFont::kGlyphWidth; ++col) {
            uint8_t mask = static_cast<uint8_t>(1u << (PixelFont::kGlyphWidth - 1 - col));
            if ((bits & mask) == 0) {
                continue;
            }

            float px = x + float(col) * scale;
            float py = y + float(row) * scale;
            glVertex2f(px, py);
            glVertex2f(px + scale, py);
            glVertex2f(px + scale, py + scale);
            glVertex2f(px, py + scale);
        }
    }
    glEnd();
}

void DrawOverlayText(const std::string& text, float x, float y, float scale)
{
    constexpr float glyph_advance = 6.0f;
    for (std::size_t i = 0; i < text.size(); ++i) {
        DrawOverlayGlyph(text[i], x + float(i) * glyph_advance * scale, y, scale);
    }
}
}

wxBEGIN_EVENT_TABLE(MyGLCanvas, wxGLCanvas)
    EVT_PAINT(MyGLCanvas::OnPaint)
    EVT_TIMER(wxID_ANY, MyGLCanvas::OnTimer)
    EVT_KEY_DOWN(MyGLCanvas::OnKeyDown)
    EVT_MOTION(MyGLCanvas::OnMouseMove)
    EVT_LEFT_DOWN(MyGLCanvas::OnLeftDown)
    EVT_LEFT_UP(MyGLCanvas::OnLeftUp)
    EVT_MIDDLE_DOWN(MyGLCanvas::OnMiddleDown)
    EVT_MIDDLE_UP(MyGLCanvas::OnMiddleUp)
    EVT_RIGHT_DOWN(MyGLCanvas::OnRightDown)
    EVT_RIGHT_UP(MyGLCanvas::OnRightUp)
    EVT_LEAVE_WINDOW(MyGLCanvas::OnMouseLeave)
    EVT_MOUSEWHEEL(MyGLCanvas::OnMouseWheel)
    EVT_SIZE(MyGLCanvas::OnSize)
wxEND_EVENT_TABLE()

MyGLCanvas::MyGLCanvas(wxWindow* parent, std::shared_ptr<GameData> gd)
    : wxGLCanvas(parent, wxID_ANY, GLCanvasAttributes, wxDefaultPosition, wxDefaultSize, wxFULL_REPAINT_ON_RESIZE),
      m_gd(gd), m_mapRenderer(gd), m_heightmapRenderer(gd), m_spriteRenderer(gd),
      m_frame_count(0), m_fps(0.0f), m_current_room(0), m_cam_x(0.0f), m_cam_y(0.0f), m_show_heightmap(false),
      m_hovered_entity_idx(-1), m_selected_entity_idx(-1), m_hovered_warp_idx(-1), m_selected_warp_idx(-1),
      m_hovered_tileswap_region_idx(-1), m_selected_tileswap_region_idx(-1),
      m_hovered_door_idx(-1), m_selected_door_idx(-1),
      m_dragging_entity(false), m_drag_z_axis_only(false), m_drag_instance_id(0),
      m_drag_start_x(0.0f), m_drag_start_y(0.0f), m_drag_start_z(0.0f),
      m_drag_plane_z(0.0f), m_drag_cursor_offset_x(0.0f), m_drag_cursor_offset_y(0.0f), m_drag_floor_snap(false),
      m_dragging_warp(false), m_drag_warp_instance_id(0), m_drag_warp_resize_axis(0),
      m_drag_warp_start_x(0.0f), m_drag_warp_start_y(0.0f),
      m_drag_warp_start_width(0.0f), m_drag_warp_start_height(0.0f), m_drag_warp_start_floor_z(0.0f),
      m_dragging_door(false), m_drag_door_idx(-1), m_drag_door_start_x(0), m_drag_door_start_y(0),
      m_dragging_tileswap_region(false), m_drag_tileswap_region_idx(-1), m_drag_tileswap_resize_axis(0),
      m_drag_tileswap_start_x(0), m_drag_tileswap_start_y(0), m_drag_tileswap_start_width(1), m_drag_tileswap_start_height(1),
    m_dragging_pan(false), m_drag_pan_start_mouse(wxDefaultPosition), m_drag_pan_start_cam_x(0.0f), m_drag_pan_start_cam_y(0.0f),
      m_bg_opacity_idx(0), m_fg_opacity_idx(0), m_sprite_opacity_idx(0), m_entity_occlusion_idx(1),
      m_debug_occlusion(false), m_show_hitboxes(true), m_tileswap_preview_active(false), m_tileswap_preview_swap_index(-1),
      m_door_preview_active(false), m_door_preview_idx(-1),
      m_timer(this), m_pending_warp_half(false), m_pending_warp_room(0xFFFF), m_pending_warp_instance_id(0),
    m_entity_clipboard_valid(false), m_zoom_step_idx(2), m_initialized(false), m_last_mouse_pos(wxDefaultPosition)
{
    m_context = new wxGLContext(this);
    SetCurrent(*m_context);
    InitGLLoader();

    m_current_room = 630;
    m_fps_stopwatch.Start();
    // Drive the paint loop at a higher cadence (~120 Hz target).
    m_timer.Start(8);
}

MyGLCanvas::~MyGLCanvas() {
    PersistCurrentRoomEdits();
    delete m_context;
}

void MyGLCanvas::LoadRoom(uint16_t roomnum) {
    if (m_initialized) {
        PersistCurrentRoomEdits();
    }
    m_tileswap_preview_active = false;
    m_tileswap_preview_swap_index = -1;
    m_door_preview_active = false;
    m_door_preview_idx = -1;
    m_tileswap_preview_map.reset();
    m_heightmapRenderer.ClearPreviewMap();
    m_current_room = roomnum;
    m_mapRenderer.LoadRoom(roomnum);
    m_heightmapRenderer.LoadRoom(roomnum);
    m_spriteRenderer.LoadRoom(roomnum);
    m_instances.clear();
    m_warps.clear();
    m_hovered_entity_idx = -1;
    m_selected_entity_idx = -1;
    m_hovered_warp_idx = -1;
    m_selected_warp_idx = -1;
    m_hovered_tileswap_region_idx = -1;
    m_selected_tileswap_region_idx = -1;
    m_hovered_door_idx = -1;
    m_selected_door_idx = -1;
    m_dragging_entity = false;
    m_dragging_warp = false;
    m_dragging_door = false;
    m_dragging_tileswap_region = false;
    SetCursor(wxCursor(wxCURSOR_ARROW));
    auto sd = m_gd->GetSpriteData();
    auto entities = sd->GetRoomEntities(roomnum);
    m_room_entities = entities;
    float mat[9] = { 32.0f, 16.0f, 0.0f, -32.0f, 16.0f, 0.0f, 512.0f, 100.0f, 1.0f };
    uint32_t instance_id = 1;
    for (const auto& e : entities) {
        float entity_x = float(e.GetXDbl());
        float entity_y = float(e.GetYDbl());
        float entity_z = float(e.GetZDbl());
        float hitbox_base = 1.0f;
        float hitbox_height = 1.0f;
        if (sd->IsEntity(e.GetType())) {
            auto hitbox = sd->GetEntityHitbox(e.GetType());
            hitbox_base = HitboxBaseToBlocks(hitbox.base);
            hitbox_height = HitboxHeightToBlocks(hitbox.height);
        }
        float hitbox_offset = HitboxDrawOffset(hitbox_base);
        float floor_z = FloorUnderHitbox(
            entity_x + hitbox_offset,
            entity_y + hitbox_offset,
            hitbox_base * 0.5f);
        float ex_block = entity_x + hitbox_offset - m_mapRenderer.GetRoomLeft();
        float ey_block = entity_y + hitbox_offset - m_mapRenderer.GetRoomTop();
        float ez_block = entity_z;
        float px = mat[0] * ex_block + mat[3] * ey_block + mat[6];
        float py = mat[1] * ex_block + mat[4] * ey_block + mat[7] - ez_block * 32.0f;

        SpriteInstance inst{};
        inst.instance_id = instance_id++;
        inst.entity_id = e.GetType();
        inst.palette = e.GetPalette();
        inst.x = px;
        inst.y = py;
        inst.map_x = entity_x;
        inst.map_y = entity_y;
        inst.map_z = entity_z;
        inst.floor_z = floor_z;
        inst.z_extent = m_heightmapRenderer.GetZExtent();
        inst.hitbox_base = hitbox_base;
        inst.hitbox_height = hitbox_height;
        inst.hitbox_offset = hitbox_offset;
        inst.room_left = float(m_mapRenderer.GetRoomLeft());
        inst.room_top = float(m_mapRenderer.GetRoomTop());
        inst.dx = 0.0f;
        inst.dy = 0.0f;
        inst.scale = 2.0f;
        inst.anim_timer = 0.0f;
        inst.anim_speed = 1.0f;
        inst.orientation = e.GetOrientation();
        m_instances.push_back(inst);
    }

    uint32_t warp_instance_id = 1;
    uint32_t warp_key = 1;
    for (const auto& warp : m_gd->GetRoomData()->GetWarpsForRoom(roomnum)) {
        WarpInstance inst = MakeWarpInstance(
            warp,
            roomnum,
            warp_instance_id++,
            float(m_mapRenderer.GetRoomLeft()),
            float(m_mapRenderer.GetRoomTop()),
            m_heightmapRenderer.GetZExtent(),
            warp_key);
        UpdateWarpFloor(inst);
        m_warps.push_back(inst);
        if (warp.room1 == roomnum && warp.room2 == roomnum && warp.IsValid()) {
            WarpInstance dest_inst = MakeWarpInstance(
                warp,
                roomnum,
                warp_instance_id++,
                float(m_mapRenderer.GetRoomLeft()),
                float(m_mapRenderer.GetRoomTop()),
                m_heightmapRenderer.GetZExtent(),
                warp_key,
                2);
            UpdateWarpFloor(dest_inst);
            m_warps.push_back(dest_inst);
        }
        ++warp_key;
    }
    if (m_pending_warp_half && m_pending_warp_room == roomnum) {
        m_pending_warp_instance_id = warp_instance_id++;
        WarpInstance inst = MakeWarpInstance(
            m_pending_warp,
            roomnum,
            m_pending_warp_instance_id,
            float(m_mapRenderer.GetRoomLeft()),
            float(m_mapRenderer.GetRoomTop()),
            m_heightmapRenderer.GetZExtent(),
            warp_key++);
        UpdateWarpFloor(inst);
        m_warps.push_back(inst);
    }

    SortEntitiesGeometrically(m_instances);
    CenterCameraOnRoom();
    m_room_stopwatch.Start();
}

void MyGLCanvas::OnTimer(wxTimerEvent&) {
    // Recompute floors/projections periodically instead of every paint.
    // This avoids heavy per-frame work, which is especially noticeable at high zoom.
    if ((m_frame_count & 0x07) == 0) {
        RefreshObjectPlacementsFromHeightmap();
    }

    auto sd = m_gd->GetSpriteData();
    for (auto& inst : m_instances) {
        if (!sd->IsEntity(inst.entity_id)) {
            continue;
        }
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
            f->SetStatusText(wxString::Format("FPS: %.2f | Entities: %zu | Room: %d (%ls) | Cam: %.0f, %.0f | HM: %s %.0f | BG: %.1f FG: %.1f SPR: %.1f OCC: %s DBG: %s BOX: %s", 
                m_fps, m_instances.size(), m_current_room, name.c_str(), m_cam_x, m_cam_y,
                m_show_heightmap ? "ON" : "OFF", m_heightmapRenderer.GetZExtent(),
                OpacityForIndex(m_bg_opacity_idx), OpacityForIndex(m_fg_opacity_idx), OpacityForIndex(m_sprite_opacity_idx),
                OcclusionModeName(m_entity_occlusion_idx), m_debug_occlusion ? "ON" : "OFF", m_show_hitboxes ? "ON" : "OFF"));
        }
    }
    Refresh();
}

void MyGLCanvas::OnKeyDown(wxKeyEvent& evt) {
    float speed = 20.0f;
    uint16_t room_count = m_gd->GetRoomData()->GetRoomCount();
    bool ctrl = evt.ControlDown();
    bool shift = evt.ShiftDown();
    bool alt = evt.AltDown();
    auto resize_selected_tileswap = [this](int dw, int dh) {
        auto regions = BuildTileSwapRegionGeometries(m_gd, m_current_room, m_mapRenderer, m_heightmapRenderer.GetZExtent());
        if (m_selected_tileswap_region_idx < 0 ||
            m_selected_tileswap_region_idx >= static_cast<int>(regions.size())) {
            return;
        }
        TileSwapRegionMetrics metrics = MetricsForTileSwapRegion(
            regions[static_cast<std::size_t>(m_selected_tileswap_region_idx)].swap,
            regions[static_cast<std::size_t>(m_selected_tileswap_region_idx)].part);
        ResizeSelectedTileSwapRegion(
            dw == 0 ? 0.0f : static_cast<float>(metrics.width + dw),
            dh == 0 ? 0.0f : static_cast<float>(metrics.height + dh));
    };
    auto change_zoom_step = [this](int delta, float anchor_x, float anchor_y) {
        int old_idx = std::clamp(m_zoom_step_idx, 0, static_cast<int>(kZoomSteps.size()) - 1);
        int new_idx = std::clamp(old_idx + delta, 0, static_cast<int>(kZoomSteps.size()) - 1);
        if (new_idx == old_idx) {
            return;
        }
        float old_zoom = kZoomSteps[static_cast<std::size_t>(old_idx)];
        float new_zoom = kZoomSteps[static_cast<std::size_t>(new_idx)];
        float world_x = (anchor_x - m_cam_x) / old_zoom;
        float world_y = (anchor_y - m_cam_y) / old_zoom;
        m_zoom_step_idx = new_idx;
        m_cam_x = anchor_x - world_x * new_zoom;
        m_cam_y = anchor_y - world_y * new_zoom;
    };

    int unicode_key = evt.GetUnicodeKey();
    if (unicode_key == '[' || unicode_key == '{') {
        if (m_selected_tileswap_region_idx >= 0) {
            CycleSelectedTileSwapId(-1);
        } else {
            ReorderSelectedObject(-1);
        }
        Refresh();
        return;
    }
    if (unicode_key == ']' || unicode_key == '}') {
        if (m_selected_tileswap_region_idx >= 0) {
            CycleSelectedTileSwapId(1);
        } else {
            ReorderSelectedObject(1);
        }
        Refresh();
        return;
    }

    switch(evt.GetKeyCode()) {
        case WXK_F4:
            if (alt) {
                auto* f = wxDynamicCast(GetParent(), wxFrame);
                if (f) {
                    f->Close();
                }
            }
            break;
        case WXK_ESCAPE: {
            m_selected_entity_idx = -1;
            m_selected_warp_idx = -1;
            m_selected_tileswap_region_idx = -1;
            m_selected_door_idx = -1;
            m_hovered_entity_idx = -1;
            m_hovered_warp_idx = -1;
            m_hovered_tileswap_region_idx = -1;
            m_hovered_door_idx = -1;
            break;
        }
        case WXK_LEFT: m_cam_x += speed; break;
        case WXK_RIGHT: m_cam_x -= speed; break;
        case WXK_UP: m_cam_y += speed; break;
        case WXK_DOWN: m_cam_y -= speed; break;
        case WXK_PAGEUP: LoadRoom((m_current_room + 1) % room_count); break;
        case WXK_PAGEDOWN: LoadRoom(m_current_room > 0 ? m_current_room - 1 : room_count - 1); break;
        case WXK_TAB:
            SelectNextObject(ctrl ? -1 : 1);
            break;
        case WXK_SPACE:
            if (m_selected_door_idx >= 0) {
                ToggleSelectedDoorPreview();
            } else {
                ToggleSelectedTileSwapPreview();
            }
            break;
        case 'c':
        case 'C':
            if (ctrl) {
                CopySelectedEntity();
            }
            break;
        case 'v':
        case 'V':
            if (ctrl) {
                PasteEntity();
            }
            break;
        case WXK_INSERT:
            if (shift) {
                AddWarpHalf();
            } else if (ctrl) {
                AddTileSwap();
            } else if (alt) {
                AddDoor();
            } else {
                AddEntity();
            }
            break;
        case WXK_DELETE:
            DeleteSelectedObject();
            break;
        case '[':
        case '{':
            if (m_selected_tileswap_region_idx >= 0) {
                CycleSelectedTileSwapId(-1);
            } else {
                ReorderSelectedObject(-1);
            }
            break;
        case ']':
        case '}':
            if (m_selected_tileswap_region_idx >= 0) {
                CycleSelectedTileSwapId(1);
            } else {
                ReorderSelectedObject(1);
            }
            break;
        case ',':
        case '<':
            if (m_selected_door_idx >= 0) {
                CycleSelectedDoorSize(-1);
            } else if (m_selected_tileswap_region_idx >= 0) {
                CycleSelectedTileSwapShape(-1);
            } else if (m_selected_warp_idx >= 0) {
                CycleSelectedWarpType(-1);
            } else {
                CycleSelectedEntityId(-1);
            }
            break;
        case '.':
        case '>':
            if (m_selected_door_idx >= 0) {
                CycleSelectedDoorSize(1);
            } else if (m_selected_tileswap_region_idx >= 0) {
                CycleSelectedTileSwapShape(1);
            } else if (m_selected_warp_idx >= 0) {
                CycleSelectedWarpType(1);
            } else {
                CycleSelectedEntityId(1);
            }
            break;
        case '+':
        case '=':
        case WXK_NUMPAD_ADD:
            if (ctrl) {
                int w = 0;
                int h = 0;
                GetClientSize(&w, &h);
                change_zoom_step(1, float(w) * 0.5f, float(h) * 0.5f);
            } else {
                m_heightmapRenderer.AdjustZExtent(4.0f);
                RefreshObjectPlacementsFromHeightmap();
            }
            break;
        case '-':
        case WXK_NUMPAD_SUBTRACT:
            if (ctrl) {
                int w = 0;
                int h = 0;
                GetClientSize(&w, &h);
                change_zoom_step(-1, float(w) * 0.5f, float(h) * 0.5f);
            } else {
                m_heightmapRenderer.AdjustZExtent(-4.0f);
                RefreshObjectPlacementsFromHeightmap();
            }
            break;
        case 'b':
        case 'B':
            m_bg_opacity_idx = (m_bg_opacity_idx + 1) % 3;
            m_mapRenderer.SetBackgroundOpacity(OpacityForIndex(m_bg_opacity_idx));
            break;
        case 'g':
        case 'G':
            m_fg_opacity_idx = (m_fg_opacity_idx + 1) % 3;
            m_mapRenderer.SetForegroundOpacity(OpacityForIndex(m_fg_opacity_idx));
            break;
        case 'e':
        case 'E':
            m_sprite_opacity_idx = (m_sprite_opacity_idx + 1) % 3;
            m_spriteRenderer.SetOpacity(OpacityForIndex(m_sprite_opacity_idx));
            break;
        case 'x':
        case 'X':
            if (ctrl) {
                CutSelectedEntity();
            } else {
                m_show_hitboxes = !m_show_hitboxes;
            }
            break;
        case 'w':
        case 'W':
            if (m_selected_tileswap_region_idx >= 0 && shift) {
                resize_selected_tileswap(0, -1);
            } else if (m_selected_tileswap_region_idx >= 0) {
                NudgeSelectedObject(0.0f, -1.0f, 0.0f);
            } else if (ctrl && m_selected_entity_idx >= 0) {
                SetSelectedEntityOrientation(Landstalker::Orientation::NW);
            } else if (shift && m_selected_warp_idx >= 0) {
                ResizeSelectedWarp(0.0f, -1.0f);
            } else if (ctrl && m_selected_warp_idx >= 0) {
                RotateSelectedWarp(0.0f, -1.0f);
            } else {
                NudgeSelectedObject(0.0f, -1.0f, 0.0f);
            }
            break;
        case 'a':
        case 'A':
            if (m_selected_tileswap_region_idx >= 0 && shift) {
                resize_selected_tileswap(-1, 0);
            } else if (m_selected_tileswap_region_idx >= 0) {
                NudgeSelectedObject(-1.0f, 0.0f, 0.0f);
            } else if (ctrl && m_selected_entity_idx >= 0) {
                SetSelectedEntityOrientation(Landstalker::Orientation::SW);
            } else if (shift && m_selected_warp_idx >= 0) {
                ResizeSelectedWarp(-1.0f, 0.0f);
            } else if (ctrl && m_selected_warp_idx >= 0) {
                RotateSelectedWarp(-1.0f, 0.0f);
            } else {
                NudgeSelectedObject(-1.0f, 0.0f, 0.0f);
            }
            break;
        case 's':
        case 'S':
            if (m_selected_tileswap_region_idx >= 0 && shift) {
                resize_selected_tileswap(0, 1);
            } else if (m_selected_tileswap_region_idx >= 0) {
                NudgeSelectedObject(0.0f, 1.0f, 0.0f);
            } else if (ctrl && m_selected_entity_idx >= 0) {
                SetSelectedEntityOrientation(Landstalker::Orientation::SE);
            } else if (shift && m_selected_warp_idx >= 0) {
                ResizeSelectedWarp(0.0f, 1.0f);
            } else if (ctrl && m_selected_warp_idx >= 0) {
                RotateSelectedWarp(0.0f, 1.0f);
            } else {
                NudgeSelectedObject(0.0f, 1.0f, 0.0f);
            }
            break;
        case 'd':
        case 'D':
            if (m_selected_tileswap_region_idx >= 0 && shift) {
                resize_selected_tileswap(1, 0);
            } else if (m_selected_tileswap_region_idx >= 0) {
                NudgeSelectedObject(1.0f, 0.0f, 0.0f);
            } else if (ctrl && m_selected_entity_idx >= 0) {
                SetSelectedEntityOrientation(Landstalker::Orientation::NE);
            } else if (shift && m_selected_warp_idx >= 0) {
                ResizeSelectedWarp(1.0f, 0.0f);
            } else if (ctrl && m_selected_warp_idx >= 0) {
                RotateSelectedWarp(1.0f, 0.0f);
            } else {
                NudgeSelectedObject(1.0f, 0.0f, 0.0f);
            }
            break;
        case 'r':
        case 'R':
            NudgeSelectedObject(0.0f, 0.0f, 0.5f);
            break;
        case 'f':
        case 'F':
            if (ctrl) {
                SetSelectedEntityToFloor();
            } else {
                NudgeSelectedObject(0.0f, 0.0f, -0.5f);
            }
            break;
        case 'h':
        case 'H': m_show_heightmap = !m_show_heightmap; break;
        case 'z':
        case 'Z':
            m_entity_occlusion_idx = (m_entity_occlusion_idx + 1) % 3;
            break;
        case 'o':
        case 'O':
            m_debug_occlusion = !m_debug_occlusion;
            break;
        case 'l':
        case 'L':
            if (m_selected_entity_idx >= 0 && m_selected_entity_idx < static_cast<int>(m_instances.size())) {
                WriteSelectedEntityOcclusionDebugLog();
            } else {
                WriteEntityDrawOrderDebugLog();
            }
            break;
        case 'p':
        case 'P':
            CycleSelectedEntityPalette();
            break;
        case 'q':
        case 'Q':
            WriteEntityDrawOrderDebugLog();
            break;
    }
    Refresh();
}

void MyGLCanvas::OnMouseWheel(wxMouseEvent& evt) {
    if (evt.ControlDown()) {
        float steps = WheelSteps(evt);
        int delta = steps > 0.0f ? 1 : (steps < 0.0f ? -1 : 0);
        if (delta != 0) {
            int old_idx = std::clamp(m_zoom_step_idx, 0, static_cast<int>(kZoomSteps.size()) - 1);
            int new_idx = std::clamp(old_idx + delta, 0, static_cast<int>(kZoomSteps.size()) - 1);
            if (new_idx != old_idx) {
                float old_zoom = kZoomSteps[static_cast<std::size_t>(old_idx)];
                float new_zoom = kZoomSteps[static_cast<std::size_t>(new_idx)];
                float anchor_x = static_cast<float>(evt.GetPosition().x);
                float anchor_y = static_cast<float>(evt.GetPosition().y);
                float world_x = (anchor_x - m_cam_x) / old_zoom;
                float world_y = (anchor_y - m_cam_y) / old_zoom;
                m_zoom_step_idx = new_idx;
                m_cam_x = anchor_x - world_x * new_zoom;
                m_cam_y = anchor_y - world_y * new_zoom;
                m_cam_x = std::round(m_cam_x);
                m_cam_y = std::round(m_cam_y);
            }
        }
        Refresh();
        return;
    }

    constexpr float wheel_pan_speed = 80.0f;
    float movement = WheelSteps(evt) * wheel_pan_speed;

    if (evt.GetWheelAxis() == wxMOUSE_WHEEL_HORIZONTAL) {
        m_cam_x += movement;
    } else {
        m_cam_y += movement;
    }

    Refresh();
}

void MyGLCanvas::OnSize(wxSizeEvent& evt) {
    if (m_initialized) {
        CenterCameraOnRoom();
        Refresh();
    }
    evt.Skip();
}

void MyGLCanvas::OnMouseMove(wxMouseEvent& evt) {
    m_last_mouse_pos = evt.GetPosition();
    if (m_dragging_pan) {
        m_cam_x = m_drag_pan_start_cam_x + static_cast<float>(evt.GetPosition().x - m_drag_pan_start_mouse.x);
        m_cam_y = m_drag_pan_start_cam_y + static_cast<float>(evt.GetPosition().y - m_drag_pan_start_mouse.y);
        Refresh();
        return;
    }
    if (m_dragging_entity) {
        UpdateEntityDrag(evt);
        return;
    }
    if (m_dragging_warp) {
        UpdateWarpDrag(evt);
        return;
    }
    if (m_dragging_door) {
        UpdateDoorDrag(evt);
        return;
    }
    if (m_dragging_tileswap_region) {
        UpdateTileSwapRegionDrag(evt);
        return;
    }

    if (HitTestRoomInfoLink(evt.GetPosition()) >= 0) {
        SetCursor(wxCursor(wxCURSOR_HAND));
        evt.Skip();
        return;
    }

    m_heightmapRenderer.SetHoverPoint(
        ScreenToWorldX(evt.GetPosition().x),
        ScreenToWorldY(evt.GetPosition().y));

    int hovered_control = HitTestEntityZControl(evt.GetPosition());
    int hovered_body = hovered_control >= 0 ? hovered_control : HitTestEntityBody(evt.GetPosition());
    int hovered_warp_resize = hovered_body < 0 ? HitTestWarpResizeControl(evt.GetPosition()) : 0;
    int hovered_warp = hovered_body < 0 && hovered_warp_resize == 0 ? HitTestWarp(evt.GetPosition()) : -1;
    int hovered = hovered_body >= 0 || hovered_warp >= 0 || hovered_warp_resize != 0 ? hovered_body : HitTestEntity(evt.GetPosition());
    int hovered_tileswap_resize = hovered < 0 && hovered_warp < 0 && hovered_warp_resize == 0
        ? HitTestTileSwapRegionResizeControl(evt.GetPosition())
        : 0;
    int hovered_tileswap_region = hovered < 0 && hovered_warp < 0 && hovered_warp_resize == 0
        && hovered_tileswap_resize == 0
        ? HitTestTileSwapRegion(evt.GetPosition())
        : -1;
    int hovered_door = hovered < 0 && hovered_warp < 0 && hovered_warp_resize == 0
        && hovered_tileswap_resize == 0 && hovered_tileswap_region < 0
        ? HitTestDoor(evt.GetPosition())
        : -1;
    if (hovered != m_hovered_entity_idx || hovered_warp != m_hovered_warp_idx ||
        hovered_tileswap_region != m_hovered_tileswap_region_idx || hovered_door != m_hovered_door_idx) {
        m_hovered_entity_idx = hovered;
        m_hovered_warp_idx = hovered_warp;
        m_hovered_tileswap_region_idx = hovered_tileswap_region;
        m_hovered_door_idx = hovered_door;
    }
    bool z_cursor = hovered_control >= 0 || (m_hovered_entity_idx >= 0 && evt.ControlDown());
    if (z_cursor) {
        SetCursor(wxCursor(wxCURSOR_SIZENS));
    } else if (hovered_warp_resize == 1) {
        SetCursor(wxCursor(wxCURSOR_SIZENWSE));
    } else if (hovered_warp_resize == 2) {
        SetCursor(wxCursor(wxCURSOR_SIZENESW));
    } else if (hovered_tileswap_resize != 0) {
        SetCursor(wxCursor(wxCURSOR_SIZENWSE));
    } else {
        SetCursor(wxCursor((m_hovered_entity_idx >= 0 || m_hovered_warp_idx >= 0 ||
            m_hovered_tileswap_region_idx >= 0 || m_hovered_door_idx >= 0) ? wxCURSOR_HAND : wxCURSOR_ARROW));
    }
    Refresh();
    evt.Skip();
}

void MyGLCanvas::OnLeftDown(wxMouseEvent& evt) {
    m_last_mouse_pos = evt.GetPosition();
    int room_info_room = HitTestRoomInfoLink(evt.GetPosition());
    if (room_info_room >= 0) {
        LoadRoom(static_cast<uint16_t>(room_info_room));
        Refresh();
        return;
    }

    int control_idx = HitTestEntityZControl(evt.GetPosition());
    m_selected_entity_idx = control_idx >= 0 ? control_idx : HitTestEntityBody(evt.GetPosition());
    if (m_selected_entity_idx >= 0) {
        m_selected_warp_idx = -1;
        m_selected_tileswap_region_idx = -1;
        m_selected_door_idx = -1;
        StartEntityDrag(m_selected_entity_idx, evt, control_idx >= 0 || evt.ControlDown());
    } else {
        int resize_axis = HitTestWarpResizeControl(evt.GetPosition());
        if (resize_axis != 0 && m_selected_warp_idx >= 0) {
            m_selected_tileswap_region_idx = -1;
            m_selected_door_idx = -1;
            StartWarpResizeDrag(m_selected_warp_idx, resize_axis, evt);
            Refresh();
            return;
        }

        m_selected_warp_idx = HitTestWarp(evt.GetPosition());
        if (m_selected_warp_idx >= 0) {
            m_selected_tileswap_region_idx = -1;
            m_selected_door_idx = -1;
            StartWarpDrag(m_selected_warp_idx, evt);
        } else {
            m_selected_entity_idx = HitTestEntity(evt.GetPosition());
            if (m_selected_entity_idx >= 0) {
                m_selected_warp_idx = -1;
                m_selected_tileswap_region_idx = -1;
                m_selected_door_idx = -1;
                StartEntityDrag(m_selected_entity_idx, evt, evt.ControlDown(), true);
            } else {
                int tileswap_resize_axis = HitTestTileSwapRegionResizeControl(evt.GetPosition());
                if (tileswap_resize_axis != 0 && m_selected_tileswap_region_idx >= 0) {
                    m_selected_door_idx = -1;
                    StartTileSwapRegionDrag(m_selected_tileswap_region_idx, tileswap_resize_axis, evt);
                    Refresh();
                    return;
                }
                m_selected_tileswap_region_idx = HitTestTileSwapRegion(evt.GetPosition());
                m_hovered_tileswap_region_idx = m_selected_tileswap_region_idx;
                if (m_selected_tileswap_region_idx >= 0) {
                    m_selected_door_idx = -1;
                    StartTileSwapRegionDrag(m_selected_tileswap_region_idx, 0, evt);
                } else {
                    m_selected_door_idx = HitTestDoor(evt.GetPosition());
                    m_hovered_door_idx = m_selected_door_idx;
                    if (m_selected_door_idx >= 0) {
                        StartDoorDrag(m_selected_door_idx, evt);
                    } else {
                        m_dragging_pan = true;
                        m_drag_pan_start_mouse = evt.GetPosition();
                        m_drag_pan_start_cam_x = m_cam_x;
                        m_drag_pan_start_cam_y = m_cam_y;
                        SetCursor(wxCursor(wxCURSOR_SIZING));
                        if (!HasCapture()) {
                            CaptureMouse();
                        }
                    }
                }
            }
        }
    }
    Refresh();
}

void MyGLCanvas::OnLeftUp(wxMouseEvent& evt) {
    if (m_dragging_entity) {
        EndEntityDrag();
    } else if (m_dragging_warp) {
        EndWarpDrag();
    } else if (m_dragging_door) {
        EndDoorDrag();
    } else if (m_dragging_tileswap_region) {
        EndTileSwapRegionDrag();
    } else if (m_dragging_pan) {
        m_dragging_pan = false;
        if (HasCapture()) {
            ReleaseMouse();
        }
        SetCursor(wxCursor(wxCURSOR_ARROW));
    }
    evt.Skip();
}

void MyGLCanvas::OnMiddleDown(wxMouseEvent& evt) {
    m_last_mouse_pos = evt.GetPosition();
    m_dragging_pan = true;
    m_drag_pan_start_mouse = evt.GetPosition();
    m_drag_pan_start_cam_x = m_cam_x;
    m_drag_pan_start_cam_y = m_cam_y;
    SetCursor(wxCursor(wxCURSOR_SIZING));
    if (!HasCapture()) {
        CaptureMouse();
    }
}

void MyGLCanvas::OnMiddleUp(wxMouseEvent& evt) {
    if (m_dragging_pan) {
        m_dragging_pan = false;
        if (HasCapture()) {
            ReleaseMouse();
        }
        SetCursor(wxCursor(wxCURSOR_ARROW));
        Refresh();
    }
    evt.Skip();
}

void MyGLCanvas::OnRightDown(wxMouseEvent& evt) {
    m_last_mouse_pos = evt.GetPosition();
    int room_info_room = HitTestRoomInfoLink(evt.GetPosition());
    if (room_info_room >= 0) {
        LoadRoom(static_cast<uint16_t>(room_info_room));
        Refresh();
        return;
    }

    int warp_idx = HitTestWarp(evt.GetPosition());
    if (warp_idx >= 0) {
        m_selected_warp_idx = warp_idx;
        m_selected_entity_idx = -1;
        m_selected_tileswap_region_idx = -1;
        m_selected_door_idx = -1;
        LoadRoom(m_warps[static_cast<std::size_t>(warp_idx)].DestinationRoom());
        Refresh();
        return;
    }

    m_selected_entity_idx = HitTestEntityBody(evt.GetPosition());
    if (m_selected_entity_idx < 0) {
        m_selected_entity_idx = HitTestEntity(evt.GetPosition());
    }
    if (m_selected_entity_idx >= 0) {
        m_selected_warp_idx = -1;
        m_selected_tileswap_region_idx = -1;
        m_selected_door_idx = -1;
        StartEntityDrag(m_selected_entity_idx, evt, true);
    } else {
        int tileswap_region = HitTestTileSwapRegion(evt.GetPosition());
        if (tileswap_region >= 0) {
            m_selected_entity_idx = -1;
            m_selected_warp_idx = -1;
            m_selected_tileswap_region_idx = tileswap_region;
            m_selected_door_idx = -1;
            m_hovered_tileswap_region_idx = tileswap_region;
            ToggleSelectedTileSwapPreview();
        } else {
            int door_idx = HitTestDoor(evt.GetPosition());
            if (door_idx >= 0) {
                m_selected_entity_idx = -1;
                m_selected_warp_idx = -1;
                m_selected_tileswap_region_idx = -1;
                m_selected_door_idx = door_idx;
                m_hovered_door_idx = door_idx;
                ToggleSelectedDoorPreview();
            }
        }
    }
    Refresh();
}

void MyGLCanvas::OnRightUp(wxMouseEvent& evt) {
    if (m_dragging_entity) {
        EndEntityDrag();
    }
    evt.Skip();
}

void MyGLCanvas::OnMouseLeave(wxMouseEvent& evt) {
    if (m_dragging_entity || m_dragging_warp || m_dragging_door || m_dragging_tileswap_region || m_dragging_pan) {
        evt.Skip();
        return;
    }

    m_hovered_entity_idx = -1;
    m_hovered_warp_idx = -1;
    m_hovered_tileswap_region_idx = -1;
    m_hovered_door_idx = -1;
    m_heightmapRenderer.ClearHover();
    SetCursor(wxCursor(wxCURSOR_ARROW));
    Refresh();
    evt.Skip();
}

void MyGLCanvas::StartEntityDrag(int entity_idx, const wxMouseEvent& evt, bool z_axis_only, bool shadow_drag) {
    if (entity_idx < 0 || entity_idx >= static_cast<int>(m_instances.size())) {
        return;
    }

    SpriteInstance& inst = m_instances[static_cast<std::size_t>(entity_idx)];
    m_dragging_entity = true;
    m_drag_z_axis_only = z_axis_only;
    m_drag_instance_id = inst.instance_id;
    m_drag_start_mouse = evt.GetPosition();
    m_drag_start_x = inst.map_x;
    m_drag_start_y = inst.map_y;
    m_drag_start_z = inst.map_z;
    m_drag_plane_z = shadow_drag ? inst.floor_z : inst.map_z;
    PickPoint cursor_map = ScreenToMapPoint(
        ScreenToWorldX(evt.GetPosition().x),
        ScreenToWorldY(evt.GetPosition().y),
        m_drag_plane_z,
        inst.room_left,
        inst.room_top,
        inst.z_extent);
    m_drag_cursor_offset_x = inst.map_x + inst.hitbox_offset - cursor_map.x;
    m_drag_cursor_offset_y = inst.map_y + inst.hitbox_offset - cursor_map.y;
    m_drag_floor_snap = std::abs(inst.map_z - inst.floor_z) <= 0.01f;
    SetCursor(wxCursor(z_axis_only ? wxCURSOR_SIZENS : wxCURSOR_HAND));
    if (!HasCapture()) {
        CaptureMouse();
    }
}

void MyGLCanvas::UpdateEntityDrag(const wxMouseEvent& evt) {
    int entity_idx = FindInstanceIndex(m_drag_instance_id);
    if (entity_idx < 0) {
        EndEntityDrag();
        return;
    }

    SpriteInstance& inst = m_instances[static_cast<std::size_t>(entity_idx)];
    bool z_axis_only = m_drag_z_axis_only || evt.ControlDown() || evt.RightIsDown();
    SetCursor(wxCursor(z_axis_only ? wxCURSOR_SIZENS : wxCURSOR_HAND));
    auto snap_half = [](float value) {
        return std::round(value * 2.0f) * 0.5f;
    };
    auto clamp_map_pos = [](float value) {
        return std::clamp(value, 0.0f, 63.5f);
    };

    if (z_axis_only) {
        float dy = static_cast<float>(evt.GetPosition().y - m_drag_start_mouse.y);
        inst.map_x = clamp_map_pos(m_drag_start_x);
        inst.map_y = clamp_map_pos(m_drag_start_y);
        inst.map_z = std::clamp(snap_half(m_drag_start_z - dy / 32.0f), 0.0f, 15.5f);
    } else {
        float world_x = ScreenToWorldX(evt.GetPosition().x);
        float world_y = ScreenToWorldY(evt.GetPosition().y);
        PickPoint cursor_map = ScreenToMapPoint(world_x, world_y, m_drag_plane_z, inst.room_left, inst.room_top, inst.z_extent);
        float hitbox_center_x = cursor_map.x + m_drag_cursor_offset_x;
        float hitbox_center_y = cursor_map.y + m_drag_cursor_offset_y;
        inst.map_x = clamp_map_pos(snap_half(hitbox_center_x - inst.hitbox_offset));
        inst.map_y = clamp_map_pos(snap_half(hitbox_center_y - inst.hitbox_offset));
    }

    inst.floor_z = FloorUnderHitbox(
        inst.map_x + inst.hitbox_offset,
        inst.map_y + inst.hitbox_offset,
        inst.hitbox_base * 0.5f);
    if (!z_axis_only && m_drag_floor_snap) {
        inst.map_z = std::clamp(inst.floor_z, 0.0f, 15.5f);
    } else if (!z_axis_only) {
        inst.map_z = std::clamp(m_drag_start_z, 0.0f, 15.5f);
    }
    UpdateEntityProjection(inst);
    SortEntitiesGeometrically(m_instances);
    entity_idx = FindInstanceIndex(m_drag_instance_id);
    m_selected_entity_idx = entity_idx;
    m_hovered_entity_idx = entity_idx;
    Refresh();
}

void MyGLCanvas::EndEntityDrag() {
    if (!m_dragging_entity) {
        return;
    }

    uint32_t dragged_id = m_drag_instance_id;
    m_dragging_entity = false;
    if (HasCapture()) {
        ReleaseMouse();
    }
    SortEntitiesGeometrically(m_instances);
    m_selected_entity_idx = FindInstanceIndex(dragged_id);
    m_hovered_entity_idx = m_selected_entity_idx;
    SetCursor(wxCursor(m_hovered_entity_idx >= 0 ? wxCURSOR_HAND : wxCURSOR_ARROW));
    Refresh();
}

void MyGLCanvas::StartWarpDrag(int warp_idx, const wxMouseEvent& evt) {
    if (warp_idx < 0 || warp_idx >= static_cast<int>(m_warps.size())) {
        return;
    }

    WarpInstance& warp = m_warps[static_cast<std::size_t>(warp_idx)];
    m_dragging_warp = true;
    m_drag_warp_instance_id = warp.instance_id;
    m_drag_warp_resize_axis = 0;
    m_drag_start_mouse = evt.GetPosition();
    m_drag_warp_start_x = warp.x;
    m_drag_warp_start_y = warp.y;
    m_drag_warp_start_width = warp.width;
    m_drag_warp_start_height = warp.height;
    m_drag_warp_start_floor_z = warp.floor_z;
    SetCursor(wxCursor(wxCURSOR_HAND));
    if (!HasCapture()) {
        CaptureMouse();
    }
}

void MyGLCanvas::StartWarpResizeDrag(int warp_idx, int axis, const wxMouseEvent& evt) {
    if (warp_idx < 0 || warp_idx >= static_cast<int>(m_warps.size())) {
        return;
    }

    WarpInstance& warp = m_warps[static_cast<std::size_t>(warp_idx)];
    m_dragging_warp = true;
    m_drag_warp_instance_id = warp.instance_id;
    m_drag_warp_resize_axis = axis;
    m_drag_start_mouse = evt.GetPosition();
    m_drag_warp_start_x = warp.x;
    m_drag_warp_start_y = warp.y;
    m_drag_warp_start_width = warp.width;
    m_drag_warp_start_height = warp.height;
    m_drag_warp_start_floor_z = warp.floor_z;
    m_selected_warp_idx = warp_idx;
    m_selected_entity_idx = -1;
    SetCursor(wxCursor(axis == 1 ? wxCURSOR_SIZENWSE : wxCURSOR_SIZENESW));
    if (!HasCapture()) {
        CaptureMouse();
    }
}

void MyGLCanvas::UpdateWarpDrag(const wxMouseEvent& evt) {
    int warp_idx = FindWarpIndex(m_drag_warp_instance_id);
    if (warp_idx < 0) {
        EndWarpDrag();
        return;
    }

    WarpInstance& warp = m_warps[static_cast<std::size_t>(warp_idx)];
    auto snap_cell = [](float value) {
        return std::round(value);
    };
    auto clamp_map_pos = [](float value) {
        return std::clamp(value, 0.0f, 63.5f);
    };

    float world_x = ScreenToWorldX(evt.GetPosition().x);
    float world_y = ScreenToWorldY(evt.GetPosition().y);
    float a = (world_x - 512.0f) / 32.0f;
    float b = (world_y - 100.0f + warp.z_extent * m_drag_warp_start_floor_z) / 16.0f;
    float center_x = (a + b) * 0.5f + warp.room_left;
    float center_y = (b - a) * 0.5f + warp.room_top;
    if (m_drag_warp_resize_axis == 1) {
        warp.x = m_drag_warp_start_x;
        warp.y = m_drag_warp_start_y;
        warp.height = ValidWarpHeight(m_drag_warp_start_height, m_drag_warp_start_width);
        warp.width = std::min(ValidWarpWidth(center_x - m_drag_warp_start_x, warp.height), 63.5f - m_drag_warp_start_x);
        SetCursor(wxCursor(wxCURSOR_SIZENWSE));
    } else if (m_drag_warp_resize_axis == 2) {
        warp.x = m_drag_warp_start_x;
        warp.y = m_drag_warp_start_y;
        warp.width = ValidWarpWidth(m_drag_warp_start_width, m_drag_warp_start_height);
        warp.height = std::min(ValidWarpHeight(center_y - m_drag_warp_start_y, warp.width), 63.5f - m_drag_warp_start_y);
        SetCursor(wxCursor(wxCURSOR_SIZENESW));
    } else {
        warp.x = clamp_map_pos(snap_cell(center_x - warp.width * 0.5f));
        warp.y = clamp_map_pos(snap_cell(center_y - warp.height * 0.5f));
        SetCursor(wxCursor(wxCURSOR_HAND));
    }
    UpdateWarpFloor(warp);

    m_selected_warp_idx = warp_idx;
    m_selected_entity_idx = -1;
    m_hovered_warp_idx = warp_idx;
    m_hovered_entity_idx = -1;
    Refresh();
}

void MyGLCanvas::EndWarpDrag() {
    if (!m_dragging_warp) {
        return;
    }

    uint32_t dragged_id = m_drag_warp_instance_id;
    m_dragging_warp = false;
    m_drag_warp_resize_axis = 0;
    if (HasCapture()) {
        ReleaseMouse();
    }
    m_selected_warp_idx = FindWarpIndex(dragged_id);
    m_hovered_warp_idx = m_selected_warp_idx;
    SetCursor(wxCursor(m_hovered_warp_idx >= 0 ? wxCURSOR_HAND : wxCURSOR_ARROW));
    Refresh();
}

void MyGLCanvas::StartDoorDrag(int door_idx, const wxMouseEvent& evt) {
    auto rd = m_gd ? m_gd->GetRoomData() : nullptr;
    if (!rd) {
        return;
    }
    auto doors = rd->GetDoors(m_current_room);
    if (door_idx < 0 || door_idx >= static_cast<int>(doors.size())) {
        return;
    }

    ClearTileSwapPreview();
    const Door& door = doors[static_cast<std::size_t>(door_idx)];
    m_dragging_door = true;
    m_drag_door_idx = door_idx;
    m_drag_start_mouse = evt.GetPosition();
    m_drag_door_start_x = door.x;
    m_drag_door_start_y = door.y;
    m_selected_door_idx = door_idx;
    m_selected_entity_idx = -1;
    m_selected_warp_idx = -1;
    m_selected_tileswap_region_idx = -1;
    SetCursor(wxCursor(wxCURSOR_HAND));
    if (!HasCapture()) {
        CaptureMouse();
    }
}

void MyGLCanvas::UpdateDoorDrag(const wxMouseEvent& evt) {
    auto rd = m_gd ? m_gd->GetRoomData() : nullptr;
    if (!rd || m_drag_door_idx < 0) {
        EndDoorDrag();
        return;
    }
    auto doors = rd->GetDoors(m_current_room);
    if (m_drag_door_idx >= static_cast<int>(doors.size())) {
        EndDoorDrag();
        return;
    }

    PickPoint start = ScreenToHeightmapPoint(
        ScreenToWorldX(m_drag_start_mouse.x),
        ScreenToWorldY(m_drag_start_mouse.y),
        static_cast<float>(m_mapRenderer.GetRoomLeft()),
        static_cast<float>(m_mapRenderer.GetRoomTop()));
    PickPoint current = ScreenToHeightmapPoint(
        ScreenToWorldX(evt.GetPosition().x),
        ScreenToWorldY(evt.GetPosition().y),
        static_cast<float>(m_mapRenderer.GetRoomLeft()),
        static_cast<float>(m_mapRenderer.GetRoomTop()));
    int dx = static_cast<int>(std::round(current.x - start.x));
    int dy = static_cast<int>(std::round(current.y - start.y));

    Door& door = doors[static_cast<std::size_t>(m_drag_door_idx)];
    door.x = static_cast<uint8_t>(std::clamp(m_drag_door_start_x + dx, 0, 63));
    door.y = static_cast<uint8_t>(std::clamp(m_drag_door_start_y + dy, 0, 63));
    rd->SetDoors(m_current_room, doors);

    m_selected_door_idx = m_drag_door_idx;
    m_hovered_door_idx = m_drag_door_idx;
    SetCursor(wxCursor(wxCURSOR_HAND));
    Refresh();
}

void MyGLCanvas::EndDoorDrag() {
    if (!m_dragging_door) {
        return;
    }

    m_dragging_door = false;
    if (HasCapture()) {
        ReleaseMouse();
    }
    m_hovered_door_idx = m_selected_door_idx;
    SetCursor(wxCursor(m_hovered_door_idx >= 0 ? wxCURSOR_HAND : wxCURSOR_ARROW));
    Refresh();
}

void MyGLCanvas::StartTileSwapRegionDrag(int region_idx, int resize_axis, const wxMouseEvent& evt) {
    auto regions = BuildTileSwapRegionGeometries(m_gd, m_current_room, m_mapRenderer, m_heightmapRenderer.GetZExtent());
    if (region_idx < 0 || region_idx >= static_cast<int>(regions.size())) {
        return;
    }

    const auto& region = regions[static_cast<std::size_t>(region_idx)];
    TileSwapRegionMetrics metrics = MetricsForTileSwapRegion(region.swap, region.part);
    m_dragging_tileswap_region = true;
    m_drag_tileswap_region_idx = region_idx;
    m_drag_tileswap_resize_axis = resize_axis;
    m_drag_start_mouse = evt.GetPosition();
    m_drag_tileswap_start_x = metrics.x;
    m_drag_tileswap_start_y = metrics.y;
    m_drag_tileswap_start_width = metrics.width;
    m_drag_tileswap_start_height = metrics.height;
    m_selected_tileswap_region_idx = region_idx;
    m_selected_entity_idx = -1;
    m_selected_warp_idx = -1;
    SetCursor(wxCursor(resize_axis != 0 ? wxCURSOR_SIZENWSE : wxCURSOR_HAND));
    if (!HasCapture()) {
        CaptureMouse();
    }
}

void MyGLCanvas::UpdateTileSwapRegionDrag(const wxMouseEvent& evt) {
    auto regions = BuildTileSwapRegionGeometries(m_gd, m_current_room, m_mapRenderer, m_heightmapRenderer.GetZExtent());
    if (m_drag_tileswap_region_idx < 0 || m_drag_tileswap_region_idx >= static_cast<int>(regions.size())) {
        EndTileSwapRegionDrag();
        return;
    }

    const auto& region = regions[static_cast<std::size_t>(m_drag_tileswap_region_idx)];
    int dx = 0;
    int dy = 0;
    if (region.part == TileSwapRegionPart::TilemapSource ||
        region.part == TileSwapRegionPart::TilemapDestination) {
        PickPoint start = ScreenToMapPoint(
            ScreenToWorldX(m_drag_start_mouse.x),
            ScreenToWorldY(m_drag_start_mouse.y),
            0.0f,
            static_cast<float>(m_mapRenderer.GetRoomLeft()),
            static_cast<float>(m_mapRenderer.GetRoomTop()));
        PickPoint current = ScreenToMapPoint(
            ScreenToWorldX(evt.GetPosition().x),
            ScreenToWorldY(evt.GetPosition().y),
            0.0f,
            static_cast<float>(m_mapRenderer.GetRoomLeft()),
            static_cast<float>(m_mapRenderer.GetRoomTop()));
        dx = static_cast<int>(std::trunc(current.x - start.x));
        dy = static_cast<int>(std::trunc(current.y - start.y));
    } else {
        PickPoint start = ScreenToHeightmapPoint(
            ScreenToWorldX(m_drag_start_mouse.x),
            ScreenToWorldY(m_drag_start_mouse.y),
            static_cast<float>(m_mapRenderer.GetRoomLeft()),
            static_cast<float>(m_mapRenderer.GetRoomTop()));
        PickPoint current = ScreenToHeightmapPoint(
            ScreenToWorldX(evt.GetPosition().x),
            ScreenToWorldY(evt.GetPosition().y),
            static_cast<float>(m_mapRenderer.GetRoomLeft()),
            static_cast<float>(m_mapRenderer.GetRoomTop()));
        dx = static_cast<int>(std::trunc(current.x - start.x));
        dy = static_cast<int>(std::trunc(current.y - start.y));
    }

    if (m_drag_tileswap_resize_axis != 0) {
        int horizontal_width_delta = static_cast<int>(std::trunc(
            (ScreenToWorldX(evt.GetPosition().x) - ScreenToWorldX(m_drag_start_mouse.x)) / 32.0f));
        int vertical_height_delta = static_cast<int>(std::trunc(
            (ScreenToWorldY(evt.GetPosition().y) - ScreenToWorldY(m_drag_start_mouse.y)) / 32.0f));

        ClearTileSwapPreview();
        auto rd = m_gd ? m_gd->GetRoomData() : nullptr;
        if (!rd) {
            return;
        }
        auto swaps = rd->GetTileSwaps(m_current_room);
        if (region.swap_index < 0 || region.swap_index >= static_cast<int>(swaps.size())) {
            return;
        }

        auto clamp_size = [&](int value, int origin) {
            return std::clamp(value, 1, std::max(1, 64 - origin));
        };

        auto apply_size = [&](std::vector<TileSwap>& target_swaps, int width, int height) {
            TileSwap& swap = target_swaps[static_cast<std::size_t>(region.swap_index)];
            TileSwapRegionMetrics metrics = MetricsForTileSwapRegion(swap, region.part);
            int clamped_w = clamp_size(width, metrics.x);
            int clamped_h = clamp_size(height, metrics.y);
            if (region.part == TileSwapRegionPart::TilemapSource ||
                region.part == TileSwapRegionPart::TilemapDestination) {
                swap.map.width = static_cast<uint8_t>(clamped_w);
                swap.map.height = static_cast<uint8_t>(clamped_h);
            } else {
                swap.heightmap.width = static_cast<uint8_t>(clamped_w);
                swap.heightmap.height = static_cast<uint8_t>(clamped_h);
            }
        };

        int base_w = std::max(1, m_drag_tileswap_start_width);
        int base_h = std::max(1, m_drag_tileswap_start_height);

        auto choose_best_nw_mapping = [&]() {
            struct Candidate {
                int w;
                int h;
            };
            std::array<Candidate, 2> candidates = {{
                {base_w + horizontal_width_delta, base_h + vertical_height_delta},
                {base_w - horizontal_width_delta, base_h + vertical_height_delta}
            }};

            PickPoint cursor{
                ScreenToWorldX(evt.GetPosition().x),
                ScreenToWorldY(evt.GetPosition().y)
            };

            float best_dist = std::numeric_limits<float>::max();
            std::vector<TileSwap> best_swaps = swaps;
            for (const auto& candidate : candidates) {
                auto test_swaps = swaps;
                apply_size(test_swaps, candidate.w, candidate.h);
                rd->SetTileSwaps(m_current_room, test_swaps);
                auto test_regions = BuildTileSwapRegionGeometries(m_gd, m_current_room, m_mapRenderer, m_heightmapRenderer.GetZExtent());
                if (m_drag_tileswap_region_idx < 0 ||
                    m_drag_tileswap_region_idx >= static_cast<int>(test_regions.size())) {
                    continue;
                }
                PickPoint handle = test_regions[static_cast<std::size_t>(m_drag_tileswap_region_idx)].resize_handle;
                float dxh = handle.x - cursor.x;
                float dyh = handle.y - cursor.y;
                float dist = dxh * dxh + dyh * dyh;
                if (dist < best_dist) {
                    best_dist = dist;
                    best_swaps = std::move(test_swaps);
                }
            }
            rd->SetTileSwaps(m_current_room, best_swaps);
        };

        if (region.swap.mode == TileSwap::Mode::FLOOR) {
            apply_size(swaps, base_w + dx, base_h + dy);
            rd->SetTileSwaps(m_current_room, swaps);
        } else if (region.swap.mode == TileSwap::Mode::WALL_NW) {
            choose_best_nw_mapping();
        } else {
            apply_size(swaps, base_w + horizontal_width_delta, base_h + vertical_height_delta);
            rd->SetTileSwaps(m_current_room, swaps);
        }
    } else {
        ClearTileSwapPreview();
        auto rd = m_gd ? m_gd->GetRoomData() : nullptr;
        if (!rd) {
            return;
        }
        auto swaps = rd->GetTileSwaps(m_current_room);
        if (region.swap_index < 0 || region.swap_index >= static_cast<int>(swaps.size())) {
            return;
        }
        TileSwap& swap = swaps[static_cast<std::size_t>(region.swap_index)];
        TileSwapRegionMetrics metrics = MetricsForTileSwapRegion(swap, region.part);
        int x = std::clamp(m_drag_tileswap_start_x + dx, 0, std::max(0, 64 - metrics.width));
        int y = std::clamp(m_drag_tileswap_start_y + dy, 0, std::max(0, 64 - metrics.height));
        switch (region.part) {
            case TileSwapRegionPart::TilemapSource:
                swap.map.src_x = static_cast<uint8_t>(x);
                swap.map.src_y = static_cast<uint8_t>(y);
                break;
            case TileSwapRegionPart::TilemapDestination:
                swap.map.dst_x = static_cast<uint8_t>(x);
                swap.map.dst_y = static_cast<uint8_t>(y);
                break;
            case TileSwapRegionPart::HeightmapSource:
                swap.heightmap.src_x = static_cast<uint8_t>(x);
                swap.heightmap.src_y = static_cast<uint8_t>(y);
                break;
            case TileSwapRegionPart::HeightmapDestination:
                swap.heightmap.dst_x = static_cast<uint8_t>(x);
                swap.heightmap.dst_y = static_cast<uint8_t>(y);
                break;
        }
        rd->SetTileSwaps(m_current_room, swaps);
    }

    m_selected_tileswap_region_idx = m_drag_tileswap_region_idx;
    m_hovered_tileswap_region_idx = m_drag_tileswap_region_idx;
    SetCursor(wxCursor(m_drag_tileswap_resize_axis != 0 ? wxCURSOR_SIZENWSE : wxCURSOR_HAND));
    Refresh();
}

void MyGLCanvas::EndTileSwapRegionDrag() {
    if (!m_dragging_tileswap_region) {
        return;
    }
    m_dragging_tileswap_region = false;
    m_drag_tileswap_resize_axis = 0;
    if (HasCapture()) {
        ReleaseMouse();
    }
    SetCursor(wxCursor(m_hovered_tileswap_region_idx >= 0 ? wxCURSOR_HAND : wxCURSOR_ARROW));
    Refresh();
}

float MyGLCanvas::ZoomFactor() const {
    int idx = std::clamp(m_zoom_step_idx, 0, static_cast<int>(kZoomSteps.size()) - 1);
    return kZoomSteps[static_cast<std::size_t>(idx)];
}

float MyGLCanvas::ScreenToWorldX(int screen_x) const {
    return (static_cast<float>(screen_x) - m_cam_x) / ZoomFactor();
}

float MyGLCanvas::ScreenToWorldY(int screen_y) const {
    return (static_cast<float>(screen_y) - m_cam_y) / ZoomFactor();
}

void MyGLCanvas::RefreshObjectPlacementsFromHeightmap() {
    for (auto& inst : m_instances) {
        inst.z_extent = m_heightmapRenderer.GetZExtent();
        inst.floor_z = FloorUnderHitbox(
            inst.map_x + inst.hitbox_offset,
            inst.map_y + inst.hitbox_offset,
            inst.hitbox_base * 0.5f);
        UpdateEntityProjection(inst);
    }

    for (auto& warp : m_warps) {
        warp.z_extent = m_heightmapRenderer.GetZExtent();
        UpdateWarpFloor(warp);
    }
}

void MyGLCanvas::UpdateEntityProjection(SpriteInstance& inst) {
    float ex_block = inst.map_x + inst.hitbox_offset - inst.room_left;
    float ey_block = inst.map_y + inst.hitbox_offset - inst.room_top;
    inst.x = 32.0f * ex_block - 32.0f * ey_block + 512.0f;
    inst.y = 16.0f * ex_block + 16.0f * ey_block + 100.0f - inst.map_z * inst.z_extent;
}

void MyGLCanvas::UpdateWarpFloor(WarpInstance& warp) {
    warp.floor_z = FloorUnderRect(warp.x, warp.y, warp.x + warp.width, warp.y + warp.height);
}

void MyGLCanvas::CenterCameraOnRoom() {
    int client_w = 0;
    int client_h = 0;
    GetClientSize(&client_w, &client_h);
    if (client_w <= 0 || client_h <= 0 || m_mapRenderer.GetRoomWidth() <= 0 || m_mapRenderer.GetRoomHeight() <= 0) {
        m_cam_x = 0.0f;
        m_cam_y = 0.0f;
        return;
    }

    auto project = [](float x, float y) {
        return PickPoint{
            32.0f * x - 32.0f * y + 512.0f,
            16.0f * x + 16.0f * y + 100.0f
        };
    };

    float room_w = float(m_mapRenderer.GetRoomWidth());
    float room_h = float(m_mapRenderer.GetRoomHeight());
    std::array<PickPoint, 4> corners = {
        project(0.0f, 0.0f),
        project(room_w, 0.0f),
        project(0.0f, room_h),
        project(room_w, room_h)
    };

    float min_x = corners.front().x;
    float max_x = corners.front().x;
    float min_y = corners.front().y;
    float max_y = corners.front().y;
    for (const auto& point : corners) {
        min_x = std::min(min_x, point.x);
        max_x = std::max(max_x, point.x);
        min_y = std::min(min_y, point.y);
        max_y = std::max(max_y, point.y);
    }

    float zoom = ZoomFactor();
    m_cam_x = float(client_w) * 0.5f - ((min_x + max_x) * 0.5f) * zoom;
    m_cam_y = float(client_h) * 0.5f - ((min_y + max_y) * 0.5f) * zoom;
}

void MyGLCanvas::EnsureWorldRectVisible(float min_x, float min_y, float max_x, float max_y) {
    int client_w = 0;
    int client_h = 0;
    GetClientSize(&client_w, &client_h);
    if (client_w <= 0 || client_h <= 0) {
        return;
    }

    if (min_x > max_x) {
        std::swap(min_x, max_x);
    }
    if (min_y > max_y) {
        std::swap(min_y, max_y);
    }

    float zoom = std::max(ZoomFactor(), 0.0001f);
    float view_min_x = ScreenToWorldX(0);
    float view_min_y = ScreenToWorldY(0);
    float view_max_x = ScreenToWorldX(client_w);
    float view_max_y = ScreenToWorldY(client_h);
    if (view_min_x > view_max_x) {
        std::swap(view_min_x, view_max_x);
    }
    if (view_min_y > view_max_y) {
        std::swap(view_min_y, view_max_y);
    }

    constexpr float margin_px = 36.0f;
    float margin_world = margin_px / zoom;
    bool visible_x = min_x >= view_min_x + margin_world && max_x <= view_max_x - margin_world;
    bool visible_y = min_y >= view_min_y + margin_world && max_y <= view_max_y - margin_world;
    if (visible_x && visible_y) {
        return;
    }

    float center_x = (min_x + max_x) * 0.5f;
    float center_y = (min_y + max_y) * 0.5f;
    m_cam_x = float(client_w) * 0.5f - center_x * zoom;
    m_cam_y = float(client_h) * 0.5f - center_y * zoom;
    m_cam_x = std::round(m_cam_x);
    m_cam_y = std::round(m_cam_y);
}

void MyGLCanvas::FocusCameraOnSelectedObjectIfNeeded() {
    if (m_selected_entity_idx >= 0 && m_selected_entity_idx < static_cast<int>(m_instances.size())) {
        const SpriteInstance& inst = m_instances[static_cast<std::size_t>(m_selected_entity_idx)];
        float center_x = inst.map_x + inst.hitbox_offset;
        float center_y = inst.map_y + inst.hitbox_offset;
        float half_base = std::max(inst.hitbox_base * 0.5f, 0.5f);
        float top_z = inst.map_z + std::max(inst.hitbox_height, 0.125f);
        PickPoint p0 = ProjectEntityGridPoint(inst, center_x - half_base, center_y - half_base, top_z);
        PickPoint p1 = ProjectEntityGridPoint(inst, center_x + half_base, center_y + half_base, inst.map_z);
        EnsureWorldRectVisible(
            std::min(p0.x, p1.x),
            std::min(p0.y, p1.y),
            std::max(p0.x, p1.x),
            std::max(p0.y, p1.y));
        return;
    }

    if (m_selected_warp_idx >= 0 && m_selected_warp_idx < static_cast<int>(m_warps.size())) {
        const WarpInstance& warp = m_warps[static_cast<std::size_t>(m_selected_warp_idx)];
        float z = warp.floor_z;
        PickPoint p0 = ProjectWarpGridPoint(warp, warp.x, warp.y, z);
        PickPoint p1 = ProjectWarpGridPoint(warp, warp.x + warp.width, warp.y + warp.height, z);
        EnsureWorldRectVisible(
            std::min(p0.x, p1.x),
            std::min(p0.y, p1.y),
            std::max(p0.x, p1.x),
            std::max(p0.y, p1.y));
        return;
    }

    if (m_selected_tileswap_region_idx >= 0) {
        auto regions = BuildTileSwapRegionGeometries(m_gd, m_current_room, m_mapRenderer, m_heightmapRenderer.GetZExtent());
        if (m_selected_tileswap_region_idx < static_cast<int>(regions.size())) {
            const auto& region = regions[static_cast<std::size_t>(m_selected_tileswap_region_idx)];
            EnsureWorldRectVisible(region.bounds.min_x, region.bounds.min_y, region.bounds.max_x, region.bounds.max_y);
        }
        return;
    }

    if (m_selected_door_idx >= 0) {
        auto doors = BuildDoorGeometries(
            m_gd,
            m_current_room,
            m_mapRenderer,
            m_heightmapRenderer.GetZExtent(),
            m_tileswap_preview_map);
        for (const auto& door : doors) {
            if (door.index == m_selected_door_idx) {
                EnsureWorldRectVisible(door.bounds.min_x, door.bounds.min_y, door.bounds.max_x, door.bounds.max_y);
                break;
            }
        }
    }
}

void MyGLCanvas::RefreshEntityMetadata(SpriteInstance& inst) {
    auto sd = m_gd->GetSpriteData();
    if (sd->IsEntity(inst.entity_id)) {
        auto hitbox = sd->GetEntityHitbox(inst.entity_id);
        inst.hitbox_base = HitboxBaseToBlocks(hitbox.base);
        inst.hitbox_height = HitboxHeightToBlocks(hitbox.height);
    } else {
        inst.hitbox_base = 1.0f;
        inst.hitbox_height = 1.0f;
    }
    inst.hitbox_offset = HitboxDrawOffset(inst.hitbox_base);
    inst.floor_z = FloorUnderHitbox(
        inst.map_x + inst.hitbox_offset,
        inst.map_y + inst.hitbox_offset,
        inst.hitbox_base * 0.5f);
    UpdateEntityProjection(inst);
}

void MyGLCanvas::PersistCurrentRoomEdits() {
    if (m_room_entities.empty() && m_instances.empty() && m_warps.empty()) {
        return;
    }

    std::vector<Landstalker::Entity> entities(m_instances.size());
    for (const auto& inst : m_instances) {
        std::size_t idx = inst.instance_id > 0 ? std::size_t(inst.instance_id - 1) : entities.size();
        if (idx >= entities.size()) {
            continue;
        }
        Landstalker::Entity entity = idx < m_room_entities.size() ? m_room_entities[idx] : Landstalker::Entity{};
        entity.SetType(inst.entity_id);
        entity.SetPalette(std::min<uint8_t>(inst.palette, 3));
        entity.SetOrientation(inst.orientation);
        entity.SetXDbl(inst.map_x);
        entity.SetYDbl(inst.map_y);
        entity.SetZDbl(inst.map_z);
        entities[idx] = entity;
    }
    m_gd->GetSpriteData()->SetRoomEntities(m_current_room, entities);
    m_room_entities = entities;

    std::vector<Landstalker::WarpList::Warp> warps;
    std::map<uint32_t, std::size_t> warp_slots;
    for (const auto& inst : m_warps) {
        uint32_t key = inst.warp_key != 0 ? inst.warp_key : inst.instance_id;
        auto slot_it = warp_slots.find(key);
        if (slot_it == warp_slots.end()) {
            warp_slots[key] = warps.size();
            warps.push_back(inst.warp);
            slot_it = warp_slots.find(key);
        }
        Landstalker::WarpList::Warp& warp = warps[slot_it->second];
        if (inst.current_room_is_room1) {
            warp.room1 = m_current_room;
            warp.x1 = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::round(inst.x)), 0, 63));
            warp.y1 = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::round(inst.y)), 0, 63));
        } else {
            warp.room2 = m_current_room;
            warp.x2 = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::round(inst.x)), 0, 63));
            warp.y2 = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::round(inst.y)), 0, 63));
        }
        warp.x_size = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::round(inst.width)), 1, 63));
        warp.y_size = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::round(inst.height)), 1, 63));
        if (m_pending_warp_half &&
            m_pending_warp_room == m_current_room &&
            inst.instance_id == m_pending_warp_instance_id &&
            inst.DestinationRoom() == 0xFFFF) {
            m_pending_warp = warp;
        }
    }
    warps.erase(
        std::remove_if(
            warps.begin(),
            warps.end(),
            [](const auto& warp) {
                return warp.room1 == 0xFFFF || warp.room2 == 0xFFFF || !warp.IsValid();
            }),
        warps.end());
    m_gd->GetRoomData()->SetWarpsForRoom(m_current_room, warps);
}

void MyGLCanvas::AddEntity() {
    if (m_room_entities.size() >= 15) {
        return;
    }

    Landstalker::Entity entity;
    float x = float(m_mapRenderer.GetRoomLeft()) + float(m_mapRenderer.GetRoomWidth()) * 0.5f;
    float y = float(m_mapRenderer.GetRoomTop()) + float(m_mapRenderer.GetRoomHeight()) * 0.5f;
    entity.SetXDbl(std::clamp<double>(x, 0.5, 63.5));
    entity.SetYDbl(std::clamp<double>(y, 0.5, 63.5));
    entity.SetZDbl(FloorUnderPoint(float(entity.GetXDbl()), float(entity.GetYDbl())));
    m_room_entities.push_back(entity);

    SpriteInstance inst{};
    inst.instance_id = static_cast<uint32_t>(m_room_entities.size());
    inst.entity_id = entity.GetType();
    inst.palette = entity.GetPalette();
    inst.map_x = float(entity.GetXDbl());
    inst.map_y = float(entity.GetYDbl());
    inst.map_z = float(entity.GetZDbl());
    inst.z_extent = m_heightmapRenderer.GetZExtent();
    inst.room_left = float(m_mapRenderer.GetRoomLeft());
    inst.room_top = float(m_mapRenderer.GetRoomTop());
    inst.dx = 0.0f;
    inst.dy = 0.0f;
    inst.scale = 2.0f;
    inst.anim_timer = 0.0f;
    inst.anim_speed = 1.0f;
    inst.orientation = entity.GetOrientation();
    RefreshEntityMetadata(inst);
    m_instances.push_back(inst);
    SortEntitiesGeometrically(m_instances);
    m_selected_entity_idx = FindInstanceIndex(inst.instance_id);
    m_selected_warp_idx = -1;
}

void MyGLCanvas::CopySelectedEntity() {
    if (m_selected_entity_idx < 0 || m_selected_entity_idx >= static_cast<int>(m_instances.size())) {
        return;
    }

    const SpriteInstance& inst = m_instances[static_cast<std::size_t>(m_selected_entity_idx)];
    std::size_t slot = inst.instance_id > 0 ? std::size_t(inst.instance_id - 1) : m_room_entities.size();
    Landstalker::Entity entity = slot < m_room_entities.size() ? m_room_entities[slot] : Landstalker::Entity{};
    entity.SetType(inst.entity_id);
    entity.SetPalette(std::min<uint8_t>(inst.palette, 3));
    entity.SetOrientation(inst.orientation);
    entity.SetXDbl(inst.map_x);
    entity.SetYDbl(inst.map_y);
    entity.SetZDbl(inst.map_z);
    m_entity_clipboard = entity;
    m_entity_clipboard_valid = true;
}

void MyGLCanvas::PasteEntity() {
    if (!m_entity_clipboard_valid || m_room_entities.size() >= 15) {
        return;
    }

    Landstalker::Entity entity = m_entity_clipboard;
    auto occupied = [this](double x, double y) {
        for (const auto& inst : m_instances) {
            if (std::abs(double(inst.map_x) - x) < 0.25 && std::abs(double(inst.map_y) - y) < 0.25) {
                return true;
            }
        }
        return false;
    };
    double x = std::clamp<double>(entity.GetXDbl() + 1.0, 0.5, 63.5);
    double y = std::clamp<double>(entity.GetYDbl(), 0.5, 63.5);
    for (int tries = 0; tries < 128 && occupied(x, y); ++tries) {
        x += 1.0;
        if (x > 63.5) {
            x = 0.5;
            y = std::clamp<double>(y + 1.0, 0.5, 63.5);
        }
    }
    entity.SetXDbl(x);
    entity.SetYDbl(y);
    m_room_entities.push_back(entity);

    SpriteInstance inst{};
    inst.instance_id = static_cast<uint32_t>(m_room_entities.size());
    inst.entity_id = entity.GetType();
    inst.palette = entity.GetPalette();
    inst.map_x = float(entity.GetXDbl());
    inst.map_y = float(entity.GetYDbl());
    inst.map_z = std::clamp(float(entity.GetZDbl()), 0.0f, 15.5f);
    inst.z_extent = m_heightmapRenderer.GetZExtent();
    inst.room_left = float(m_mapRenderer.GetRoomLeft());
    inst.room_top = float(m_mapRenderer.GetRoomTop());
    inst.dx = 0.0f;
    inst.dy = 0.0f;
    inst.scale = 2.0f;
    inst.anim_timer = 0.0f;
    inst.anim_speed = 1.0f;
    inst.orientation = entity.GetOrientation();
    RefreshEntityMetadata(inst);
    m_instances.push_back(inst);
    SortEntitiesGeometrically(m_instances);
    m_selected_entity_idx = FindInstanceIndex(inst.instance_id);
    m_hovered_entity_idx = m_selected_entity_idx;
    m_selected_warp_idx = -1;
    m_hovered_warp_idx = -1;
}

void MyGLCanvas::CutSelectedEntity() {
    if (m_selected_entity_idx < 0 || m_selected_entity_idx >= static_cast<int>(m_instances.size())) {
        return;
    }

    CopySelectedEntity();
    DeleteSelectedObject();
}

void MyGLCanvas::DeleteSelectedObject() {
    if (m_selected_entity_idx >= 0 && m_selected_entity_idx < static_cast<int>(m_instances.size())) {
        uint32_t id = m_instances[static_cast<std::size_t>(m_selected_entity_idx)].instance_id;
        std::size_t slot = id > 0 ? std::size_t(id - 1) : m_room_entities.size();
        if (slot < m_room_entities.size()) {
            m_room_entities.erase(m_room_entities.begin() + static_cast<std::ptrdiff_t>(slot));
        }
        m_instances.erase(m_instances.begin() + m_selected_entity_idx);
        for (auto& inst : m_instances) {
            if (inst.instance_id > id) {
                --inst.instance_id;
            }
        }
        m_selected_entity_idx = -1;
        m_hovered_entity_idx = -1;
        return;
    }

    if (m_selected_warp_idx >= 0 && m_selected_warp_idx < static_cast<int>(m_warps.size())) {
        const WarpInstance& selected = m_warps[static_cast<std::size_t>(m_selected_warp_idx)];
        if (m_pending_warp_half &&
            m_current_room == m_pending_warp_room &&
            selected.instance_id == m_pending_warp_instance_id &&
            selected.DestinationRoom() == 0xFFFF) {
            m_pending_warp_half = false;
            m_pending_warp_room = 0xFFFF;
            m_pending_warp_instance_id = 0;
        }
        m_warps.erase(m_warps.begin() + m_selected_warp_idx);
        for (std::size_t i = 0; i < m_warps.size(); ++i) {
            m_warps[i].instance_id = static_cast<uint32_t>(i + 1);
        }
        m_selected_warp_idx = -1;
        m_hovered_warp_idx = -1;
        return;
    }

    if (m_selected_tileswap_region_idx >= 0) {
        ClearTileSwapPreview();
        auto regions = BuildTileSwapRegionGeometries(m_gd, m_current_room, m_mapRenderer, m_heightmapRenderer.GetZExtent());
        if (m_selected_tileswap_region_idx >= static_cast<int>(regions.size())) {
            m_selected_tileswap_region_idx = -1;
            m_hovered_tileswap_region_idx = -1;
            return;
        }
        int swap_index = regions[static_cast<std::size_t>(m_selected_tileswap_region_idx)].swap_index;
        auto rd = m_gd ? m_gd->GetRoomData() : nullptr;
        if (!rd) {
            return;
        }
        auto swaps = rd->GetTileSwaps(m_current_room);
        if (swap_index >= 0 && swap_index < static_cast<int>(swaps.size())) {
            swaps.erase(swaps.begin() + swap_index);
            rd->SetTileSwaps(m_current_room, swaps);
        }
        m_selected_tileswap_region_idx = -1;
        m_hovered_tileswap_region_idx = -1;
        return;
    }

    if (m_selected_door_idx >= 0) {
        ClearTileSwapPreview();
        auto rd = m_gd ? m_gd->GetRoomData() : nullptr;
        if (!rd) {
            return;
        }
        auto doors = rd->GetDoors(m_current_room);
        if (m_selected_door_idx >= 0 && m_selected_door_idx < static_cast<int>(doors.size())) {
            doors.erase(doors.begin() + m_selected_door_idx);
            rd->SetDoors(m_current_room, doors);
        }
        m_selected_door_idx = -1;
        m_hovered_door_idx = -1;
    }
}

void MyGLCanvas::ReorderSelectedObject(int delta) {
    if (m_selected_entity_idx >= 0 && m_selected_entity_idx < static_cast<int>(m_instances.size())) {
        uint32_t id = m_instances[static_cast<std::size_t>(m_selected_entity_idx)].instance_id;
        int slot = static_cast<int>(id) - 1;
        int new_slot = std::clamp(slot + delta, 0, static_cast<int>(m_room_entities.size()) - 1);
        if (new_slot == slot) {
            return;
        }
        std::swap(m_room_entities[static_cast<std::size_t>(slot)], m_room_entities[static_cast<std::size_t>(new_slot)]);
        for (auto& inst : m_instances) {
            if (inst.instance_id == static_cast<uint32_t>(slot + 1)) {
                inst.instance_id = static_cast<uint32_t>(new_slot + 1);
            } else if (inst.instance_id == static_cast<uint32_t>(new_slot + 1)) {
                inst.instance_id = static_cast<uint32_t>(slot + 1);
            }
        }
        m_selected_entity_idx = FindInstanceIndex(static_cast<uint32_t>(new_slot + 1));
        return;
    }

    if (m_selected_warp_idx >= 0 && m_selected_warp_idx < static_cast<int>(m_warps.size())) {
        int new_idx = std::clamp(m_selected_warp_idx + delta, 0, static_cast<int>(m_warps.size()) - 1);
        if (new_idx != m_selected_warp_idx) {
            std::swap(m_warps[static_cast<std::size_t>(m_selected_warp_idx)], m_warps[static_cast<std::size_t>(new_idx)]);
            m_selected_warp_idx = new_idx;
        }
        return;
    }

    if (m_selected_door_idx >= 0) {
        auto rd = m_gd ? m_gd->GetRoomData() : nullptr;
        if (!rd) {
            return;
        }
        auto doors = rd->GetDoors(m_current_room);
        if (m_selected_door_idx >= static_cast<int>(doors.size())) {
            m_selected_door_idx = -1;
            m_hovered_door_idx = -1;
            return;
        }
        int new_idx = std::clamp(m_selected_door_idx + delta, 0, static_cast<int>(doors.size()) - 1);
        if (new_idx != m_selected_door_idx) {
            std::swap(doors[static_cast<std::size_t>(m_selected_door_idx)], doors[static_cast<std::size_t>(new_idx)]);
            rd->SetDoors(m_current_room, doors);
            m_selected_door_idx = new_idx;
            m_hovered_door_idx = new_idx;
        }
    }
}

void MyGLCanvas::SelectNextObject(int direction) {
    enum class SelectionType {
        Entity,
        Warp,
        TileSwapRegion,
        Door
    };

    struct TabEntry {
        SelectionType type;
        int index;
        int id;
        int part_order;
    };

    auto region_part_order = [](TileSwapRegionPart part) {
        switch (part) {
            case TileSwapRegionPart::TilemapSource: return 0;
            case TileSwapRegionPart::TilemapDestination: return 1;
            case TileSwapRegionPart::HeightmapSource: return 2;
            case TileSwapRegionPart::HeightmapDestination: return 3;
        }
        return 4;
    };

    auto type_order = [](SelectionType type) {
        switch (type) {
            case SelectionType::Entity: return 0;
            case SelectionType::Warp: return 1;
            case SelectionType::TileSwapRegion: return 2;
            case SelectionType::Door: return 3;
        }
        return 4;
    };

    std::vector<TabEntry> order;
    order.reserve(m_instances.size() + m_warps.size());

    for (const auto& inst : m_instances) {
        order.push_back({
            SelectionType::Entity,
            static_cast<int>(inst.instance_id),
            static_cast<int>(inst.instance_id),
            0
        });
    }

    for (const auto& warp : m_warps) {
        order.push_back({
            SelectionType::Warp,
            static_cast<int>(warp.instance_id),
            static_cast<int>(warp.instance_id),
            0
        });
    }

    auto regions = BuildTileSwapRegionGeometries(m_gd, m_current_room, m_mapRenderer, m_heightmapRenderer.GetZExtent());
    auto rd = m_gd ? m_gd->GetRoomData() : nullptr;
    std::vector<TileSwap> swaps = rd ? rd->GetTileSwaps(m_current_room) : std::vector<TileSwap>{};
    for (const auto& region : regions) {
        if (region.swap_index < 0 || region.swap_index >= static_cast<int>(swaps.size())) {
            continue;
        }
        const TileSwap& swap = swaps[static_cast<std::size_t>(region.swap_index)];
        order.push_back({
            SelectionType::TileSwapRegion,
            region.flat_index,
            static_cast<int>(swap.trigger),
            region_part_order(region.part)
        });
    }

    if (rd) {
        auto doors = rd->GetDoors(m_current_room);
        for (int i = 0; i < static_cast<int>(doors.size()); ++i) {
            order.push_back({SelectionType::Door, i, i + 1, 0});
        }
    }

    if (order.empty()) {
        m_selected_entity_idx = -1;
        m_selected_warp_idx = -1;
        m_selected_tileswap_region_idx = -1;
        m_selected_door_idx = -1;
        return;
    }

    std::stable_sort(order.begin(), order.end(), [&](const TabEntry& lhs, const TabEntry& rhs) {
        int lhs_type = type_order(lhs.type);
        int rhs_type = type_order(rhs.type);
        if (lhs_type != rhs_type) {
            return lhs_type < rhs_type;
        }
        if (lhs.id != rhs.id) {
            return lhs.id < rhs.id;
        }
        if (lhs.part_order != rhs.part_order) {
            return lhs.part_order < rhs.part_order;
        }
        return lhs.index < rhs.index;
    });

    int current = -1;
    if (m_selected_entity_idx >= 0 && m_selected_entity_idx < static_cast<int>(m_instances.size())) {
        int selected_id = static_cast<int>(m_instances[static_cast<std::size_t>(m_selected_entity_idx)].instance_id);
        for (int i = 0; i < static_cast<int>(order.size()); ++i) {
            if (order[static_cast<std::size_t>(i)].type == SelectionType::Entity &&
                order[static_cast<std::size_t>(i)].id == selected_id) {
                current = i;
                break;
            }
        }
    } else if (m_selected_warp_idx >= 0 && m_selected_warp_idx < static_cast<int>(m_warps.size())) {
        int selected_id = static_cast<int>(m_warps[static_cast<std::size_t>(m_selected_warp_idx)].instance_id);
        for (int i = 0; i < static_cast<int>(order.size()); ++i) {
            if (order[static_cast<std::size_t>(i)].type == SelectionType::Warp &&
                order[static_cast<std::size_t>(i)].id == selected_id) {
                current = i;
                break;
            }
        }
    } else if (m_selected_tileswap_region_idx >= 0) {
        for (int i = 0; i < static_cast<int>(order.size()); ++i) {
            if (order[static_cast<std::size_t>(i)].type == SelectionType::TileSwapRegion &&
                order[static_cast<std::size_t>(i)].index == m_selected_tileswap_region_idx) {
                current = i;
                break;
            }
        }
    } else if (m_selected_door_idx >= 0) {
        for (int i = 0; i < static_cast<int>(order.size()); ++i) {
            if (order[static_cast<std::size_t>(i)].type == SelectionType::Door &&
                order[static_cast<std::size_t>(i)].index == m_selected_door_idx) {
                current = i;
                break;
            }
        }
    }

    int step = direction >= 0 ? 1 : -1;
    int next = -1;
    if (current < 0) {
        next = step > 0 ? 0 : static_cast<int>(order.size()) - 1;
    } else {
        next = (current + step + static_cast<int>(order.size())) % static_cast<int>(order.size());
    }

    const TabEntry& target = order[static_cast<std::size_t>(next)];
    if (target.type == SelectionType::Entity) {
        m_selected_entity_idx = FindInstanceIndex(static_cast<uint32_t>(target.id));
        m_hovered_entity_idx = m_selected_entity_idx;
        m_selected_warp_idx = -1;
        m_hovered_warp_idx = -1;
        m_selected_tileswap_region_idx = -1;
        m_hovered_tileswap_region_idx = -1;
        m_selected_door_idx = -1;
        m_hovered_door_idx = -1;
    } else if (target.type == SelectionType::Warp) {
        m_selected_entity_idx = -1;
        m_hovered_entity_idx = -1;
        m_selected_warp_idx = FindWarpIndex(static_cast<uint32_t>(target.id));
        m_hovered_warp_idx = m_selected_warp_idx;
        m_selected_tileswap_region_idx = -1;
        m_hovered_tileswap_region_idx = -1;
        m_selected_door_idx = -1;
        m_hovered_door_idx = -1;
    } else if (target.type == SelectionType::TileSwapRegion) {
        m_selected_entity_idx = -1;
        m_hovered_entity_idx = -1;
        m_selected_warp_idx = -1;
        m_hovered_warp_idx = -1;
        m_selected_tileswap_region_idx = target.index;
        m_hovered_tileswap_region_idx = m_selected_tileswap_region_idx;
        m_selected_door_idx = -1;
        m_hovered_door_idx = -1;
    } else {
        m_selected_entity_idx = -1;
        m_hovered_entity_idx = -1;
        m_selected_warp_idx = -1;
        m_hovered_warp_idx = -1;
        m_selected_tileswap_region_idx = -1;
        m_hovered_tileswap_region_idx = -1;
        m_selected_door_idx = target.index;
        m_hovered_door_idx = target.index;
    }

    FocusCameraOnSelectedObjectIfNeeded();
}

void MyGLCanvas::SelectNextTileSwapRegion(int direction) {
    auto regions = BuildTileSwapRegionGeometries(m_gd, m_current_room, m_mapRenderer, m_heightmapRenderer.GetZExtent());
    if (regions.empty()) {
        m_selected_tileswap_region_idx = -1;
        return;
    }
    int current = m_selected_tileswap_region_idx >= 0 ? m_selected_tileswap_region_idx : -1;
    int next = (current + direction) % static_cast<int>(regions.size());
    if (next < 0) {
        next += static_cast<int>(regions.size());
    }
    m_selected_entity_idx = -1;
    m_hovered_entity_idx = -1;
    m_selected_warp_idx = -1;
    m_hovered_warp_idx = -1;
    m_selected_tileswap_region_idx = next;
    m_hovered_tileswap_region_idx = next;
}

void MyGLCanvas::CycleSelectedEntityId(int delta) {
    if (m_selected_entity_idx < 0 || m_selected_entity_idx >= static_cast<int>(m_instances.size())) {
        return;
    }
    SpriteInstance& inst = m_instances[static_cast<std::size_t>(m_selected_entity_idx)];
    inst.entity_id = static_cast<uint8_t>((int(inst.entity_id) + delta + 256) & 0xFF);
    RefreshEntityMetadata(inst);
}

void MyGLCanvas::CycleSelectedEntityPalette() {
    if (m_selected_entity_idx < 0 || m_selected_entity_idx >= static_cast<int>(m_instances.size())) {
        return;
    }
    SpriteInstance& inst = m_instances[static_cast<std::size_t>(m_selected_entity_idx)];
    inst.palette = static_cast<uint8_t>((inst.palette + 1) % 4);
}

void MyGLCanvas::SetSelectedEntityOrientation(Landstalker::Orientation orientation) {
    if (m_selected_entity_idx < 0 || m_selected_entity_idx >= static_cast<int>(m_instances.size())) {
        return;
    }
    m_instances[static_cast<std::size_t>(m_selected_entity_idx)].orientation = orientation;
}

void MyGLCanvas::SetSelectedEntityToFloor() {
    if (m_selected_entity_idx < 0 || m_selected_entity_idx >= static_cast<int>(m_instances.size())) {
        return;
    }
    SpriteInstance& inst = m_instances[static_cast<std::size_t>(m_selected_entity_idx)];
    inst.floor_z = FloorUnderHitbox(inst.map_x + inst.hitbox_offset, inst.map_y + inst.hitbox_offset, inst.hitbox_base * 0.5f);
    inst.map_z = std::clamp(inst.floor_z, 0.0f, 15.5f);
    UpdateEntityProjection(inst);
}

void MyGLCanvas::AddWarpHalf() {
    Landstalker::WarpList::Warp warp;
    auto [preferred_x, preferred_y] = MouseHeightmapCell();
    auto [x, y] = FindNearestFreeWarpCell(static_cast<float>(preferred_x), static_cast<float>(preferred_y));
    bool update_pending_instance = false;
    if (!m_pending_warp_half) {
        warp.room1 = m_current_room;
        warp.x1 = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::round(x)), 0, 63));
        warp.y1 = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::round(y)), 0, 63));
        warp.room2 = 0xFFFF;
        m_pending_warp = warp;
        m_pending_warp_half = true;
        m_pending_warp_room = m_current_room;
        m_pending_warp_instance_id = static_cast<uint32_t>(m_warps.size() + 1);
    } else {
        if (m_pending_warp_room == m_current_room && m_pending_warp_instance_id != 0) {
            int pending_idx = FindWarpIndex(m_pending_warp_instance_id);
            if (pending_idx >= 0) {
                const WarpInstance& pending = m_warps[static_cast<std::size_t>(pending_idx)];
                m_pending_warp.x1 = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::round(pending.x)), 0, 63));
                m_pending_warp.y1 = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::round(pending.y)), 0, 63));
                m_pending_warp.x_size = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::round(pending.width)), 1, 63));
                m_pending_warp.y_size = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::round(pending.height)), 1, 63));
                m_pending_warp.type = pending.warp.type;
            }
        }
        warp = m_pending_warp;
        warp.room2 = m_current_room;
        warp.x2 = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::round(x)), 0, 63));
        warp.y2 = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::round(y)), 0, 63));
        update_pending_instance = m_pending_warp_room == m_current_room;
        m_pending_warp_half = false;
        m_pending_warp_room = 0xFFFF;
    }

    if (update_pending_instance) {
        int pending_idx = FindWarpIndex(m_pending_warp_instance_id);
        if (pending_idx < 0) {
            for (std::size_t i = 0; i < m_warps.size(); ++i) {
                const WarpInstance& candidate = m_warps[i];
                if (candidate.current_room_is_room1 &&
                    candidate.warp.room1 == warp.room1 &&
                    candidate.warp.room2 == 0xFFFF &&
                    candidate.warp.x1 == warp.x1 &&
                    candidate.warp.y1 == warp.y1) {
                    pending_idx = static_cast<int>(i);
                    break;
                }
            }
        }
        uint32_t key = m_pending_warp_instance_id;
        if (pending_idx >= 0) {
            key = m_warps[static_cast<std::size_t>(pending_idx)].warp_key;
            WarpInstance first_inst = MakeWarpInstance(
                warp,
                m_current_room,
                m_warps[static_cast<std::size_t>(pending_idx)].instance_id,
                float(m_mapRenderer.GetRoomLeft()),
                float(m_mapRenderer.GetRoomTop()),
                m_heightmapRenderer.GetZExtent(),
                key,
                1);
            UpdateWarpFloor(first_inst);
            m_warps[static_cast<std::size_t>(pending_idx)] = first_inst;
        } else {
            WarpInstance first_inst = MakeWarpInstance(
                warp,
                m_current_room,
                static_cast<uint32_t>(m_warps.size() + 1),
                float(m_mapRenderer.GetRoomLeft()),
                float(m_mapRenderer.GetRoomTop()),
                m_heightmapRenderer.GetZExtent(),
                key,
                1);
            UpdateWarpFloor(first_inst);
            m_warps.push_back(first_inst);
        }
        WarpInstance second_inst = MakeWarpInstance(
            warp,
            m_current_room,
            static_cast<uint32_t>(m_warps.size() + 1),
            float(m_mapRenderer.GetRoomLeft()),
            float(m_mapRenderer.GetRoomTop()),
            m_heightmapRenderer.GetZExtent(),
            key,
            2);
        UpdateWarpFloor(second_inst);
        m_warps.push_back(second_inst);
        m_selected_warp_idx = static_cast<int>(m_warps.size()) - 1;
    } else {
        WarpInstance inst = MakeWarpInstance(
            warp,
            m_current_room,
            static_cast<uint32_t>(m_warps.size() + 1),
            float(m_mapRenderer.GetRoomLeft()),
            float(m_mapRenderer.GetRoomTop()),
            m_heightmapRenderer.GetZExtent());
        UpdateWarpFloor(inst);
        m_warps.push_back(inst);
        m_selected_warp_idx = static_cast<int>(m_warps.size()) - 1;
    }
    if (warp.IsValid()) {
        for (std::size_t i = 0; i < m_warps.size(); ) {
            const WarpInstance& candidate = m_warps[i];
            if (static_cast<int>(i) != m_selected_warp_idx &&
                candidate.warp.room1 == warp.room1 &&
                candidate.warp.room2 == 0xFFFF &&
                candidate.warp.x1 == warp.x1 &&
                candidate.warp.y1 == warp.y1) {
                m_warps.erase(m_warps.begin() + static_cast<std::ptrdiff_t>(i));
                if (m_selected_warp_idx > static_cast<int>(i)) {
                    --m_selected_warp_idx;
                }
                continue;
            }
            ++i;
        }
        for (std::size_t i = 0; i < m_warps.size(); ++i) {
            m_warps[i].instance_id = static_cast<uint32_t>(i + 1);
        }
    }
    if (!m_pending_warp_half) {
        m_pending_warp_instance_id = 0;
        if (warp.IsValid()) {
            PersistCurrentRoomEdits();
        }
    }
    m_selected_entity_idx = -1;
}

std::pair<float, float> MyGLCanvas::FindNearestFreeWarpCell(float preferred_x, float preferred_y) const {
    int start_x = std::clamp(static_cast<int>(std::round(preferred_x)), 0, 63);
    int start_y = std::clamp(static_cast<int>(std::round(preferred_y)), 0, 63);

    auto cell_free = [this](int x, int y) {
        float fx = static_cast<float>(x);
        float fy = static_cast<float>(y);
        for (const auto& warp : m_warps) {
            if (fx < warp.x + warp.width &&
                fx + 1.0f > warp.x &&
                fy < warp.y + warp.height &&
                fy + 1.0f > warp.y) {
                return false;
            }
        }
        return true;
    };

    int best_x = start_x;
    int best_y = start_y;
    int best_dist = std::numeric_limits<int>::max();
    for (int y = 0; y <= 63; ++y) {
        for (int x = 0; x <= 63; ++x) {
            if (!cell_free(x, y)) {
                continue;
            }
            int dx = x - start_x;
            int dy = y - start_y;
            int dist = dx * dx + dy * dy;
            if (dist < best_dist) {
                best_dist = dist;
                best_x = x;
                best_y = y;
            }
        }
    }

    return {static_cast<float>(best_x), static_cast<float>(best_y)};
}

std::pair<int, int> MyGLCanvas::MouseHeightmapCell() const {
    if (m_last_mouse_pos.x < 0 || m_last_mouse_pos.y < 0) {
        return {
            std::clamp(m_mapRenderer.GetRoomLeft() + m_mapRenderer.GetRoomWidth() / 2, 0, 63),
            std::clamp(m_mapRenderer.GetRoomTop() + m_mapRenderer.GetRoomHeight() / 2, 0, 63)
        };
    }
    PickPoint point = ScreenToHeightmapPoint(
        ScreenToWorldX(m_last_mouse_pos.x),
        ScreenToWorldY(m_last_mouse_pos.y),
        static_cast<float>(m_mapRenderer.GetRoomLeft()),
        static_cast<float>(m_mapRenderer.GetRoomTop()));
    return {
        std::clamp(static_cast<int>(std::round(point.x)), 0, 63),
        std::clamp(static_cast<int>(std::round(point.y)), 0, 63)
    };
}

void MyGLCanvas::ResizeSelectedWarp(float dx, float dy) {
    if (m_selected_warp_idx < 0 || m_selected_warp_idx >= static_cast<int>(m_warps.size())) {
        return;
    }
    WarpInstance& warp = m_warps[static_cast<std::size_t>(m_selected_warp_idx)];
    if (dx != 0.0f) {
        warp.height = ValidWarpHeight(warp.height, warp.width);
        warp.width = ValidWarpWidth(warp.width + dx, warp.height);
    }
    if (dy != 0.0f) {
        warp.width = ValidWarpWidth(warp.width, warp.height);
        warp.height = ValidWarpHeight(warp.height + dy, warp.width);
    }
    ClampWarpToValidSize(warp);
    UpdateWarpFloor(warp);
}

void MyGLCanvas::RotateSelectedWarp(float dx, float dy) {
    if (m_selected_warp_idx < 0 || m_selected_warp_idx >= static_cast<int>(m_warps.size())) {
        return;
    }
    WarpInstance& warp = m_warps[static_cast<std::size_t>(m_selected_warp_idx)];
    warp.x = std::clamp(warp.x + dx, 0.0f, 63.5f);
    warp.y = std::clamp(warp.y + dy, 0.0f, 63.5f);
    UpdateWarpFloor(warp);
}

void MyGLCanvas::CycleSelectedWarpType(int delta) {
    if (m_selected_warp_idx < 0 || m_selected_warp_idx >= static_cast<int>(m_warps.size())) {
        return;
    }
    WarpInstance& warp = m_warps[static_cast<std::size_t>(m_selected_warp_idx)];
    int type = static_cast<int>(warp.warp.type);
    type = (type + delta + 3) % 3;
    warp.warp.type = static_cast<Landstalker::WarpList::Warp::Type>(type);
}

void MyGLCanvas::CycleSelectedDoorSize(int delta) {
    ClearTileSwapPreview();
    auto rd = m_gd ? m_gd->GetRoomData() : nullptr;
    if (!rd || m_selected_door_idx < 0) {
        return;
    }
    auto doors = rd->GetDoors(m_current_room);
    if (m_selected_door_idx >= static_cast<int>(doors.size()) || delta == 0) {
        return;
    }

    static constexpr std::array<Door::Size, 4> sizes{
        Door::Size::DOOR_1X4,
        Door::Size::DOOR_2X4,
        Door::Size::DOOR_2X5,
        Door::Size::DOOR_1X0
    };

    Door& door = doors[static_cast<std::size_t>(m_selected_door_idx)];
    auto it = std::find(sizes.begin(), sizes.end(), door.size);
    int idx = it == sizes.end() ? 0 : static_cast<int>(std::distance(sizes.begin(), it));
    idx = (idx + delta + static_cast<int>(sizes.size())) % static_cast<int>(sizes.size());
    door.size = sizes[static_cast<std::size_t>(idx)];
    rd->SetDoors(m_current_room, doors);
}

void MyGLCanvas::AddDoor() {
    ClearTileSwapPreview();
    auto rd = m_gd ? m_gd->GetRoomData() : nullptr;
    if (!rd) {
        return;
    }

    auto doors = rd->GetDoors(m_current_room);
    auto [preferred_x, preferred_y] = MouseHeightmapCell();
    auto cell_used = [&doors](int x, int y) {
        for (const auto& door : doors) {
            if (static_cast<int>(door.x) == x && static_cast<int>(door.y) == y) {
                return true;
            }
        }
        return false;
    };

    int best_x = preferred_x;
    int best_y = preferred_y;
    int best_dist = std::numeric_limits<int>::max();
    bool found = false;
    for (int y = 0; y <= 63; ++y) {
        for (int x = 0; x <= 63; ++x) {
            if (cell_used(x, y)) {
                continue;
            }
            int dx = x - preferred_x;
            int dy = y - preferred_y;
            int dist = dx * dx + dy * dy;
            if (dist < best_dist) {
                best_dist = dist;
                best_x = x;
                best_y = y;
                found = true;
            }
        }
    }
    if (!found) {
        return;
    }

    doors.emplace_back(
        static_cast<uint8_t>(best_x),
        static_cast<uint8_t>(best_y),
        Door::Size::DOOR_1X4);
    rd->SetDoors(m_current_room, doors);

    m_selected_entity_idx = -1;
    m_selected_warp_idx = -1;
    m_selected_tileswap_region_idx = -1;
    m_hovered_entity_idx = -1;
    m_hovered_warp_idx = -1;
    m_hovered_tileswap_region_idx = -1;
    m_selected_door_idx = static_cast<int>(doors.size() - 1);
    m_hovered_door_idx = m_selected_door_idx;
}

void MyGLCanvas::AddTileSwap() {
    ClearTileSwapPreview();
    auto rd = m_gd ? m_gd->GetRoomData() : nullptr;
    if (!rd) {
        return;
    }

    auto swaps = rd->GetTileSwaps(m_current_room);
    std::set<int> used_triggers;
    for (const auto& swap : swaps) {
        used_triggers.insert(static_cast<int>(swap.trigger));
    }

    int trigger = -1;
    for (int candidate = 0; candidate <= 31; ++candidate) {
        if (used_triggers.count(candidate) == 0) {
            trigger = candidate;
            break;
        }
    }
    if (trigger < 0) {
        return;
    }

    auto cell_used = [&swaps](int x, int y) {
        for (const auto& swap : swaps) {
            auto contains = [x, y](const TileSwap::CopyOp& op, bool source) {
                int rx = source ? op.src_x : op.dst_x;
                int ry = source ? op.src_y : op.dst_y;
                return x >= rx && x < rx + op.width && y >= ry && y < ry + op.height;
            };
            if (contains(swap.map, true) || contains(swap.map, false) ||
                contains(swap.heightmap, true) || contains(swap.heightmap, false)) {
                return true;
            }
        }
        return false;
    };

    auto [preferred_x, preferred_y] = MouseHeightmapCell();
    preferred_x = std::clamp(preferred_x, 0, 62);
    preferred_y = std::clamp(preferred_y, 0, 63);
    int src_x = preferred_x;
    int src_y = preferred_y;
    bool found = false;
    int best_dist = std::numeric_limits<int>::max();
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 63; ++x) {
            if (!cell_used(x, y) && !cell_used(x + 1, y)) {
                int dx = x - preferred_x;
                int dy = y - preferred_y;
                int dist = dx * dx + dy * dy;
                if (dist < best_dist) {
                    best_dist = dist;
                    src_x = x;
                    src_y = y;
                    found = true;
                }
            }
        }
    }
    if (!found) {
        return;
    }

    TileSwap swap;
    swap.trigger = static_cast<uint8_t>(trigger);
    swap.mode = TileSwap::Mode::FLOOR;
    swap.map = {
        static_cast<uint8_t>(src_x),
        static_cast<uint8_t>(src_y),
        static_cast<uint8_t>(src_x + 1),
        static_cast<uint8_t>(src_y),
        1,
        1
    };
    swap.heightmap = swap.map;
    swaps.push_back(swap);
    rd->SetTileSwaps(m_current_room, swaps);

    m_selected_entity_idx = -1;
    m_selected_warp_idx = -1;
    m_selected_tileswap_region_idx = static_cast<int>((swaps.size() - 1) * 4);
    m_hovered_tileswap_region_idx = m_selected_tileswap_region_idx;
}

void MyGLCanvas::CycleSelectedTileSwapShape(int delta) {
    ClearTileSwapPreview();
    auto regions = BuildTileSwapRegionGeometries(m_gd, m_current_room, m_mapRenderer, m_heightmapRenderer.GetZExtent());
    if (m_selected_tileswap_region_idx < 0 ||
        m_selected_tileswap_region_idx >= static_cast<int>(regions.size())) {
        return;
    }
    const auto& region = regions[static_cast<std::size_t>(m_selected_tileswap_region_idx)];
    auto rd = m_gd ? m_gd->GetRoomData() : nullptr;
    if (!rd) {
        return;
    }
    auto swaps = rd->GetTileSwaps(m_current_room);
    if (region.swap_index < 0 || region.swap_index >= static_cast<int>(swaps.size())) {
        return;
    }
    int mode = static_cast<int>(swaps[static_cast<std::size_t>(region.swap_index)].mode);
    mode = (mode + delta + 3) % 3;
    swaps[static_cast<std::size_t>(region.swap_index)].mode = static_cast<TileSwap::Mode>(mode);
    rd->SetTileSwaps(m_current_room, swaps);
}

void MyGLCanvas::CycleSelectedTileSwapId(int delta) {
    ClearTileSwapPreview();
    auto regions = BuildTileSwapRegionGeometries(m_gd, m_current_room, m_mapRenderer, m_heightmapRenderer.GetZExtent());
    if (m_selected_tileswap_region_idx < 0 ||
        m_selected_tileswap_region_idx >= static_cast<int>(regions.size())) {
        return;
    }

    const auto& region = regions[static_cast<std::size_t>(m_selected_tileswap_region_idx)];
    auto rd = m_gd ? m_gd->GetRoomData() : nullptr;
    if (!rd) {
        return;
    }
    auto swaps = rd->GetTileSwaps(m_current_room);
    if (region.swap_index < 0 || region.swap_index >= static_cast<int>(swaps.size()) || delta == 0) {
        return;
    }

    std::set<int> used_triggers;
    for (int i = 0; i < static_cast<int>(swaps.size()); ++i) {
        if (i != region.swap_index) {
            used_triggers.insert(static_cast<int>(swaps[static_cast<std::size_t>(i)].trigger));
        }
    }

    int id = static_cast<int>(swaps[static_cast<std::size_t>(region.swap_index)].trigger);
    int step = delta >= 0 ? 1 : -1;
    for (int attempt = 0; attempt < 32; ++attempt) {
        int candidate = (id + step * (attempt + 1) + 32) % 32;
        if (used_triggers.count(candidate) == 0) {
            swaps[static_cast<std::size_t>(region.swap_index)].trigger = static_cast<uint8_t>(candidate);
            rd->SetTileSwaps(m_current_room, swaps);
            return;
        }
    }

    // If every trigger is already in use, swap with the next/previous trigger owner.
    int candidate = (id + step + 32) % 32;
    for (int i = 0; i < static_cast<int>(swaps.size()); ++i) {
        if (i == region.swap_index) {
            continue;
        }
        if (static_cast<int>(swaps[static_cast<std::size_t>(i)].trigger) == candidate) {
            swaps[static_cast<std::size_t>(i)].trigger = static_cast<uint8_t>(id);
            swaps[static_cast<std::size_t>(region.swap_index)].trigger = static_cast<uint8_t>(candidate);
            rd->SetTileSwaps(m_current_room, swaps);
            return;
        }
    }
}

void MyGLCanvas::ResizeSelectedTileSwapRegion(float requested_width, float requested_height) {
    ClearTileSwapPreview();
    auto regions = BuildTileSwapRegionGeometries(m_gd, m_current_room, m_mapRenderer, m_heightmapRenderer.GetZExtent());
    if (m_selected_tileswap_region_idx < 0 ||
        m_selected_tileswap_region_idx >= static_cast<int>(regions.size())) {
        return;
    }
    const auto& region = regions[static_cast<std::size_t>(m_selected_tileswap_region_idx)];
    auto rd = m_gd ? m_gd->GetRoomData() : nullptr;
    if (!rd) {
        return;
    }
    auto swaps = rd->GetTileSwaps(m_current_room);
    if (region.swap_index < 0 || region.swap_index >= static_cast<int>(swaps.size())) {
        return;
    }
    TileSwap& swap = swaps[static_cast<std::size_t>(region.swap_index)];
    TileSwapRegionMetrics metrics = MetricsForTileSwapRegion(swap, region.part);
    int width = metrics.width;
    int height = metrics.height;
    if (requested_width > 0.0f) {
        width = std::clamp(static_cast<int>(std::round(requested_width)), 1, std::max(1, 64 - metrics.x));
    }
    if (requested_height > 0.0f) {
        height = std::clamp(static_cast<int>(std::round(requested_height)), 1, std::max(1, 64 - metrics.y));
    }
    if (region.part == TileSwapRegionPart::TilemapSource ||
        region.part == TileSwapRegionPart::TilemapDestination) {
        swap.map.width = static_cast<uint8_t>(width);
        swap.map.height = static_cast<uint8_t>(height);
    } else {
        swap.heightmap.width = static_cast<uint8_t>(width);
        swap.heightmap.height = static_cast<uint8_t>(height);
    }
    rd->SetTileSwaps(m_current_room, swaps);
}

void MyGLCanvas::ToggleSelectedTileSwapPreview() {
    auto regions = BuildTileSwapRegionGeometries(m_gd, m_current_room, m_mapRenderer, m_heightmapRenderer.GetZExtent());
    if (m_selected_tileswap_region_idx < 0 ||
        m_selected_tileswap_region_idx >= static_cast<int>(regions.size())) {
        return;
    }

    int swap_index = regions[static_cast<std::size_t>(m_selected_tileswap_region_idx)].swap_index;
    if (m_tileswap_preview_active && m_tileswap_preview_swap_index == swap_index) {
        ClearTileSwapPreview();
        return;
    }

    ClearTileSwapPreview();

    auto rd = m_gd ? m_gd->GetRoomData() : nullptr;
    if (!rd) {
        return;
    }
    auto map_entry = rd->GetMapForRoom(m_current_room);
    if (!map_entry) {
        return;
    }
    auto map = map_entry->GetData();
    if (!map) {
        return;
    }
    auto swaps = rd->GetTileSwaps(m_current_room);
    if (swap_index < 0 || swap_index >= static_cast<int>(swaps.size())) {
        return;
    }

    m_tileswap_preview_map = std::make_shared<Tilemap3D>(*map);
    const TileSwap& swap = swaps[static_cast<std::size_t>(swap_index)];
    swap.DrawSwap(*m_tileswap_preview_map, Tilemap3D::Layer::BG);
    swap.DrawSwap(*m_tileswap_preview_map, Tilemap3D::Layer::FG);
    swap.DrawHeightmapSwap(*m_tileswap_preview_map);

    m_tileswap_preview_active = true;
    m_tileswap_preview_swap_index = swap_index;
    m_mapRenderer.LoadPreviewRoom(m_current_room, *m_tileswap_preview_map);
    m_heightmapRenderer.SetPreviewMap(m_tileswap_preview_map);
    RefreshObjectPlacementsFromHeightmap();
}

void MyGLCanvas::ToggleSelectedDoorPreview() {
    auto rd = m_gd ? m_gd->GetRoomData() : nullptr;
    if (!rd || m_selected_door_idx < 0) {
        return;
    }

    auto doors = rd->GetDoors(m_current_room);
    if (m_selected_door_idx >= static_cast<int>(doors.size())) {
        return;
    }

    if (m_door_preview_active && m_door_preview_idx == m_selected_door_idx) {
        ClearTileSwapPreview();
        return;
    }

    ClearTileSwapPreview();

    auto map_entry = rd->GetMapForRoom(m_current_room);
    if (!map_entry) {
        return;
    }
    auto map = map_entry->GetData();
    if (!map) {
        return;
    }

    m_tileswap_preview_map = std::make_shared<Tilemap3D>(*map);
    doors[static_cast<std::size_t>(m_selected_door_idx)].DrawDoor(*m_tileswap_preview_map, Tilemap3D::Layer::FG);

    m_door_preview_active = true;
    m_door_preview_idx = m_selected_door_idx;
    m_mapRenderer.LoadPreviewRoom(m_current_room, *m_tileswap_preview_map);
    m_heightmapRenderer.SetPreviewMap(m_tileswap_preview_map);
    RefreshObjectPlacementsFromHeightmap();
}

void MyGLCanvas::ClearTileSwapPreview() {
    if (!m_tileswap_preview_active && !m_door_preview_active && !m_tileswap_preview_map) {
        return;
    }

    m_tileswap_preview_active = false;
    m_tileswap_preview_swap_index = -1;
    m_door_preview_active = false;
    m_door_preview_idx = -1;
    m_tileswap_preview_map.reset();
    m_heightmapRenderer.ClearPreviewMap();
    if (m_initialized) {
        m_mapRenderer.LoadRoom(m_current_room);
        RefreshObjectPlacementsFromHeightmap();
    }
}

void MyGLCanvas::NudgeSelectedObject(float dx, float dy, float dz) {
    if (m_selected_door_idx >= 0) {
        ClearTileSwapPreview();
        auto rd = m_gd ? m_gd->GetRoomData() : nullptr;
        if (!rd) {
            return;
        }
        auto doors = rd->GetDoors(m_current_room);
        if (m_selected_door_idx < 0 || m_selected_door_idx >= static_cast<int>(doors.size())) {
            m_selected_door_idx = -1;
            m_hovered_door_idx = -1;
            return;
        }
        Door& door = doors[static_cast<std::size_t>(m_selected_door_idx)];
        door.x = static_cast<uint8_t>(std::clamp(static_cast<int>(door.x) + static_cast<int>(dx), 0, 63));
        door.y = static_cast<uint8_t>(std::clamp(static_cast<int>(door.y) + static_cast<int>(dy), 0, 63));
        rd->SetDoors(m_current_room, doors);
        m_hovered_door_idx = m_selected_door_idx;
        return;
    }

    if (m_selected_tileswap_region_idx >= 0) {
        ClearTileSwapPreview();
        auto regions = BuildTileSwapRegionGeometries(m_gd, m_current_room, m_mapRenderer, m_heightmapRenderer.GetZExtent());
        if (m_selected_tileswap_region_idx >= static_cast<int>(regions.size())) {
            m_selected_tileswap_region_idx = -1;
            return;
        }
        const auto& region = regions[static_cast<std::size_t>(m_selected_tileswap_region_idx)];
        auto rd = m_gd ? m_gd->GetRoomData() : nullptr;
        if (!rd) {
            return;
        }
        auto swaps = rd->GetTileSwaps(m_current_room);
        if (region.swap_index < 0 || region.swap_index >= static_cast<int>(swaps.size())) {
            return;
        }
        TileSwap& swap = swaps[static_cast<std::size_t>(region.swap_index)];
        TileSwapRegionMetrics metrics = MetricsForTileSwapRegion(swap, region.part);
        int x = std::clamp(metrics.x + static_cast<int>(dx), 0, std::max(0, 64 - metrics.width));
        int y = std::clamp(metrics.y + static_cast<int>(dy), 0, std::max(0, 64 - metrics.height));
        switch (region.part) {
            case TileSwapRegionPart::TilemapSource:
                swap.map.src_x = static_cast<uint8_t>(x);
                swap.map.src_y = static_cast<uint8_t>(y);
                break;
            case TileSwapRegionPart::TilemapDestination:
                swap.map.dst_x = static_cast<uint8_t>(x);
                swap.map.dst_y = static_cast<uint8_t>(y);
                break;
            case TileSwapRegionPart::HeightmapSource:
                swap.heightmap.src_x = static_cast<uint8_t>(x);
                swap.heightmap.src_y = static_cast<uint8_t>(y);
                break;
            case TileSwapRegionPart::HeightmapDestination:
                swap.heightmap.dst_x = static_cast<uint8_t>(x);
                swap.heightmap.dst_y = static_cast<uint8_t>(y);
                break;
        }
        rd->SetTileSwaps(m_current_room, swaps);
        return;
    }

    if (m_selected_entity_idx >= 0 && m_selected_entity_idx < static_cast<int>(m_instances.size())) {
        uint32_t selected_id = m_instances[static_cast<std::size_t>(m_selected_entity_idx)].instance_id;
        SpriteInstance& inst = m_instances[static_cast<std::size_t>(m_selected_entity_idx)];
        bool was_on_floor = std::abs(inst.map_z - inst.floor_z) <= 0.01f;

        inst.map_x = std::clamp(inst.map_x + dx * 0.5f, 0.0f, 63.5f);
        inst.map_y = std::clamp(inst.map_y + dy * 0.5f, 0.0f, 63.5f);
        inst.map_z = std::clamp(inst.map_z + dz, 0.0f, 15.5f);
        inst.floor_z = FloorUnderHitbox(
            inst.map_x + inst.hitbox_offset,
            inst.map_y + inst.hitbox_offset,
            inst.hitbox_base * 0.5f);
        if (dz == 0.0f && was_on_floor) {
            inst.map_z = std::clamp(inst.floor_z, 0.0f, 15.5f);
        }

        UpdateEntityProjection(inst);
        SortEntitiesGeometrically(m_instances);
        m_selected_entity_idx = FindInstanceIndex(selected_id);
        m_hovered_entity_idx = m_selected_entity_idx;
        m_selected_warp_idx = -1;
        m_hovered_warp_idx = -1;
        return;
    }

    if (m_selected_warp_idx >= 0 && m_selected_warp_idx < static_cast<int>(m_warps.size())) {
        WarpInstance& warp = m_warps[static_cast<std::size_t>(m_selected_warp_idx)];
        warp.x = std::clamp(warp.x + dx, 0.0f, 63.5f);
        warp.y = std::clamp(warp.y + dy, 0.0f, 63.5f);
        UpdateWarpFloor(warp);
        m_selected_entity_idx = -1;
        m_hovered_entity_idx = -1;
    }
}

void MyGLCanvas::RenderWarps() {
    glUseProgram(0);
    for (int i = 0; i <= 5; ++i) {
        glActiveTexture(GL_TEXTURE0 + i);
        glDisable(GL_TEXTURE_2D);
    }
    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    for (std::size_t i = 0; i < m_warps.size(); ++i) {
        const WarpInstance& warp = m_warps[i];
        auto quad = WarpQuad(warp);
        if (static_cast<int>(i) == m_selected_warp_idx) {
            auto selected_quad = WarpQuad(warp, -0.08f);
            glLineWidth(3.0f);
            glColor4f(1.0f, 0.0f, 0.0f, 0.95f);
            glBegin(GL_LINE_LOOP);
            for (const auto& point : selected_quad) {
                glVertex2f(point.x, point.y);
            }
            glEnd();
        }

        if (warp.DestinationRoom() == 0xFFFF) {
            glColor4f(1.0f, 0.05f, 0.05f, 0.95f);
        } else {
            switch (warp.warp.type) {
                case Landstalker::WarpList::Warp::Type::STAIR_SE:
                    glColor4f(0.0f, 0.95f, 1.0f, 0.95f);
                    break;
                case Landstalker::WarpList::Warp::Type::STAIR_SW:
                    glColor4f(0.1f, 1.0f, 0.1f, 0.95f);
                    break;
                case Landstalker::WarpList::Warp::Type::NORMAL:
                default:
                    glColor4f(1.0f, 0.95f, 0.0f, 0.95f);
                    break;
            }
        }

        glLineWidth(1.0f);
        glBegin(GL_LINE_LOOP);
        for (const auto& point : quad) {
            glVertex2f(point.x, point.y);
        }
        glEnd();

        if (static_cast<int>(i) == m_selected_warp_idx) {
            for (int axis = 1; axis <= 2; ++axis) {
                PickRect rect = WarpResizeControlRect(warp, axis);
                bool usable = WarpResizeAxisUsable(warp, axis);
                glColor4f(0.02f, 0.02f, 0.02f, 0.6f);
                glBegin(GL_QUADS);
                glVertex2f(rect.min_x, rect.min_y);
                glVertex2f(rect.max_x, rect.min_y);
                glVertex2f(rect.max_x, rect.max_y);
                glVertex2f(rect.min_x, rect.max_y);
                glEnd();

                if (!usable) {
                    glColor4f(0.8f, 0.1f, 0.1f, 0.75f);
                } else if (axis == 1) {
                    glColor4f(1.0f, 0.95f, 0.1f, 0.95f);
                } else {
                    glColor4f(0.0f, 0.95f, 1.0f, 0.95f);
                }
                glLineWidth(2.0f);
                glBegin(GL_LINE_LOOP);
                glVertex2f(rect.min_x, rect.min_y);
                glVertex2f(rect.max_x, rect.min_y);
                glVertex2f(rect.max_x, rect.max_y);
                glVertex2f(rect.min_x, rect.max_y);
                glEnd();
                if (!usable) {
                    glBegin(GL_LINES);
                    glVertex2f(rect.min_x + 2.0f, rect.min_y + 2.0f);
                    glVertex2f(rect.max_x - 2.0f, rect.max_y - 2.0f);
                    glVertex2f(rect.max_x - 2.0f, rect.min_y + 2.0f);
                    glVertex2f(rect.min_x + 2.0f, rect.max_y - 2.0f);
                    glEnd();
                }
                glLineWidth(1.0f);
            }
        }
    }

    glLineWidth(1.0f);
}

void MyGLCanvas::RenderEntityControls() {
    auto draw_control = [this](int entity_idx, bool selected) {
        if (entity_idx < 0 || entity_idx >= static_cast<int>(m_instances.size())) {
            return;
        }

        const SpriteInstance& inst = m_instances[static_cast<std::size_t>(entity_idx)];
        PickRect rect = EntityZControlRect(inst);
        float center_x = inst.map_x + inst.hitbox_offset;
        float center_y = inst.map_y + inst.hitbox_offset;
        float top_z = inst.map_z + std::max(inst.hitbox_height, 0.125f);
        PickPoint top_center = ProjectEntityGridPoint(inst, center_x, center_y, top_z);
        PickPoint handle_center{
            (rect.min_x + rect.max_x) * 0.5f,
            (rect.min_y + rect.max_y) * 0.5f
        };

        glColor4f(selected ? 1.0f : 0.85f, selected ? 0.2f : 0.9f, selected ? 0.2f : 1.0f, 0.95f);
        glLineWidth(1.0f);
        glBegin(GL_LINES);
        glVertex2f(top_center.x, top_center.y);
        glVertex2f(handle_center.x, handle_center.y);
        glEnd();

        glColor4f(0.02f, 0.02f, 0.02f, 0.55f);
        glBegin(GL_QUADS);
        glVertex2f(rect.min_x, rect.min_y);
        glVertex2f(rect.max_x, rect.min_y);
        glVertex2f(rect.max_x, rect.max_y);
        glVertex2f(rect.min_x, rect.max_y);
        glEnd();

        glColor4f(selected ? 1.0f : 0.85f, selected ? 0.2f : 0.9f, selected ? 0.2f : 1.0f, 0.95f);
        glLineWidth(2.0f);
        glBegin(GL_LINE_LOOP);
        glVertex2f(rect.min_x, rect.min_y);
        glVertex2f(rect.max_x, rect.min_y);
        glVertex2f(rect.max_x, rect.max_y);
        glVertex2f(rect.min_x, rect.max_y);
        glEnd();
        glLineWidth(1.0f);
    };

    glUseProgram(0);
    for (int i = 0; i <= 5; ++i) {
        glActiveTexture(GL_TEXTURE0 + i);
        glDisable(GL_TEXTURE_2D);
    }
    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    draw_control(m_selected_entity_idx, true);
}

void MyGLCanvas::RenderSelectedEntityTooltip() {
    if (m_selected_entity_idx < 0 || m_selected_entity_idx >= static_cast<int>(m_instances.size())) {
        return;
    }

    const SpriteInstance& inst = m_instances[static_cast<std::size_t>(m_selected_entity_idx)];
    float center_x = inst.map_x + inst.hitbox_offset;
    float center_y = inst.map_y + inst.hitbox_offset;
    float top_z = inst.map_z + std::max(inst.hitbox_height, 0.125f);
    PickPoint anchor = ProjectEntityGridPoint(inst, center_x, center_y, top_z);

    auto fmt = [](float value) {
        std::ostringstream out;
        out << std::fixed << std::setprecision(1) << value;
        return out.str();
    };

    std::array<std::string, 5> lines = {
        std::string{"I:"} + std::to_string(std::clamp<int>(static_cast<int>(inst.instance_id), 1, 15)),
        std::string{"EID:"} + HexByte(inst.entity_id),
        std::string{"X:"} + fmt(inst.map_x),
        std::string{"Y:"} + fmt(inst.map_y),
        std::string{"Z:"} + fmt(inst.map_z)
    };

    float zoom = std::max(ZoomFactor(), 0.0001f);
    float scale = 1.0f;
    float line_height = 10.0f;
    float char_width = 6.0f * scale;
    float text_w = 0.0f;
    for (const auto& line : lines) {
        text_w = std::max(text_w, float(line.size()) * char_width);
    }
    float text_h = float(lines.size()) * line_height;
    float x = anchor.x * zoom + m_cam_x + 14.0f;
    float y = anchor.y * zoom + m_cam_y - 44.0f;

    int w = 0;
    int h = 0;
    GetClientSize(&w, &h);

    glUseProgram(0);
    for (int i = 0; i <= 5; ++i) {
        glActiveTexture(GL_TEXTURE0 + i);
        glDisable(GL_TEXTURE_2D);
    }
    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, w, h, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glColor4f(0.02f, 0.02f, 0.025f, 0.72f);
    glBegin(GL_QUADS);
    glVertex2f(x - 5.0f, y - 5.0f);
    glVertex2f(x + text_w + 5.0f, y - 5.0f);
    glVertex2f(x + text_w + 5.0f, y + text_h + 2.0f);
    glVertex2f(x - 5.0f, y + text_h + 2.0f);
    glEnd();

    glLineWidth(1.0f);
    glColor4f(0.9f, 0.9f, 0.88f, 0.95f);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        DrawOverlayText(lines[i], x, y + float(i) * line_height, scale);
    }
}

void MyGLCanvas::RenderSelectedWarpTooltip() {
    if (m_selected_warp_idx < 0 || m_selected_warp_idx >= static_cast<int>(m_warps.size())) {
        return;
    }

    const WarpInstance& warp = m_warps[static_cast<std::size_t>(m_selected_warp_idx)];
    PickPoint anchor = ProjectWarpGridPoint(warp, warp.x + warp.width, warp.y + warp.height, warp.floor_z);

    auto fmt = [](float value) {
        std::ostringstream out;
        out << std::fixed << std::setprecision(0) << value;
        return out.str();
    };

    std::array<std::string, 4> lines = {
        std::string{"ID:"} + std::to_string(warp.instance_id),
        std::string{"X:"} + fmt(warp.x),
        std::string{"Y:"} + fmt(warp.y),
        std::string{"D:"} + HexWord(warp.DestinationRoom())
    };

    float zoom = std::max(ZoomFactor(), 0.0001f);
    float scale = 1.0f;
    float line_height = 10.0f;
    float char_width = 6.0f * scale;
    float text_w = 0.0f;
    for (const auto& line : lines) {
        text_w = std::max(text_w, float(line.size()) * char_width);
    }
    float text_h = float(lines.size()) * line_height;
    float x = anchor.x * zoom + m_cam_x + 48.0f;
    float y = anchor.y * zoom + m_cam_y - 34.0f;

    int w = 0;
    int h = 0;
    GetClientSize(&w, &h);

    glUseProgram(0);
    for (int i = 0; i <= 5; ++i) {
        glActiveTexture(GL_TEXTURE0 + i);
        glDisable(GL_TEXTURE_2D);
    }
    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, w, h, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glColor4f(0.02f, 0.02f, 0.025f, 0.72f);
    glBegin(GL_QUADS);
    glVertex2f(x - 5.0f, y - 5.0f);
    glVertex2f(x + text_w + 5.0f, y - 5.0f);
    glVertex2f(x + text_w + 5.0f, y + text_h + 2.0f);
    glVertex2f(x - 5.0f, y + text_h + 2.0f);
    glEnd();

    if (warp.DestinationRoom() == 0xFFFF) {
        glColor4f(1.0f, 0.2f, 0.2f, 0.95f);
    } else {
        glColor4f(0.9f, 0.9f, 0.88f, 0.95f);
    }
    glLineWidth(1.0f);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        DrawOverlayText(lines[i], x, y + float(i) * line_height, scale);
    }
}

void MyGLCanvas::RenderSelectedDoorTooltip() {
    auto rd = m_gd ? m_gd->GetRoomData() : nullptr;
    if (!rd || m_selected_door_idx < 0) {
        return;
    }

    auto doors = rd->GetDoors(m_current_room);
    if (m_selected_door_idx >= static_cast<int>(doors.size())) {
        return;
    }

    auto geometries = BuildDoorGeometries(
        m_gd,
        m_current_room,
        m_mapRenderer,
        m_heightmapRenderer.GetZExtent(),
        m_tileswap_preview_map);

    PickPoint anchor{0.0f, 0.0f};
    for (const auto& geom : geometries) {
        if (geom.index == m_selected_door_idx) {
            anchor = geom.bounds.max_x != geom.bounds.min_x || geom.bounds.max_y != geom.bounds.min_y
                ? PickPoint{geom.bounds.max_x, geom.bounds.min_y}
                : (geom.cell_points.empty() ? PickPoint{0.0f, 0.0f} : geom.cell_points.front());
            break;
        }
    }

    const Door& door = doors[static_cast<std::size_t>(m_selected_door_idx)];
    const char* type_label = "Invalid";
    auto map_entry = rd->GetMapForRoom(m_current_room);
    if (map_entry) {
        auto map = map_entry->GetData();
        int x = static_cast<int>(door.x);
        int y = static_cast<int>(door.y);
        if (x >= 0 && y >= 0 && x < map->GetHeightmapWidth() && y < map->GetHeightmapHeight()) {
            auto floor_type = static_cast<Tilemap3D::FloorType>(map->GetCellType({x, y}));
            if (floor_type == Tilemap3D::FloorType::DOOR_NW) {
                type_label = "NW";
            } else if (floor_type == Tilemap3D::FloorType::DOOR_NE) {
                type_label = "NE";
            }
        }
    }

    std::array<std::string, 4> lines = {
        std::string{"ID:"} + HexByte(static_cast<uint8_t>(std::clamp(m_selected_door_idx + 1, 0, 255))),
        std::string{"X:"} + std::to_string(static_cast<int>(door.x)),
        std::string{"Y:"} + std::to_string(static_cast<int>(door.y)),
        std::string{"T:"} + type_label
    };

    float zoom = std::max(ZoomFactor(), 0.0001f);
    float scale = 1.0f;
    float line_height = 10.0f;
    float char_width = 6.0f * scale;
    float text_w = 0.0f;
    for (const auto& line : lines) {
        text_w = std::max(text_w, float(line.size()) * char_width);
    }
    float text_h = float(lines.size()) * line_height;
    float x = anchor.x * zoom + m_cam_x + 12.0f;
    float y = anchor.y * zoom + m_cam_y - 20.0f;

    int w = 0;
    int h = 0;
    GetClientSize(&w, &h);

    glUseProgram(0);
    for (int i = 0; i <= 5; ++i) {
        glActiveTexture(GL_TEXTURE0 + i);
        glDisable(GL_TEXTURE_2D);
    }
    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, w, h, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glColor4f(0.02f, 0.02f, 0.025f, 0.72f);
    glBegin(GL_QUADS);
    glVertex2f(x - 5.0f, y - 5.0f);
    glVertex2f(x + text_w + 5.0f, y - 5.0f);
    glVertex2f(x + text_w + 5.0f, y + text_h + 2.0f);
    glVertex2f(x - 5.0f, y + text_h + 2.0f);
    glEnd();

    glLineWidth(1.0f);
    glColor4f(0.9f, 0.9f, 0.88f, 0.95f);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        DrawOverlayText(lines[i], x, y + float(i) * line_height, scale);
    }
}

void MyGLCanvas::RenderSelectedTileSwapRegionTooltip() {
    auto regions = BuildTileSwapRegionGeometries(m_gd, m_current_room, m_mapRenderer, m_heightmapRenderer.GetZExtent());
    if (m_selected_tileswap_region_idx < 0 ||
        m_selected_tileswap_region_idx >= static_cast<int>(regions.size())) {
        return;
    }

    const auto& region = regions[static_cast<std::size_t>(m_selected_tileswap_region_idx)];
    const TileSwap& swap = region.swap;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    switch (region.part) {
        case TileSwapRegionPart::TilemapSource:
            x = swap.map.src_x;
            y = swap.map.src_y;
            width = swap.map.width;
            height = swap.map.height;
            break;
        case TileSwapRegionPart::TilemapDestination:
            x = swap.map.dst_x;
            y = swap.map.dst_y;
            width = swap.map.width;
            height = swap.map.height;
            break;
        case TileSwapRegionPart::HeightmapSource:
            x = swap.heightmap.src_x;
            y = swap.heightmap.src_y;
            width = swap.heightmap.width;
            height = swap.heightmap.height;
            break;
        case TileSwapRegionPart::HeightmapDestination:
            x = swap.heightmap.dst_x;
            y = swap.heightmap.dst_y;
            width = swap.heightmap.width;
            height = swap.heightmap.height;
            break;
    }

    auto two_digits = [](int value) {
        std::ostringstream out;
        out << std::setw(2) << std::setfill('0') << std::clamp(value, 0, 99);
        return out.str();
    };

    std::array<std::string, 5> lines = {
        std::string{"ID:"} + two_digits(static_cast<int>(swap.trigger)),
        std::string{TileSwapPartLabel(region.part)},
        std::string{"SH:"} + TileSwapShapeLabel(swap.mode),
        std::string{"SZ:"} + two_digits(width) + "X" + two_digits(height),
        std::string{"XY:"} + two_digits(x) + "." + two_digits(y)
    };

    constexpr float scale = 1.0f;
    constexpr float line_height = 10.0f;
    constexpr float char_width = 6.0f * scale;
    float text_w = 0.0f;
    for (const auto& line : lines) {
        text_w = std::max(text_w, float(line.size()) * char_width);
    }
    float text_h = float(lines.size()) * line_height;
    float zoom = std::max(ZoomFactor(), 0.0001f);
    float tooltip_x = region.bounds.max_x * zoom + m_cam_x + 10.0f;
    float tooltip_y = region.bounds.min_y * zoom + m_cam_y - 4.0f;

    int w = 0;
    int h = 0;
    GetClientSize(&w, &h);

    glUseProgram(0);
    for (int i = 0; i <= 5; ++i) {
        glActiveTexture(GL_TEXTURE0 + i);
        glDisable(GL_TEXTURE_2D);
    }
    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, w, h, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glColor4f(0.02f, 0.02f, 0.025f, 0.72f);
    glBegin(GL_QUADS);
    glVertex2f(tooltip_x - 5.0f, tooltip_y - 5.0f);
    glVertex2f(tooltip_x + text_w + 5.0f, tooltip_y - 5.0f);
    glVertex2f(tooltip_x + text_w + 5.0f, tooltip_y + text_h + 2.0f);
    glVertex2f(tooltip_x - 5.0f, tooltip_y + text_h + 2.0f);
    glEnd();

    glLineWidth(1.0f);
    glColor4f(0.9f, 0.9f, 0.88f, 0.95f);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        DrawOverlayText(lines[i], tooltip_x, tooltip_y + float(i) * line_height, scale);
    }
}

void MyGLCanvas::RenderRoomInfoTable(int width, int height) {
    auto rows = BuildRoomInfoRows(m_gd, m_current_room);
    m_room_info_links.clear();
    if (rows.empty()) {
        return;
    }

    glUseProgram(0);
    for (int i = 0; i <= 5; ++i) {
        glActiveTexture(GL_TEXTURE0 + i);
        glDisable(GL_TEXTURE_2D);
    }
    glActiveTexture(GL_TEXTURE0);
    glDisable(GL_STENCIL_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, width, height, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    constexpr float scale = 1.0f;
    constexpr float glyph_advance = 6.0f;
    constexpr float row_height = 10.0f;
    constexpr float padding = 6.0f;
    constexpr float column_gap = 12.0f;
    constexpr float start_x = 8.0f;
    constexpr float start_y = 8.0f;

    float label_width = 0.0f;
    float value_width = 0.0f;
    for (const auto& row : rows) {
        label_width = std::max(label_width, float(row.label.size()) * glyph_advance * scale);
        value_width = std::max(value_width, float(row.value.size()) * glyph_advance * scale);
    }

    float table_width = padding * 2.0f + label_width + column_gap + value_width;
    float table_height = padding * 2.0f + float(rows.size()) * row_height;
    float label_x = start_x + padding;
    float value_x = label_x + label_width + column_gap;
    int hovered_room = HitTestRoomInfoLink(m_last_mouse_pos);

    glColor4f(0.02f, 0.02f, 0.03f, 0.78f);
    glBegin(GL_QUADS);
    glVertex2f(start_x, start_y);
    glVertex2f(start_x + table_width, start_y);
    glVertex2f(start_x + table_width, start_y + table_height);
    glVertex2f(start_x, start_y + table_height);
    glEnd();

    glColor4f(0.45f, 0.45f, 0.5f, 0.85f);
    glLineWidth(1.0f);
    glBegin(GL_LINE_LOOP);
    glVertex2f(start_x, start_y);
    glVertex2f(start_x + table_width, start_y);
    glVertex2f(start_x + table_width, start_y + table_height);
    glVertex2f(start_x, start_y + table_height);
    glEnd();

    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& row = rows[i];
        float row_y = start_y + padding + float(i) * row_height;
        float value_text_width = float(row.value.size()) * glyph_advance * scale;
        wxRect value_rect(
            static_cast<int>(std::floor(value_x)),
            static_cast<int>(std::floor(row_y)),
            static_cast<int>(std::ceil(value_text_width)),
            static_cast<int>(std::ceil(row_height)));

        if (row.room == hovered_room) {
            glColor4f(1.0f, 1.0f, 1.0f, 0.09f);
            glBegin(GL_QUADS);
            glVertex2f(float(value_rect.GetLeft()), float(value_rect.GetTop()));
            glVertex2f(float(value_rect.GetRight()), float(value_rect.GetTop()));
            glVertex2f(float(value_rect.GetRight()), float(value_rect.GetBottom()));
            glVertex2f(float(value_rect.GetLeft()), float(value_rect.GetBottom()));
            glEnd();
        }

        glColor4f(1.0f, 1.0f, 1.0f, 0.95f);
        DrawOverlayText(row.label, label_x, row_y, scale);
        glColor4f(1.0f, 0.95f, 0.18f, 0.98f);
        DrawOverlayText(row.value, value_x, row_y, scale);

        m_room_info_links.push_back({value_rect, row.room});
    }
}

void MyGLCanvas::WriteSelectedEntityOcclusionDebugLog() {
    if (m_selected_entity_idx < 0 || m_selected_entity_idx >= static_cast<int>(m_instances.size())) {
        wxLogMessage("Select an entity before writing occlusion debug logs.");
        return;
    }

    const SpriteInstance& inst = m_instances[static_cast<std::size_t>(m_selected_entity_idx)];
    auto depth_range = EntityOcclusionDebugDepthRange(inst);
    float center_x = inst.map_x + inst.hitbox_offset;
    float center_y = inst.map_y + inst.hitbox_offset;
    float half_base = inst.hitbox_base * 0.5f;
    constexpr const char* path = "/tmp/landstalker_occlusion_debug.log";
    m_heightmapRenderer.WriteEntityOcclusionDebugLog(
        path,
        depth_range.first,
        depth_range.second,
        inst.map_z,
        center_x - half_base,
        center_y - half_base,
        center_x + half_base,
        center_y + half_base,
        inst.map_z + std::max(inst.hitbox_height, 0.125f));

    std::array<PickPoint, 8> projected_bounds = {
        ProjectEntityGridPoint(inst, center_x - half_base, center_y - half_base, inst.map_z),
        ProjectEntityGridPoint(inst, center_x + half_base, center_y - half_base, inst.map_z),
        ProjectEntityGridPoint(inst, center_x + half_base, center_y + half_base, inst.map_z),
        ProjectEntityGridPoint(inst, center_x - half_base, center_y + half_base, inst.map_z),
        ProjectEntityGridPoint(inst, center_x - half_base, center_y - half_base, inst.map_z + std::max(inst.hitbox_height, 0.125f)),
        ProjectEntityGridPoint(inst, center_x + half_base, center_y - half_base, inst.map_z + std::max(inst.hitbox_height, 0.125f)),
        ProjectEntityGridPoint(inst, center_x + half_base, center_y + half_base, inst.map_z + std::max(inst.hitbox_height, 0.125f)),
        ProjectEntityGridPoint(inst, center_x - half_base, center_y + half_base, inst.map_z + std::max(inst.hitbox_height, 0.125f))
    };
    float min_x = projected_bounds.front().x;
    float min_y = projected_bounds.front().y;
    float max_x = projected_bounds.front().x;
    float max_y = projected_bounds.front().y;
    for (const auto& point : projected_bounds) {
        min_x = std::min(min_x, point.x);
        min_y = std::min(min_y, point.y);
        max_x = std::max(max_x, point.x);
        max_y = std::max(max_y, point.y);
    }

    constexpr const char* fg_path = "/tmp/landstalker_foreground_priority_debug.log";
    m_mapRenderer.WriteForegroundPriorityDebugLog(fg_path, min_x, min_y, max_x, max_y);
    wxLogMessage("Wrote occlusion debug logs to %s and %s", path, fg_path);
}

void MyGLCanvas::WriteEntityDrawOrderDebugLog() {
    uint32_t selected_entity_id = 0;
    uint32_t hovered_entity_id = 0;
    if (m_selected_entity_idx >= 0 && m_selected_entity_idx < static_cast<int>(m_instances.size())) {
        selected_entity_id = m_instances[static_cast<std::size_t>(m_selected_entity_idx)].instance_id;
    }
    if (m_hovered_entity_idx >= 0 && m_hovered_entity_idx < static_cast<int>(m_instances.size())) {
        hovered_entity_id = m_instances[static_cast<std::size_t>(m_hovered_entity_idx)].instance_id;
    }

    SortEntitiesGeometrically(m_instances);
    m_selected_entity_idx = selected_entity_id != 0 ? FindInstanceIndex(selected_entity_id) : -1;
    m_hovered_entity_idx = hovered_entity_id != 0 ? FindInstanceIndex(hovered_entity_id) : -1;

    constexpr const char* path = "/tmp/landstalker_entity_draw_order.log";
    std::ofstream log(path, std::ios::out | std::ios::trunc);
    if (!log.is_open()) {
        wxLogMessage("Unable to write entity draw-order log to %s", path);
        return;
    }

    log << std::fixed << std::setprecision(3);
    log << "room," << m_current_room << "\n";
    log << "selected_index," << m_selected_entity_idx << "\n";
    log << "hovered_index," << m_hovered_entity_idx << "\n";
    log << "entity_count," << m_instances.size() << "\n";
    log << "note,rows are in actual render order; later rows draw over earlier rows\n\n";
    log << "render_order,index,instance_id,entity_id,selected,hovered,map_x,map_y,map_z,floor_z,depth_key,hitbox_base,hitbox_height,hitbox_offset,min_x,min_y,max_x,max_y,min_z,max_z,back_depth,front_depth,screen_x,screen_y,depth_back,depth_front\n";

    for (std::size_t i = 0; i < m_instances.size(); ++i) {
        const SpriteInstance& inst = m_instances[i];
        auto depth_range = EntityOcclusionDebugDepthRange(inst);
        EntityBounds bounds = GetEntityBounds(inst);
        float depth_key = EntityFrontDepthKey(inst);
        log << i << ","
            << i << ","
            << inst.instance_id << ","
            << int(inst.entity_id) << ","
            << (static_cast<int>(i) == m_selected_entity_idx ? 1 : 0) << ","
            << (static_cast<int>(i) == m_hovered_entity_idx ? 1 : 0) << ","
            << inst.map_x << ","
            << inst.map_y << ","
            << inst.map_z << ","
            << inst.floor_z << ","
            << depth_key << ","
            << inst.hitbox_base << ","
            << inst.hitbox_height << ","
            << inst.hitbox_offset << ","
            << bounds.min_x << ","
            << bounds.min_y << ","
            << bounds.max_x << ","
            << bounds.max_y << ","
            << bounds.min_z << ","
            << bounds.max_z << ","
            << bounds.back_depth << ","
            << bounds.front_depth << ","
            << inst.x << ","
            << inst.y << ","
            << depth_range.first << ","
            << depth_range.second
            << "\n";
    }

    log << "\n";
    log << "pair_a,pair_b,a_before_b,b_before_a,a_instance,b_instance,a_entity,b_entity,a_front_depth,b_front_depth,footprints_overlap,z_ranges_overlap,a_min_x,a_min_y,a_max_x,a_max_y,a_min_z,a_max_z,b_min_x,b_min_y,b_max_x,b_max_y,b_min_z,b_max_z\n";
    for (std::size_t a = 0; a < m_instances.size(); ++a) {
        for (std::size_t b = a + 1; b < m_instances.size(); ++b) {
            const SpriteInstance& lhs = m_instances[a];
            const SpriteInstance& rhs = m_instances[b];
            EntityBounds lhs_bounds = GetEntityBounds(lhs);
            EntityBounds rhs_bounds = GetEntityBounds(rhs);
            bool footprints_overlap = lhs_bounds.min_x < rhs_bounds.max_x - 0.001f &&
                                      lhs_bounds.max_x > rhs_bounds.min_x + 0.001f &&
                                      lhs_bounds.min_y < rhs_bounds.max_y - 0.001f &&
                                      lhs_bounds.max_y > rhs_bounds.min_y + 0.001f;
            bool z_ranges_overlap = lhs_bounds.min_z < rhs_bounds.max_z - 0.001f &&
                                    lhs_bounds.max_z > rhs_bounds.min_z + 0.001f;
            log << a << ","
                << b << ","
                << (EntityMustDrawBefore(lhs, rhs) ? 1 : 0) << ","
                << (EntityMustDrawBefore(rhs, lhs) ? 1 : 0) << ","
                << lhs.instance_id << ","
                << rhs.instance_id << ","
                << int(lhs.entity_id) << ","
                << int(rhs.entity_id) << ","
                << lhs_bounds.front_depth << ","
                << rhs_bounds.front_depth << ","
                << (footprints_overlap ? 1 : 0) << ","
                << (z_ranges_overlap ? 1 : 0) << ","
                << lhs_bounds.min_x << ","
                << lhs_bounds.min_y << ","
                << lhs_bounds.max_x << ","
                << lhs_bounds.max_y << ","
                << lhs_bounds.min_z << ","
                << lhs_bounds.max_z << ","
                << rhs_bounds.min_x << ","
                << rhs_bounds.min_y << ","
                << rhs_bounds.max_x << ","
                << rhs_bounds.max_y << ","
                << rhs_bounds.min_z << ","
                << rhs_bounds.max_z
                << "\n";
        }
    }

    wxLogMessage("Wrote entity draw-order log to %s", path);
}

float MyGLCanvas::FloorUnderRect(float min_x, float min_y, float max_x, float max_y) const {
    auto map_entry = m_gd->GetRoomData()->GetMapForRoom(m_current_room);
    if (!map_entry) {
        return 0.0f;
    }

    auto map = m_tileswap_preview_map ? m_tileswap_preview_map : map_entry->GetData();
    constexpr float heightmap_entity_offset = 12.0f;
    float hm_min_x = min_x - heightmap_entity_offset;
    float hm_min_y = min_y - heightmap_entity_offset;
    float hm_max_x = max_x - heightmap_entity_offset;
    float hm_max_y = max_y - heightmap_entity_offset;
    int min_cell_x = static_cast<int>(std::floor(hm_min_x));
    int min_cell_y = static_cast<int>(std::floor(hm_min_y));
    int max_cell_x = static_cast<int>(std::floor(std::nextafter(hm_max_x, hm_min_x)));
    int max_cell_y = static_cast<int>(std::floor(std::nextafter(hm_max_y, hm_min_y)));
    uint8_t floor_z = 0;

    for (int y = min_cell_y; y <= max_cell_y; ++y) {
        for (int x = min_cell_x; x <= max_cell_x; ++x) {
            if (x < 0 || y < 0 || x >= map->GetHeightmapWidth() || y >= map->GetHeightmapHeight()) {
                continue;
            }
            uint8_t height = map->GetHeight({x, y});
            if (height != 0xFF) {
                floor_z = std::max(floor_z, height);
            }
        }
    }

    return float(floor_z);
}

float MyGLCanvas::FloorUnderPoint(float x, float y) const {
    auto map_entry = m_gd->GetRoomData()->GetMapForRoom(m_current_room);
    if (!map_entry) {
        return 0.0f;
    }

    auto map = m_tileswap_preview_map ? m_tileswap_preview_map : map_entry->GetData();
    constexpr float heightmap_entity_offset = 12.0f;
    int cell_x = static_cast<int>(std::floor(x - heightmap_entity_offset));
    int cell_y = static_cast<int>(std::floor(y - heightmap_entity_offset));
    if (cell_x < 0 || cell_y < 0 || cell_x >= map->GetHeightmapWidth() || cell_y >= map->GetHeightmapHeight()) {
        return 0.0f;
    }

    uint8_t height = map->GetHeight({cell_x, cell_y});
    return height == 0xFF ? 0.0f : float(height);
}

bool MyGLCanvas::ShadowOccludedByHeightmap(float min_x, float min_y, float max_x, float max_y, float z) const {
    auto map_entry = m_gd->GetRoomData()->GetMapForRoom(m_current_room);
    if (!map_entry) {
        return false;
    }

    auto map = map_entry->GetData();
    constexpr float heightmap_entity_offset = 12.0f;
    float hm_min_x = min_x - heightmap_entity_offset;
    float hm_min_y = min_y - heightmap_entity_offset;
    float hm_max_x = max_x - heightmap_entity_offset;
    float hm_max_y = max_y - heightmap_entity_offset;
    float shadow_back_depth = hm_min_x + hm_min_y;
    float shadow_front_depth = hm_max_x + hm_max_y;

    auto visible_cell = [&](int x, int y, uint8_t height, uint8_t restriction) {
        return height != 0xFF && !(restriction == 4 && height == 0) && height > z;
    };
    auto overlaps_shadow = [&](int x, int y) {
        return float(x) < hm_max_x &&
               float(x + 1) > hm_min_x &&
               float(y) < hm_max_y &&
               float(y + 1) > hm_min_y;
    };
    auto cell_is_in_front = [&](int x, int y) {
        return !overlaps_shadow(x, y) &&
               float(x + 1) > hm_min_x &&
               float(y + 1) > hm_min_y;
    };

    for (int y = 0; y < map->GetHeightmapHeight(); ++y) {
        for (int x = 0; x < map->GetHeightmapWidth(); ++x) {
            uint8_t height = map->GetHeight({x, y});
            uint16_t value = map->GetHeightmapCell({x, y});
            uint8_t restriction = static_cast<uint8_t>(value >> 12);
            float cell_depth = float(x + y);
            if (cell_depth < shadow_back_depth || cell_depth > shadow_front_depth + 1.0f) {
                continue;
            }
            if (visible_cell(x, y, height, restriction) && cell_is_in_front(x, y)) {
                return true;
            }
        }
    }

    return false;
}

bool MyGLCanvas::EntityCollidesWithHeightmap(const SpriteInstance& inst) const {
    auto map_entry = m_gd->GetRoomData()->GetMapForRoom(m_current_room);
    if (!map_entry || inst.hitbox_base <= 0.0f) {
        return false;
    }

    auto map = map_entry->GetData();
    constexpr float heightmap_entity_offset = 12.0f;
    constexpr float epsilon = 0.001f;
    float center_x = inst.map_x + inst.hitbox_offset;
    float center_y = inst.map_y + inst.hitbox_offset;
    float half_base = inst.hitbox_base * 0.5f;
    float hm_min_x = center_x - half_base - heightmap_entity_offset;
    float hm_min_y = center_y - half_base - heightmap_entity_offset;
    float hm_max_x = center_x + half_base - heightmap_entity_offset;
    float hm_max_y = center_y + half_base - heightmap_entity_offset;
    int min_cell_x = static_cast<int>(std::floor(hm_min_x));
    int min_cell_y = static_cast<int>(std::floor(hm_min_y));
    int max_cell_x = static_cast<int>(std::floor(std::nextafter(hm_max_x, hm_min_x)));
    int max_cell_y = static_cast<int>(std::floor(std::nextafter(hm_max_y, hm_min_y)));

    for (int y = min_cell_y; y <= max_cell_y; ++y) {
        for (int x = min_cell_x; x <= max_cell_x; ++x) {
            if (x < 0 || y < 0 || x >= map->GetHeightmapWidth() || y >= map->GetHeightmapHeight()) {
                continue;
            }
            uint8_t height = map->GetHeight({x, y});
            uint16_t value = map->GetHeightmapCell({x, y});
            uint8_t restriction = static_cast<uint8_t>(value >> 12);
            if (height == 0xFF || (restriction == 4 && height == 0)) {
                continue;
            }
            if (float(height) > inst.map_z + epsilon) {
                return true;
            }
        }
    }

    return false;
}

float MyGLCanvas::FloorUnderHitbox(float center_x, float center_y, float half_base) const {
    return FloorUnderRect(center_x - half_base, center_y - half_base, center_x + half_base, center_y + half_base);
}

int MyGLCanvas::FindInstanceIndex(uint32_t instance_id) const {
    for (std::size_t i = 0; i < m_instances.size(); ++i) {
        if (m_instances[i].instance_id == instance_id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int MyGLCanvas::FindWarpIndex(uint32_t instance_id) const {
    for (std::size_t i = 0; i < m_warps.size(); ++i) {
        if (m_warps[i].instance_id == instance_id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int MyGLCanvas::HitTestRoomInfoLink(const wxPoint& point) const {
    for (const auto& link : m_room_info_links) {
        if (link.rect.Contains(point)) {
            return static_cast<int>(link.room);
        }
    }
    return -1;
}

int MyGLCanvas::HitTestEntity(const wxPoint& point) const {
    PickPoint world_point{
        ScreenToWorldX(point.x),
        ScreenToWorldY(point.y)
    };

    for (int i = static_cast<int>(m_instances.size()) - 1; i >= 0; --i) {
        if (PointInEntityHitbox(m_instances[static_cast<std::size_t>(i)], world_point)) {
            return i;
        }
    }
    return -1;
}

int MyGLCanvas::HitTestEntityBody(const wxPoint& point) const {
    PickPoint world_point{
        ScreenToWorldX(point.x),
        ScreenToWorldY(point.y)
    };

    for (int i = static_cast<int>(m_instances.size()) - 1; i >= 0; --i) {
        if (PointInEntityHitbox(m_instances[static_cast<std::size_t>(i)], world_point, false)) {
            return i;
        }
    }
    return -1;
}

int MyGLCanvas::HitTestEntityZControl(const wxPoint& point) const {
    PickPoint world_point{
        ScreenToWorldX(point.x),
        ScreenToWorldY(point.y)
    };

    if (m_selected_entity_idx >= 0 && m_selected_entity_idx < static_cast<int>(m_instances.size()) &&
        PointInRect(world_point, EntityZControlRect(m_instances[static_cast<std::size_t>(m_selected_entity_idx)]))) {
        return m_selected_entity_idx;
    }

    return -1;
}

int MyGLCanvas::HitTestWarpResizeControl(const wxPoint& point) const {
    if (m_selected_warp_idx < 0 || m_selected_warp_idx >= static_cast<int>(m_warps.size())) {
        return 0;
    }

    PickPoint world_point{
        ScreenToWorldX(point.x),
        ScreenToWorldY(point.y)
    };
    const WarpInstance& warp = m_warps[static_cast<std::size_t>(m_selected_warp_idx)];
    for (int axis = 1; axis <= 2; ++axis) {
        if (WarpResizeAxisUsable(warp, axis) && PointInRect(world_point, WarpResizeControlRect(warp, axis))) {
            return axis;
        }
    }
    return 0;
}

int MyGLCanvas::HitTestWarp(const wxPoint& point) const {
    PickPoint world_point{
        ScreenToWorldX(point.x),
        ScreenToWorldY(point.y)
    };

    for (int i = static_cast<int>(m_warps.size()) - 1; i >= 0; --i) {
        if (PointInWarp(m_warps[static_cast<std::size_t>(i)], world_point)) {
            return i;
        }
    }
    return -1;
}

int MyGLCanvas::HitTestTileSwapRegion(const wxPoint& point) const {
    PickPoint world_point{
        ScreenToWorldX(point.x),
        ScreenToWorldY(point.y)
    };
    auto regions = BuildTileSwapRegionGeometries(m_gd, m_current_room, m_mapRenderer, m_heightmapRenderer.GetZExtent());
    for (int i = static_cast<int>(regions.size()) - 1; i >= 0; --i) {
        const auto& region = regions[static_cast<std::size_t>(i)];
        constexpr float hit_padding = 5.0f;
        if (world_point.x < region.bounds.min_x - hit_padding ||
            world_point.x > region.bounds.max_x + hit_padding ||
            world_point.y < region.bounds.min_y - hit_padding ||
            world_point.y > region.bounds.max_y + hit_padding) {
            continue;
        }
        bool hit = PointInPolygon(world_point, region.fill_points) ||
            PointInPolygonWinding(world_point, region.fill_points) ||
            (region.segments
                ? PointNearSegments(world_point, region.points, hit_padding)
                : PointNearPolyline(world_point, region.points, hit_padding));
        if (hit) {
            return region.flat_index;
        }
    }
    return -1;
}

int MyGLCanvas::HitTestTileSwapRegionResizeControl(const wxPoint& point) const {
    auto regions = BuildTileSwapRegionGeometries(m_gd, m_current_room, m_mapRenderer, m_heightmapRenderer.GetZExtent());
    if (m_selected_tileswap_region_idx < 0 ||
        m_selected_tileswap_region_idx >= static_cast<int>(regions.size())) {
        return 0;
    }
    PickPoint world_point{
        ScreenToWorldX(point.x),
        ScreenToWorldY(point.y)
    };
    const auto& region = regions[static_cast<std::size_t>(m_selected_tileswap_region_idx)];
    if (PointInRect(world_point, TileSwapRegionResizeControlRect(region))) {
        return 1;
    }
    return 0;
}

int MyGLCanvas::HitTestDoor(const wxPoint& point) const {
    PickPoint world_point{
        ScreenToWorldX(point.x),
        ScreenToWorldY(point.y)
    };
    auto doors = BuildDoorGeometries(
        m_gd,
        m_current_room,
        m_mapRenderer,
        m_heightmapRenderer.GetZExtent(),
        m_tileswap_preview_map);
    for (int i = static_cast<int>(doors.size()) - 1; i >= 0; --i) {
        const auto& door = doors[static_cast<std::size_t>(i)];
        constexpr float hit_padding = 5.0f;
        bool hit_cell = PointInPolygon(world_point, door.cell_points) ||
            PointInPolygonWinding(world_point, door.cell_points);
        bool hit_region = door.valid && !door.map_points.empty() &&
            (PointInPolygon(world_point, door.map_points) ||
             PointInPolygonWinding(world_point, door.map_points) ||
             PointNearPolyline(world_point, door.map_points, hit_padding));
        if (hit_cell || hit_region) {
            return door.index;
        }
    }
    return -1;
}

void MyGLCanvas::RenderTileSwapOutlines() {
    auto regions = BuildTileSwapRegionGeometries(m_gd, m_current_room, m_mapRenderer, m_heightmapRenderer.GetZExtent());
    if (regions.empty()) {
        return;
    }

    glUseProgram(0);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_STENCIL_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    int hover_swap = -1;
    if (m_hovered_tileswap_region_idx >= 0 && m_hovered_tileswap_region_idx < static_cast<int>(regions.size())) {
        hover_swap = regions[static_cast<std::size_t>(m_hovered_tileswap_region_idx)].swap_index;
    }

    auto colour_for_part = [](TileSwapRegionPart part) {
        switch (part) {
            case TileSwapRegionPart::TilemapSource:
                return std::array<float, 3>{0.15f, 0.45f, 1.0f};
            case TileSwapRegionPart::TilemapDestination:
                return std::array<float, 3>{1.0f, 0.15f, 0.15f};
            case TileSwapRegionPart::HeightmapSource:
                return std::array<float, 3>{0.75f, 0.25f, 1.0f};
            case TileSwapRegionPart::HeightmapDestination:
                return std::array<float, 3>{0.0f, 0.85f, 0.8f};
        }
        return std::array<float, 3>{1.0f, 1.0f, 1.0f};
    };

    for (const auto& region : regions) {
        bool selected = region.flat_index == m_selected_tileswap_region_idx;
        bool hovered_group = region.swap_index == hover_swap;
        auto colour = colour_for_part(region.part);
        float brighten = hovered_group || selected ? 1.2f : 1.0f;
        glColor4f(
            std::min(colour[0] * brighten, 1.0f),
            std::min(colour[1] * brighten, 1.0f),
            std::min(colour[2] * brighten, 1.0f),
            hovered_group || selected ? 1.0f : 0.8f);
        glLineWidth(selected ? 4.0f : (hovered_group ? 2.5f : 1.5f));
        if (m_tileswap_preview_active && region.segments) {
            DrawSegments(region.points);
        } else if (m_tileswap_preview_active) {
            DrawClosedPolyline(region.points);
        } else if (region.segments) {
            DrawDashedSegments(region.points, selected ? 10.0f : 8.0f, selected ? 3.0f : 5.0f);
        } else {
            DrawDashedClosedPolyline(region.points, selected ? 10.0f : 8.0f, selected ? 3.0f : 5.0f);
        }

        if (selected) {
            PickRect rect = TileSwapRegionResizeControlRect(region);
            glColor4f(0.02f, 0.02f, 0.025f, 0.68f);
            glBegin(GL_QUADS);
            glVertex2f(rect.min_x, rect.min_y);
            glVertex2f(rect.max_x, rect.min_y);
            glVertex2f(rect.max_x, rect.max_y);
            glVertex2f(rect.min_x, rect.max_y);
            glEnd();

            glColor4f(1.0f, 0.95f, 0.2f, 0.95f);
            glLineWidth(2.0f);
            glBegin(GL_LINE_LOOP);
            glVertex2f(rect.min_x, rect.min_y);
            glVertex2f(rect.max_x, rect.min_y);
            glVertex2f(rect.max_x, rect.max_y);
            glVertex2f(rect.min_x, rect.max_y);
            glEnd();
        }
    }

    glLineWidth(1.0f);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
}

void MyGLCanvas::RenderDoors() {
    auto doors = BuildDoorGeometries(
        m_gd,
        m_current_room,
        m_mapRenderer,
        m_heightmapRenderer.GetZExtent(),
        m_tileswap_preview_map);
    if (doors.empty()) {
        return;
    }

    glUseProgram(0);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_STENCIL_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    for (const auto& door : doors) {
        bool selected = door.index == m_selected_door_idx;
        bool hovered = door.index == m_hovered_door_idx;

        if (door.valid) {
            glColor4f(0.0f, 0.28f, 0.12f, selected || hovered ? 0.52f : 0.34f);
        } else {
            glColor4f(1.0f, 0.05f, 0.05f, selected || hovered ? 0.55f : 0.38f);
        }
        glBegin(GL_POLYGON);
        for (const auto& point : door.cell_points) {
            glVertex2f(point.x, point.y);
        }
        glEnd();

        glLineWidth(selected ? 3.5f : (hovered ? 2.5f : 1.5f));
        glColor4f(door.valid ? 0.0f : 1.0f, door.valid ? 0.45f : 0.1f, door.valid ? 0.18f : 0.1f, 0.95f);
        DrawClosedPolyline(door.cell_points);

        if (door.valid && door.map_points.size() >= 2) {
            glColor4f(1.0f, 0.62f, 1.0f, selected || hovered ? 1.0f : 0.8f);
            glLineWidth(selected ? 3.0f : (hovered ? 2.25f : 1.35f));
            DrawDashedClosedPolyline(door.map_points, selected ? 10.0f : 8.0f, selected ? 3.0f : 5.0f);
        }
    }

    glLineWidth(1.0f);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
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
    glScalef(ZoomFactor(), ZoomFactor(), 1.0f);
    glClearColor(0.1f, 0.1f, 0.15f, 1.0f); 
    glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    uint32_t selected_entity_id = 0;
    uint32_t hovered_entity_id = 0;
    if (m_selected_entity_idx >= 0 && m_selected_entity_idx < static_cast<int>(m_instances.size())) {
        selected_entity_id = m_instances[static_cast<std::size_t>(m_selected_entity_idx)].instance_id;
    }
    if (m_hovered_entity_idx >= 0 && m_hovered_entity_idx < static_cast<int>(m_instances.size())) {
        hovered_entity_id = m_instances[static_cast<std::size_t>(m_hovered_entity_idx)].instance_id;
    }
    SortEntitiesGeometrically(m_instances);
    m_selected_entity_idx = selected_entity_id != 0 ? FindInstanceIndex(selected_entity_id) : -1;
    m_hovered_entity_idx = hovered_entity_id != 0 ? FindInstanceIndex(hovered_entity_id) : -1;

    // Delegate the actual drawing to specialized renderer classes
    // 1. Draw the isometric map background and foreground
    m_mapRenderer.Render(m_cam_x, m_cam_y);
    // 2. Draw the heightmap overlay when enabled
    if (m_show_heightmap) {
        m_heightmapRenderer.Render();
    }
    // 3. Draw tile swap source/destination outlines
    RenderTileSwapOutlines();
    // 4. Draw door cells and their tilemap traces
    RenderDoors();
    // 5. Draw room warp handles at floor level
    RenderWarps();
    // 6. Draw the animated sprites on top
    SpriteRenderer::OcclusionMode occlusion_mode = SpriteRenderer::OcclusionMode::AlwaysOnTop;
    switch (m_entity_occlusion_idx) {
        case 1:
            occlusion_mode = SpriteRenderer::OcclusionMode::ObscuredTransparent;
            break;
        case 2:
            occlusion_mode = SpriteRenderer::OcclusionMode::ObscuredHidden;
            break;
        case 0:
        default:
            occlusion_mode = SpriteRenderer::OcclusionMode::AlwaysOnTop;
            break;
    }
    const GLint sprite_occlusion_ref = m_show_heightmap ? 0x01 : 0x05;
    const GLint sprite_occlusion_mask = m_show_heightmap ? 0x01 : 0x05;
    std::set<uint32_t> collided_entities = FindCollidedEntities(m_instances);
    int selected_collision_warning = 0;
    if (m_selected_entity_idx >= 0 && m_selected_entity_idx < static_cast<int>(m_instances.size())) {
        const SpriteInstance& selected = m_instances[static_cast<std::size_t>(m_selected_entity_idx)];
        if (EntityCollidesWithHeightmap(selected)) {
            selected_collision_warning = 2;
        } else if (collided_entities.count(selected.instance_id) != 0) {
            selected_collision_warning = 1;
        }
    }
    m_spriteRenderer.Render(
        m_instances,
        m_cam_x,
        m_cam_y,
        m_selected_entity_idx,
        selected_collision_warning,
        occlusion_mode,
        m_show_hitboxes,
        sprite_occlusion_ref,
        sprite_occlusion_mask,
        [this](
            GLint entity_back_depth,
            GLint entity_front_depth,
            float entity_z,
            float entity_min_x,
            float entity_min_y,
            float entity_max_x,
            float entity_max_y,
            float entity_top_z,
            float sprite_min_x,
            float sprite_min_y,
            float sprite_max_x,
            float sprite_max_y) {
            m_heightmapRenderer.BuildEntityOcclusionStencil(
                entity_back_depth,
                entity_front_depth,
                entity_z,
                entity_min_x,
                entity_min_y,
                entity_max_x,
                entity_max_y,
                entity_top_z,
                sprite_min_x,
                sprite_min_y,
                sprite_max_x,
                sprite_max_y);
            if (!m_show_heightmap) {
                m_mapRenderer.BuildForegroundCoverageStencil();
            }
        },
        [this](float x, float y) {
            return FloorUnderPoint(x, y);
        },
        [this](float min_x, float min_y, float max_x, float max_y, float z) {
            return ShadowOccludedByHeightmap(min_x, min_y, max_x, max_y, z);
        },
        [this](float min_x, float min_y, float max_x, float max_y, float z, float screen_min_x, float screen_min_y, float screen_max_x, float screen_max_y) {
            GLint back_depth = std::clamp(static_cast<int>(std::floor(min_x + min_y + 1.0f)), 0, 255);
            GLint front_depth = std::clamp(static_cast<int>(std::ceil(max_x + max_y + 25.0f)), 0, 255);
            m_heightmapRenderer.BuildEntityOcclusionStencil(
                back_depth,
                front_depth,
                z,
                min_x,
                min_y,
                max_x,
                max_y,
                z + 0.125f,
                screen_min_x,
                screen_min_y,
                screen_max_x,
                screen_max_y);
        },
        [&collided_entities](uint32_t instance_id) {
            return collided_entities.count(instance_id) != 0;
        });
    RenderEntityControls();
    RenderSelectedEntityTooltip();
    RenderSelectedWarpTooltip();
    RenderSelectedDoorTooltip();
    RenderSelectedTileSwapRegionTooltip();
    if (m_debug_occlusion &&
        m_selected_entity_idx >= 0 &&
        m_selected_entity_idx < static_cast<int>(m_instances.size())) {
        const auto& selected = m_instances[static_cast<std::size_t>(m_selected_entity_idx)];
        auto depth_range = EntityOcclusionDebugDepthRange(selected);
        float center_x = selected.map_x + selected.hitbox_offset;
        float center_y = selected.map_y + selected.hitbox_offset;
        float half_base = selected.hitbox_base * 0.5f;
        float top_z = selected.map_z + std::max(selected.hitbox_height, 0.125f);
        std::array<PickPoint, 8> clip_bounds = {
            ProjectEntityGridPoint(selected, center_x - half_base, center_y - half_base, selected.map_z),
            ProjectEntityGridPoint(selected, center_x + half_base, center_y - half_base, selected.map_z),
            ProjectEntityGridPoint(selected, center_x + half_base, center_y + half_base, selected.map_z),
            ProjectEntityGridPoint(selected, center_x - half_base, center_y + half_base, selected.map_z),
            ProjectEntityGridPoint(selected, center_x - half_base, center_y - half_base, top_z),
            ProjectEntityGridPoint(selected, center_x + half_base, center_y - half_base, top_z),
            ProjectEntityGridPoint(selected, center_x + half_base, center_y + half_base, top_z),
            ProjectEntityGridPoint(selected, center_x - half_base, center_y + half_base, top_z)
        };
        float clip_min_x = clip_bounds.front().x;
        float clip_min_y = clip_bounds.front().y;
        float clip_max_x = clip_bounds.front().x;
        float clip_max_y = clip_bounds.front().y;
        for (const auto& point : clip_bounds) {
            clip_min_x = std::min(clip_min_x, point.x);
            clip_min_y = std::min(clip_min_y, point.y);
            clip_max_x = std::max(clip_max_x, point.x);
            clip_max_y = std::max(clip_max_y, point.y);
        }
        if (!m_show_heightmap) {
            m_heightmapRenderer.BuildEntityOcclusionStencil(
                depth_range.first,
                depth_range.second,
                selected.map_z,
                center_x - half_base,
                center_y - half_base,
                center_x + half_base,
                center_y + half_base,
                top_z,
                clip_min_x,
                clip_min_y,
                clip_max_x,
                clip_max_y);
            m_mapRenderer.BuildForegroundCoverageStencil();
            DrawStencilOverlay(m_cam_x, m_cam_y, w, h, 0x04, 0x04, 0.0f, 0.8f, 1.0f, 0.22f);
            DrawStencilOverlay(m_cam_x, m_cam_y, w, h, 0x05, 0x05, 1.0f, 0.0f, 1.0f, 0.38f);
        }
        m_heightmapRenderer.RenderEntityOcclusionDebug(
            depth_range.first,
            depth_range.second,
            selected.map_z,
            center_x - half_base,
            center_y - half_base,
            center_x + half_base,
            center_y + half_base,
            top_z);
    }

    RenderRoomInfoTable(w, h);

    // Display the buffer on screen (double buffering)
    SwapBuffers();
}
