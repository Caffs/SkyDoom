#include "skydoom_shared.h"

#ifdef _WIN32

#include <windows.h>
#include <string.h>

#include "doomstat.h"
#include "doomdef.h"
#include "d_player.h"

static HANDLE skydoom_mapping = NULL;
static SkyDoomSharedState *skydoom_state = NULL;

int SkyDoom_SharedInit(void)
{
    skydoom_mapping = CreateFileMappingA(
        INVALID_HANDLE_VALUE,
        NULL,
        PAGE_READWRITE,
        0,
        sizeof(SkyDoomSharedState),
        SKYDOOM_MAPPING_NAME
    );

    if (skydoom_mapping == NULL)
    {
        return 0;
    }

    skydoom_state = (SkyDoomSharedState *) MapViewOfFile(
        skydoom_mapping,
        FILE_MAP_ALL_ACCESS,
        0,
        0,
        sizeof(SkyDoomSharedState)
    );

    if (skydoom_state == NULL)
    {
        CloseHandle(skydoom_mapping);
        skydoom_mapping = NULL;
        return 0;
    }

    memset(skydoom_state, 0, sizeof(SkyDoomSharedState));

    skydoom_state->magic = SKYDOOM_MAGIC;
    skydoom_state->version = SKYDOOM_VERSION;
    skydoom_state->doom_running = 1;

    return 1;
}

void SkyDoom_SharedUpdate(void)
{
    player_t *player;

    if (skydoom_state == NULL)
    {
        return;
    }

    skydoom_state->doom_running = 1;
    skydoom_state->episode = gameepisode;
    skydoom_state->map = gamemap;

    player = &players[consoleplayer];

    skydoom_state->health = player->health;
    skydoom_state->armor = player->armorpoints;
    skydoom_state->ready_weapon = (int32_t) player->readyweapon;

    skydoom_state->ammo_bullets = player->ammo[am_clip];
    skydoom_state->ammo_shells = player->ammo[am_shell];
    skydoom_state->ammo_rockets = player->ammo[am_misl];
    skydoom_state->ammo_cells = player->ammo[am_cell];

    skydoom_state->heartbeat++;
}

void SkyDoom_SharedShutdown(void)
{
    if (skydoom_state != NULL)
    {
        skydoom_state->doom_running = 0;

        UnmapViewOfFile(skydoom_state);
        skydoom_state = NULL;
    }

    if (skydoom_mapping != NULL)
    {
        CloseHandle(skydoom_mapping);
        skydoom_mapping = NULL;
    }
}

#else

int SkyDoom_SharedInit(void)
{
    return 0;
}

void SkyDoom_SharedUpdate(void)
{
}

void SkyDoom_SharedShutdown(void)
{
}

#endif