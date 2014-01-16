#include <llmr/map/map.hpp>
#include <llmr/map/tile.hpp>
#include <llmr/util/vec2.hpp>

#include <iostream>
#include <thread>

#include <cmath>
#include <cassert>

using namespace llmr;

map::map(class settings *settings)
    : settings(settings),
      transform(new class transform()),
      painter(new class painter(transform)),
      min_zoom(0),
      max_zoom(14) {
}

map::~map() {
    delete transform;
}

void map::setup() {
    painter->setup();
}

void map::loadSettings() {
    transform->setAngle(settings->angle);
    transform->setScale(settings->scale);
    transform->setLonLat(settings->longitude, settings->latitude);
    update();
}

void map::resize(uint32_t width, uint32_t height) {
    transform->width = width;
    transform->height = height;
    update();
}

void map::moveBy(double dx, double dy) {
    transform->moveBy(dx, dy);
    update();

    transform->getLonLat(settings->longitude, settings->latitude);
    settings->save();
}

void map::scaleBy(double ds, double cx, double cy) {
    transform->scaleBy(ds, cx, cy);
    update();

    transform->getLonLat(settings->longitude, settings->latitude);
    settings->scale = transform->getScale();
    settings->save();
}

void map::rotateBy(double cx, double cy, double sx, double sy, double ex, double ey) {
    transform->rotateBy(cx, cy, sx, sy, ex, ey);
    update();

    settings->angle = transform->getAngle();
    settings->save();
}

void map::resetNorth() {
    transform->setAngle(0);
    update();

    settings->angle = transform->getAngle();
    settings->save();
}

void map::resetPosition() {
    transform->setAngle(0);
    transform->setLonLat(0, 0);
    transform->setZoom(0);
    update();

    transform->getLonLat(settings->longitude, settings->latitude);
    settings->scale = transform->getScale();
    settings->angle = transform->getAngle();
    settings->save();
}

void map::toggleDebug() {
    settings->debug = !settings->debug;
    update();

    settings->save();
}

void map::update() {
    updateTiles();
    platform::restart(this);
}


tile::ptr map::hasTile(const tile_id& id) {
    for (tile::ptr& tile : tiles) {
        if (tile->id == id) {
            return tile;
        }
    }

    return tile::ptr();
}

tile::ptr map::addTile(const tile_id& id) {
    tile::ptr tile = hasTile(id);

    if (!tile.get()) {
        // We couldn't find the tile in the list. Create a new one.
        tile = std::make_shared<class tile>(id);
        assert(tile);
        // std::cerr << "init " << id.z << "/" << id.x << "/" << id.y << std::endl;
        // std::cerr << "add " << tile->toString() << std::endl;
        tiles.push_front(tile);
    }

    return tile;
}

/**
 * Recursively find children of the given tile that are already loaded.
 *
 * @param id The tile ID that we should find children for.
 * @param maxCoveringZoom The maximum zoom level of children to look for.
 * @param retain An object that we add the found tiles to.
 *
 * @return boolean Whether the children found completely cover the tile.
 */
bool map::findLoadedChildren(const tile_id& id, int32_t maxCoveringZoom, std::forward_list<tile_id>& retain) {
    bool complete = true;
    int32_t z = id.z;

    auto ids = tile::children(id, z + 1);
    for (const tile_id& child_id : ids) {
        const tile::ptr& tile = hasTile(child_id);
        if (tile && tile->state == tile::ready) {
            assert(tile);
            retain.emplace_front(tile->id);
        } else {
            complete = false;
            if (z < maxCoveringZoom) {
                // Go further down the hierarchy to find more unloaded children.
                findLoadedChildren(child_id, maxCoveringZoom, retain);
            }
        }
    }
    return complete;
};

/**
 * Find a loaded parent of the given tile.
 *
 * @param id The tile ID that we should find children for.
 * @param minCoveringZoom The minimum zoom level of parents to look for.
 * @param retain An object that we add the found tiles to.
 *
 * @return boolean Whether a parent was found.
 */
