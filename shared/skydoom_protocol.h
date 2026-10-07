#ifndef SKYDOOM_PROTOCOL_H
#define SKYDOOM_PROTOCOL_H

#include <stdint.h>

#define SKYDOOM_MAPPING_NAME "Local\\SkyDoom_Guest_v11"

#define SKYDOOM_MAGIC   0x314D4453u
#define SKYDOOM_VERSION 11u

#define SKYDOOM_PROTOCOL_FLAG_GUEST_MODE 0x00000001u


#define SKYDOOM_INPUT_RING_ENTRIES 256u
#define SKYDOOM_INPUT_RING_MASK    255u

#define SKYDOOM_INPUT_EVENT_BUTTON      1u
#define SKYDOOM_INPUT_EVENT_RELEASE_ALL 2u

/*
    SKYDOOM_DOOM_HEALTH_V7

    Skyrim -> Chocolate Doom incoming damage.

    SkyDoomInputEvent.value contains the integer damage amount.
*/
#define SKYDOOM_INPUT_EVENT_DAMAGE      3u
#define SKYDOOM_INPUT_EVENT_RESPAWN     4u
#define SKYDOOM_INPUT_EVENT_ROCKET_DAMAGE 5u
#define SKYDOOM_INPUT_EVENT_PICKUP        6u

/* SKYDOOM_MUSIC_TOGGLE_V15_8 */
#define SKYDOOM_INPUT_EVENT_MUSIC_TOGGLE  7u

#define SKYDOOM_PICKUP_NONE              0u
#define SKYDOOM_PICKUP_MEDIKIT           1u
#define SKYDOOM_PICKUP_ARMOR             2u
#define SKYDOOM_PICKUP_BULLETS           3u
#define SKYDOOM_PICKUP_SHELLS            4u
#define SKYDOOM_PICKUP_ROCKET            5u
#define SKYDOOM_PICKUP_CELLS             6u
#define SKYDOOM_PICKUP_ARMOR_BONUS       7u

/* SKYDOOM_COMPLETE_ITEMS_V15_9A1 */
#define SKYDOOM_PICKUP_STIMPACK          8u
#define SKYDOOM_PICKUP_BLUE_ARMOR        9u
#define SKYDOOM_PICKUP_BULLET_BOX       10u
#define SKYDOOM_PICKUP_SHELL_BOX        11u
#define SKYDOOM_PICKUP_ROCKET_BOX       12u
#define SKYDOOM_PICKUP_CELL_PACK        13u
/* SKYDOOM_COMPLETE_ITEMS_V15_9B1 */
#define SKYDOOM_PICKUP_HEALTH_BONUS      14u
#define SKYDOOM_PICKUP_SOULSPHERE        15u
#define SKYDOOM_PICKUP_BACKPACK          16u
#define SKYDOOM_PICKUP_MEGASPHERE        17u


#define SKYDOOM_INPUT_FORWARD         1u
#define SKYDOOM_INPUT_BACK            2u
#define SKYDOOM_INPUT_STRAFE_LEFT     3u
#define SKYDOOM_INPUT_STRAFE_RIGHT    4u
#define SKYDOOM_INPUT_RUN             5u
#define SKYDOOM_INPUT_FIRE            6u
#define SKYDOOM_INPUT_USE             7u
#define SKYDOOM_INPUT_JUMP            8u

#define SKYDOOM_INPUT_WEAPON_PISTOL    9u
#define SKYDOOM_INPUT_WEAPON_SHOTGUN  10u
#define SKYDOOM_INPUT_WEAPON_CHAINGUN 11u
#define SKYDOOM_INPUT_WEAPON_MELEE   12u
#define SKYDOOM_INPUT_WEAPON_ROCKET  13u
#define SKYDOOM_INPUT_WEAPON_PLASMA  14u
#define SKYDOOM_INPUT_WEAPON_BFG     15u


#define SKYDOOM_COMBAT_RING_ENTRIES 256u
#define SKYDOOM_COMBAT_RING_MASK    255u

#define SKYDOOM_COMBAT_EVENT_PELLET 1u
#define SKYDOOM_COMBAT_EVENT_MELEE  2u
#define SKYDOOM_COMBAT_EVENT_ROCKET_FIRED  3u
#define SKYDOOM_COMBAT_EVENT_ROCKET_DAMAGE 4u
#define SKYDOOM_COMBAT_EVENT_PLASMA_FIRED  5u
#define SKYDOOM_COMBAT_EVENT_BFG_FIRED     6u
#define SKYDOOM_COMBAT_EVENT_PICKUP_RESULT 7u

#define SKYDOOM_COMBAT_WEAPON_PISTOL  1u
#define SKYDOOM_COMBAT_WEAPON_SHOTGUN 2u
#define SKYDOOM_COMBAT_WEAPON_CHAINGUN 3u
#define SKYDOOM_COMBAT_WEAPON_FIST     4u
#define SKYDOOM_COMBAT_WEAPON_CHAINSAW 5u
#define SKYDOOM_COMBAT_WEAPON_ROCKET   6u
#define SKYDOOM_COMBAT_WEAPON_PLASMA   7u
#define SKYDOOM_COMBAT_WEAPON_BFG      8u


#define SKYDOOM_OVERLAY_WIDTH  320u
#define SKYDOOM_OVERLAY_HEIGHT 200u

#define SKYDOOM_OVERLAY_RGBA_BYTES \
    (SKYDOOM_OVERLAY_WIDTH * SKYDOOM_OVERLAY_HEIGHT * 4u)

#define SKYDOOM_OVERLAY_FLAG_WEAPON 0x00000001u


#pragma pack(push, 8)


typedef struct SkyDoomInputEvent
{
    uint16_t type;
    uint16_t code;

    int32_t value;

    uint32_t sequence;
    uint32_t reserved;

} SkyDoomInputEvent;


typedef struct SkyDoomInputRing
{
    volatile uint32_t head;
    volatile uint32_t tail;

    volatile uint32_t dropped;
    uint32_t reserved;

    SkyDoomInputEvent events[
        SKYDOOM_INPUT_RING_ENTRIES
    ];

} SkyDoomInputRing;


typedef struct SkyDoomCombatEvent
{
    uint16_t type;
    uint16_t weapon;

    int32_t damage;

    int32_t angle_offset;
    int32_t slope_offset;

} SkyDoomCombatEvent;


typedef struct SkyDoomCombatRing
{
    volatile uint32_t head;
    volatile uint32_t tail;

    volatile uint32_t dropped;
    uint32_t reserved;

    SkyDoomCombatEvent events[
        SKYDOOM_COMBAT_RING_ENTRIES
    ];

} SkyDoomCombatRing;


typedef struct SkyDoomSkyrimState
{
    uint32_t running;
    uint32_t pid;

    uint32_t paused;
    uint32_t in_game;

    float player_x;
    float player_y;
    float player_z;

    float player_yaw;
    float player_pitch;

    uint32_t reserved0;

    uint64_t heartbeat_ms;
    uint64_t update_counter;

} SkyDoomSkyrimState;


typedef struct SkyDoomDoomState
{
    uint32_t running;
    uint32_t pid;

    uint32_t in_level;

    uint32_t episode;
    uint32_t map;

    int32_t health;
    int32_t armor;

    int32_t weapon;

    int32_t ammo_bullets;
    int32_t ammo_shells;
    int32_t ammo_rockets;
    int32_t ammo_cells;

    /* SKYDOOM_COMPLETE_ITEMS_V15_9B1 */
    int32_t maxammo_bullets;
    int32_t maxammo_shells;
    int32_t maxammo_rockets;
    int32_t maxammo_cells;

    uint32_t has_backpack;
    uint32_t commercial_mode;

    int32_t accepted_forward;
    int32_t accepted_side;

    uint32_t accepted_buttons;

    uint32_t guest_mode;

    uint64_t heartbeat_ms;
    uint64_t tick_counter;

    uint64_t pistol_shot_serial;
    uint64_t pistol_damage_total;

} SkyDoomDoomState;


typedef struct SkyDoomOverlayFrame
{
    volatile uint32_t seq;

    uint32_t width;
    uint32_t height;

    uint32_t flags;

    uint64_t frame_id;

    uint8_t rgba[
        SKYDOOM_OVERLAY_RGBA_BYTES
    ];

} SkyDoomOverlayFrame;


typedef struct SkyDoomSharedState
{
    uint32_t magic;
    uint32_t version;
    uint32_t struct_size;

    uint32_t protocol_flags;

    SkyDoomSkyrimState skyrim;
    SkyDoomDoomState doom;

    SkyDoomInputRing input;

    SkyDoomCombatRing combat;

    SkyDoomOverlayFrame overlay;

} SkyDoomSharedState;


#pragma pack(pop)


#ifdef __cplusplus

static_assert(
    sizeof(SkyDoomInputEvent) == 16,
    "SkyDoomInputEvent size mismatch"
    );

static_assert(
    sizeof(SkyDoomInputRing) == 4112,
    "SkyDoomInputRing size mismatch"
    );

static_assert(
    sizeof(SkyDoomCombatEvent) == 16,
    "SkyDoomCombatEvent size mismatch"
    );

static_assert(
    sizeof(SkyDoomCombatRing) == 4112,
    "SkyDoomCombatRing size mismatch"
    );

static_assert(
    sizeof(SkyDoomSkyrimState) == 56,
    "SkyDoomSkyrimState size mismatch"
    );

static_assert(
    sizeof(SkyDoomDoomState) == 120,
    "SkyDoomDoomState size mismatch"
    );

static_assert(
    sizeof(SkyDoomOverlayFrame) == 256024,
    "SkyDoomOverlayFrame size mismatch"
    );

static_assert(
    sizeof(SkyDoomSharedState) == 264440,
    "SkyDoomSharedState size mismatch"
    );

#endif

#endif