bool map::findLoadedParent(const tile_id& id, int32_t minCoveringZoom, std::forward_list<tile_id>& retain) {
    for (int32_t z = id.z - 1; z >= minCoveringZoom; z--) {
        const tile_id parent_id = tile::parent(id, z);
        const tile::ptr tile = hasTile(parent_id);
        if (tile && tile->state == tile::ready) {
            assert(tile);
            retain.emplace_front(tile->id);
            return true;
        }
    }
    return false;
};


void map::updateTiles() {
    // Figure out what tiles we need to load
    int32_t zoom = transform->getZoom();
    if (zoom > max_zoom) zoom = max_zoom;
    if (zoom < min_zoom) zoom = min_zoom;

    int32_t max_covering_zoom = zoom + 1;
    if (max_covering_zoom > max_zoom) max_covering_zoom = max_zoom;

    int32_t min_covering_zoom = zoom - 10;
    if (min_covering_zoom < min_zoom) min_covering_zoom = min_zoom;


    int32_t max_dim = pow(2, zoom);

    // Map four viewport corners to pixel coordinates
    box box;
    transform->mapCornersToBox(zoom, box);

    vec2<int32_t> tl, br;
    tl.x = fmax(0, floor(fmin(box.tl.x, box.bl.x)));
    tl.y = fmax(0, floor(fmin(box.tl.y, box.tr.y)));
    br.x = fmin(max_dim, ceil(fmax(box.tr.x, box.br.x)));
    br.y = fmin(max_dim, ceil(fmax(box.bl.y, box.br.y)));


    // TODO: Discard tiles that are outside the viewport
    std::forward_list<tile_id> required;
    for (int32_t y = tl.y; y < br.y; y++) {
        for (int32_t x = tl.x; x < br.x; x++) {
            required.emplace_front(x, y, zoom);
        }
    }

    // Retain is a list of tiles that we shouldn't delete, even if they are not
    // the most ideal tile for the current viewport. This may include tiles like
    // parent or child tiles that are *already* loaded.
    std::forward_list<tile_id> retain(required);

    // Add existing child/parent tiles if the actual tile is not yet loaded
    for (const tile_id& id : required) {
        tile::ptr tile = addTile(id);
        assert(tile);

        if (tile->state != tile::ready) {
            // The tile we require is not yet loaded. Try to find a parent or
            // child tile that we already have.

            // First, try to find existing child tiles that completely cover the
            // missing tile.
            bool complete = findLoadedChildren(id, max_covering_zoom, retain);

            // Then, if there are no complete child tiles, try to find existing
            // parent tiles that completely cover the missing tile.
            if (!complete) {
                findLoadedParent(id, min_covering_zoom, retain);
            }
        }

        if (tile->state == tile::initial) {
            // If the tile is new, we have to make sure to load it.
            tile->state = tile::loading;
            platform::request(this, tile);
        }
    }

    // Remove tiles that we definitely don't need, i.e. tiles that are not on
    // the required list.
    tiles.remove_if([&retain](const tile::ptr& tile) {
        assert(tile);
        bool obsolete = std::find(retain.begin(), retain.end(), tile->id) == retain.end();
        if (obsolete) {
            tile->cancel();
        }
        return obsolete;
    });

    // Sort tiles by zoom level, front to back.
    // We're painting front-to-back, so we want to draw more detailed tiles first
    // before filling in other parts with lower zoom levels.
    tiles.sort([](const tile::ptr& a, const tile::ptr& b) {
        return a->id.z > b->id.z;
    });
}

bool map::render() {
    painter->clear();

    for (tile::ptr& tile : tiles) {
        assert(tile);
        if (tile->state == tile::ready) {
            painter->render(tile);
        }
    }

    return false;
}

void map::tileLoaded(tile::ptr tile) {
    // std::cerr << "loaded " << tile->toString() << std::endl;
    update();
}

void map::tileFailed(tile::ptr tile) {
    // fprintf(stderr, "[%8zx] tile failed to load %d/%d/%d\n",
    //         std::hash<std::thread::id>()(std::this_thread::get_id()),
    //         tile->z, tile->x, tile->y);
}
