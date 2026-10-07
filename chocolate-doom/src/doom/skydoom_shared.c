#include "skydoom_shared.h"
#include "m_random.h"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>

#include <stdlib.h>
#include <string.h>

#include "doomstat.h"
#include "doomdef.h"
#include "d_player.h"
#include "d_ticcmd.h"
#include "p_local.h"

#include "i_video.h"
#include "st_stuff.h"
#include "m_argv.h"
#include "s_sound.h"
#include "sounds.h"
#include "w_wad.h"
#include "z_zone.h"


#define SKYDOOM_BT_ATTACK 0x01u
#define SKYDOOM_BT_USE    0x02u

#define SKYDOOM_FORWARD_WALK 25
#define SKYDOOM_FORWARD_RUN  50

#define SKYDOOM_SIDE_WALK 24
#define SKYDOOM_SIDE_RUN  40

#define SKYDOOM_SKYRIM_TIMEOUT_MS 1500ULL

/*
    Make a quick Skyrim attack click remain present
    for a couple of real DOOM tics.
*/

#define SKYDOOM_ACTION_PULSE_TICS 2


static HANDLE skydoom_mapping = NULL;

static SkyDoomSharedState *skydoom_state = NULL;

static int skydoom_guest_mode = 0;


/*
    Guest input state.
*/

static int skydoom_forward_down = 0;
static int skydoom_back_down = 0;

static int skydoom_left_down = 0;
static int skydoom_right_down = 0;

static int skydoom_run_down = 0;

static int skydoom_fire_down = 0;
static int skydoom_use_down = 0;

static int skydoom_jump_down = 0;

static int skydoom_fire_pulse_tics = 0;
static int skydoom_use_pulse_tics = 0;



/*

    SKYDOOM_WEAPON_SELECTION_V6



    Weapon requests come from Skyrim's physical 2 / 3 keys.

*/



static int skydoom_pending_weapon = -1;



/*

    SKYDOOM_DOOM_HEALTH_V7



    Incoming Skyrim damage waits here until the guest reaches the

    normal DOOM command/tic path.



    P_DamageMobj is then responsible for armor, health, pain flash

    and death.

*/



static int skydoom_pending_external_damage = 0;





/*

    Temporary development loadout.



    Grant the normal shotgun once when a DOOM map becomes active

    so the integration can be tested before inventory/pickups are

    bridged properly.

*/



static int skydoom_dev_loadout_episode = -1;

static int skydoom_dev_loadout_map = -1;


/*
    Snapshot of the DOOM framebuffer immediately
    before the first-person weapon is drawn.

    Comparing before/after isolates the actual weapon
    animation without including E1M1's world graphics.
*/

static pixel_t skydoom_before_weapon[SCREENWIDTH * SCREENHEIGHT];

static int skydoom_weapon_capture_ready = 0;

/*
    SKYDOOM_NATIVE_PICKUP_MESSAGES_V15_6

    HU_Drawer renders into Chocolate Doom's hidden framebuffer.
    Capture only the pixels HU_Drawer changes, then merge those
    genuine Doom glyph pixels into SkyDoom's transparent overlay.
*/
static pixel_t
    skydoom_before_hud[
        SCREENWIDTH *
        SCREENHEIGHT
    ];

static uint8_t
    skydoom_hud_rgba[
        SKYDOOM_OVERLAY_RGBA_BYTES
    ];

static int
    skydoom_hud_capture_ready =
        0;

/*
    SKYDOOM_NATIVE_PICKUP_MESSAGE_PERSIST_V15_6_R1

    Bounding box of the most recently captured genuine Doom HUD
    message.  Used only to draw the requested compact black panel.
*/
static int
    skydoom_hud_message_bounds_valid =
        0;

static int
    skydoom_hud_message_min_x =
        0;

static int
    skydoom_hud_message_max_x =
        0;

static int
    skydoom_hud_message_min_y =
        0;

static int
    skydoom_hud_message_max_y =
        0;


/*
    ------------------------------------------------------------
    INPUT
    ------------------------------------------------------------
*/

static void SkyDoom_ClearHeldInput(void)
{
    skydoom_forward_down = 0;
    skydoom_back_down = 0;

    skydoom_left_down = 0;
    skydoom_right_down = 0;

    skydoom_run_down = 0;

    skydoom_fire_down = 0;
    skydoom_use_down = 0;

    skydoom_jump_down = 0;

    skydoom_fire_pulse_tics = 0;
    skydoom_use_pulse_tics = 0;

    skydoom_pending_weapon = -1;

    skydoom_pending_external_damage = 0;
}


static void SkyDoom_InitializeProtocolIfNeeded(int newly_created)
{
    if (skydoom_state == NULL)
    {
        return;
    }

    if (newly_created || skydoom_state->magic != SKYDOOM_MAGIC ||
        skydoom_state->version != SKYDOOM_VERSION ||
        skydoom_state->struct_size != sizeof(SkyDoomSharedState))
    {
        ZeroMemory(skydoom_state, sizeof(SkyDoomSharedState));

        skydoom_state->magic = SKYDOOM_MAGIC;

        skydoom_state->version = SKYDOOM_VERSION;

        skydoom_state->struct_size = (uint32_t) sizeof(SkyDoomSharedState);

        skydoom_state->protocol_flags = SKYDOOM_PROTOCOL_FLAG_GUEST_MODE;

        skydoom_state->overlay.width = SKYDOOM_OVERLAY_WIDTH;

        skydoom_state->overlay.height = SKYDOOM_OVERLAY_HEIGHT;

        skydoom_state->overlay.flags = SKYDOOM_OVERLAY_FLAG_WEAPON;
    }
}


static int SkyDoom_SkyrimIsFresh(void)
{
    ULONGLONG now;
    uint64_t heartbeat;

    if (skydoom_state == NULL)
    {
        return 0;
    }

    if (!skydoom_state->skyrim.running)
    {
        return 0;
    }

    heartbeat = skydoom_state->skyrim.heartbeat_ms;

    if (heartbeat == 0)
    {
        return 0;
    }

    now = GetTickCount64();

    if (now < heartbeat)
    {
        return 1;
    }

    return (now - heartbeat) <= SKYDOOM_SKYRIM_TIMEOUT_MS;
}


/*
    SKYDOOM_PICKUP_GRANT_V15
    Native Doom pickup helper implemented in p_inter.c.
*/
boolean P_GiveBody(
    player_t *player,
    int num
);
/*
    SKYDOOM_MULTI_PICKUP_GRANT_V15_3
    Native Doom helpers implemented in p_inter.c.
*/
boolean P_GiveAmmo(
    player_t *player,
    ammotype_t ammo,
    int num
);

boolean P_GiveArmor(
    player_t *player,
    int armortype
);

/*
    SKYDOOM_ARMOR_BONUS_GRANT_V15_5
    Implemented in p_inter.c using Doom's real deh_max_armor.
*/
boolean SkyDoom_GiveArmorBonus(
    player_t *player
);

/* SKYDOOM_COMPLETE_ITEMS_V15_9B1 */
boolean SkyDoom_GiveHealthBonusV15_9(
    player_t *player
);

boolean SkyDoom_GiveSoulsphereV15_9(
    player_t *player
);

boolean SkyDoom_GiveBackpackV15_9(
    player_t *player
);

boolean SkyDoom_GiveMegasphereV15_9(
    player_t *player
);

void SkyDoom_PlayResourcePickupSoundV15_9(
    player_t *player,
    int pickup_type
);

/*
    SKYDOOM_NATIVE_PICKUP_MESSAGES_V15_6
    Implemented beside Doom's normal pickup rules in p_inter.c.
*/
void SkyDoom_SetResourcePickupMessage(
    player_t *player,
    int pickup_type
);

/*
    SKYDOOM_HUD_MESSAGE_ACTIVE_FORWARD_V15_6_R1

    hu_stuff.c owns the implementation.
    Keep this explicit prototype here so C does not fall back to
    an implicit-int declaration when the overlay bridge queries
    Doom's message_on state.
*/
boolean SkyDoom_HUDMessageActive(void);

/*
    SKYDOOM_PUSH_COMBAT_FORWARD_V15_R3
    The v15 pickup input handler runs before the existing
    combat-ring helper's definition, so give C the exact
    prototype derived from that real definition below.
*/
static void SkyDoom_PushCombatEvent(

    uint16_t type,

    uint16_t weapon,

    int32_t damage,

    int32_t angle_offset,

    int32_t slope_offset

);

static void SkyDoom_ApplyInputEvent(const SkyDoomInputEvent *event)
{
    int down;

    if (event == NULL)
    {
        return;
    }

    /*
        SKYDOOM_MUSIC_TOGGLE_V15_8

        F10 on the Skyrim host sends this one-shot event.

        Chocolate Doom remains authoritative for its audio backend.
        The existing SkyDoom Doom-HUD message bridge gives immediate
        visual feedback using the same top-left panel as pickups.
    */
    if (
        event->type ==
        SKYDOOM_INPUT_EVENT_MUSIC_TOGGLE
    )
    {
        const boolean music_enabled =
            S_SkyDoomToggleMusic();

        if (
            consoleplayer >= 0 &&
            consoleplayer < MAXPLAYERS &&
            playeringame[
                consoleplayer
            ]
        )
        {
            players[
                consoleplayer
            ].message =
                music_enabled ?
                    "DOOM MUSIC ON" :
                    "DOOM MUSIC OFF";
        }

        return;
    }
        /*
        SKYDOOM_PICKUP_GRANT_V15
        SKYDOOM_MULTI_PICKUP_GRANT_V15_3

        Skyrim owns the physical sprite/proximity.
        Chocolate Doom owns the genuine pickup rules.
    */
    if (
        event->type ==
        SKYDOOM_INPUT_EVENT_PICKUP
    )
    {
        int accepted;
        int resource_after;
        player_t *player;

        accepted = 0;
        resource_after = 0;
        player = NULL;

        if (
            event->value > 0 &&
            consoleplayer >= 0 &&
            consoleplayer < MAXPLAYERS &&
            playeringame[consoleplayer] &&
            players[consoleplayer].mo != NULL &&
            players[consoleplayer].mo->health > 0
        )
        {
            player =
                &players[consoleplayer];

            switch (event->code)
            {
                case SKYDOOM_PICKUP_MEDIKIT:
                    accepted =
                        P_GiveBody(
                            player,
                            25
                        ) ?
                            1 :
                            0;

                    resource_after =
                        player->health;
                    break;

                case SKYDOOM_PICKUP_ARMOR:
                    /*
                        Genuine green armor behaviour:
                        armortype 1 -> set armor to 100,
                        rejected if Doom armor is already >=100.
                    */
                    accepted =
                        P_GiveArmor(
                            player,
                            1
                        ) ?
                            1 :
                            0;

                    resource_after =
                        player->armorpoints;
                    break;

                case SKYDOOM_PICKUP_ARMOR_BONUS:
                    /*
                        Genuine BON2 armor bonus:
                        +1 armor, may exceed 100, Doom cap remains
                        authoritative through deh_max_armor.
                    */
                    accepted =
                        SkyDoom_GiveArmorBonus(
                            player
                        ) ?
                            1 :
                            0;

                    resource_after =
                        player->armorpoints;
                    break;

                case SKYDOOM_PICKUP_BULLETS:
                    /*
                        Genuine MF_DROPPED clip amount:
                        num=0 => half clip => 5 bullets in vanilla.
                    */
                    accepted =
                        P_GiveAmmo(
                            player,
                            am_clip,
                            0
                        ) ?
                            1 :
                            0;

                    resource_after =
                        player->ammo[am_clip];
                    break;

                case SKYDOOM_PICKUP_SHELLS:
                    accepted =
                        P_GiveAmmo(
                            player,
                            am_shell,
                            1
                        ) ?
                            1 :
                            0;

                    resource_after =
                        player->ammo[am_shell];
                    break;

                case SKYDOOM_PICKUP_ROCKET:
                    accepted =
                        P_GiveAmmo(
                            player,
                            am_misl,
                            1
                        ) ?
                            1 :
                            0;

                    resource_after =
                        player->ammo[am_misl];
                    break;

                case SKYDOOM_PICKUP_CELLS:
                    accepted =
                        P_GiveAmmo(
                            player,
                            am_cell,
                            1
                        ) ?
                            1 :
                            0;

                    resource_after =
                        player->ammo[am_cell];
                    break;

                
                /* SKYDOOM_COMPLETE_ITEMS_V15_9A1 */
                case SKYDOOM_PICKUP_STIMPACK:
                    accepted =
                        P_GiveBody(
                            player,
                            10
                        ) ?
                            1 :
                            0;

                    resource_after =
                        player->health;
                    break;

                case SKYDOOM_PICKUP_BLUE_ARMOR:
                    accepted =
                        P_GiveArmor(
                            player,
                            2
                        ) ?
                            1 :
                            0;

                    resource_after =
                        player->armorpoints;
                    break;

                case SKYDOOM_PICKUP_BULLET_BOX:
                    accepted =
                        P_GiveAmmo(
                            player,
                            am_clip,
                            5
                        ) ?
                            1 :
                            0;

                    resource_after =
                        player->ammo[am_clip];
                    break;

                case SKYDOOM_PICKUP_SHELL_BOX:
                    accepted =
                        P_GiveAmmo(
                            player,
                            am_shell,
                            5
                        ) ?
                            1 :
                            0;

                    resource_after =
                        player->ammo[am_shell];
                    break;

                case SKYDOOM_PICKUP_ROCKET_BOX:
                    accepted =
                        P_GiveAmmo(
                            player,
                            am_misl,
                            5
                        ) ?
                            1 :
                            0;

                    resource_after =
                        player->ammo[am_misl];
                    break;

                case SKYDOOM_PICKUP_CELL_PACK:
                    accepted =
                        P_GiveAmmo(
                            player,
                            am_cell,
                            5
                        ) ?
                            1 :
                            0;

                    resource_after =
                        player->ammo[am_cell];
                    break;

                /* SKYDOOM_COMPLETE_ITEMS_V15_9B1 */
                case SKYDOOM_PICKUP_HEALTH_BONUS:
                    accepted =
                        SkyDoom_GiveHealthBonusV15_9(
                            player
                        ) ?
                            1 :
                            0;

                    resource_after =
                        player->health;
                    break;

                case SKYDOOM_PICKUP_SOULSPHERE:
                    accepted =
                        SkyDoom_GiveSoulsphereV15_9(
                            player
                        ) ?
                            1 :
                            0;

                    resource_after =
                        player->health;
                    break;

                case SKYDOOM_PICKUP_BACKPACK:
                    accepted =
                        SkyDoom_GiveBackpackV15_9(
                            player
                        ) ?
                            1 :
                            0;

                    resource_after =
                        player->backpack ?
                            1 :
                            0;
                    break;

                case SKYDOOM_PICKUP_MEGASPHERE:
                    accepted =
                        SkyDoom_GiveMegasphereV15_9(
                            player
                        ) ?
                            1 :
                            0;

                    resource_after =
                        player->health;
                    break;
default:
                    break;
            }

            if (accepted)
            {
                SkyDoom_SetResourcePickupMessage(
                    player,
                    event->code
                );

                SkyDoom_PlayResourcePickupSoundV15_9(
                    player,
                    event->code
                );
            }
        }

        SkyDoom_PushCombatEvent(
            SKYDOOM_COMBAT_EVENT_PICKUP_RESULT,
            event->code,
            accepted,
            event->value,
            resource_after
        );

        return;
    }
/*

        Skyrim host damage uses the same reliable SPSC input ring

        as the existing controls.

    */



        /*

        SKYDOOM_REAL_REBORN_V8



        Skyrim has finished its own death/load cycle.



        Do NOT fake the DOOM HUD here.



        PST_REBORN asks Chocolate Doom itself to perform its normal

        player rebirth path on the next game tick. That resets the

        real player state, health, armor, face/status state and

        weapon psprites correctly.

    */



    if (

        event->type ==

        SKYDOOM_INPUT_EVENT_RESPAWN

    )

    {

        skydoom_pending_external_damage =

            0;





        skydoom_pending_weapon =

            -1;





        /*

            Our v6 shotgun grant is temporary development support.



            Reset these markers so the testing shotgun/shell loadout

            gets granted again after the hidden DOOM level reload.

        */



        skydoom_dev_loadout_episode =

            -1;





        skydoom_dev_loadout_map =

            -1;





        if (

            skydoom_guest_mode &&

            gamestate == GS_LEVEL &&

            playeringame[

                consoleplayer

            ]

        )

        {

            players[

                consoleplayer

            ].playerstate =

                PST_REBORN;

        }





        return;

    }








    /*

        SKYDOOM_ROCKET_DAMAGE_REQUEST_V11



        Skyrim detected an actual Skyrim-space rocket collision

        with an Actor.



        Consume the genuine Chocolate Doom RNG here:



            ((P_Random() % 8) + 1) * 20



        The request id is returned in combat-event angle_offset so

        Skyrim can match the response to the correct impact.

    */



    if (

        event->type ==

        SKYDOOM_INPUT_EVENT_ROCKET_DAMAGE

    )

    {

        // SKYDOOM_BFG_IMPACT_SOUND_V14_2B
        // SKYDOOM_BFG_DAMAGE_RNG_V14_3
        //
        // BFG-coded request values:
        //   0           = genuine sfx_rxplod only
        //   positive    = direct-hit RNG request id
        //   negative    = A_BFGSpray RNG request id
        if (
            event->code ==
                SKYDOOM_COMBAT_WEAPON_BFG
        )
        {
            if (event->value == 0)
            {
                if (
                    consoleplayer >= 0 &&
                    consoleplayer < MAXPLAYERS &&
                    players[consoleplayer].mo != NULL
                )
                {
                    S_StartSound(
                        players[consoleplayer].mo,
                        sfx_rxplod
                    );
                }

                return;
            }

            if (event->value > 0)
            {
                SkyDoom_ReportBFGDamageRequest(
                    event->value,
                    0
                );

                return;
            }

            SkyDoom_ReportBFGDamageRequest(
                -event->value,
                1
            );

            return;
        }

// SKYDOOM_ROCKET_EXPLOSION_SOUND_FIX_V13_1

        //

        // The hidden MT_ROCKET is suppressed while SkyDoom is active,

        // so its normal Doom deathstate never emits MT_ROCKET's

        // genuine sfx_barexp deathsound.

        //

        // A zero-valued ROCKET-coded request is sound-only.

        // Existing rocket direct-damage requests use positive values

        // and code 0, while plasma requests use code PLASMA.

        if (

            event->code ==

                SKYDOOM_COMBAT_WEAPON_ROCKET &&

            event->value ==

                0

        )

        {

            if (

                consoleplayer >= 0 &&

                consoleplayer < MAXPLAYERS &&

                players[consoleplayer].mo != NULL

            )

            {

                S_StartSound(

                    players[consoleplayer].mo,

                    sfx_barexp

                );

            }



            return;

        }



        // SKYDOOM_PLASMA_DAMAGE_V13

        if (

            event->code ==

            SKYDOOM_COMBAT_WEAPON_PLASMA

        )

        {

            if (event->value > 0)

            {

                SkyDoom_ReportPlasmaDamageRequest(

                    event->value

                );

            }

        }

        else if (event->value > 0)

        {

            SkyDoom_ReportRocketDamageRequest(

                event->value

            );

        }



        return;

    }




if (

        event->type ==

        SKYDOOM_INPUT_EVENT_DAMAGE

    )

    {

        if (event->value > 0)

        {

            skydoom_pending_external_damage +=

                event->value;





            if (

                skydoom_pending_external_damage >

                100000

            )

            {

                skydoom_pending_external_damage =

                    100000;

            }

        }



        return;

    }



    if (event->type == SKYDOOM_INPUT_EVENT_RELEASE_ALL)
    {
        SkyDoom_ClearHeldInput();

        return;
    }

    if (event->type != SKYDOOM_INPUT_EVENT_BUTTON)
    {
        return;
    }

    down = event->value != 0;

    switch (event->code)
    {
        case SKYDOOM_INPUT_FORWARD:

            skydoom_forward_down = down;

            break;


        case SKYDOOM_INPUT_BACK:

            skydoom_back_down = down;

            break;


        case SKYDOOM_INPUT_STRAFE_LEFT:

            skydoom_left_down = down;

            break;


        case SKYDOOM_INPUT_STRAFE_RIGHT:

            skydoom_right_down = down;

            break;


        case SKYDOOM_INPUT_RUN:

            skydoom_run_down = down;

            break;


        case SKYDOOM_INPUT_FIRE:

            if (down && !skydoom_fire_down)
            {
                skydoom_fire_pulse_tics = SKYDOOM_ACTION_PULSE_TICS;
            }

            skydoom_fire_down = down;

            break;


        case SKYDOOM_INPUT_USE:

            if (down && !skydoom_use_down)
            {
                skydoom_use_pulse_tics = SKYDOOM_ACTION_PULSE_TICS;
            }

            skydoom_use_down = down;

            break;


        case SKYDOOM_INPUT_JUMP:

            skydoom_jump_down = down;

            break;



        case SKYDOOM_INPUT_WEAPON_PISTOL:



            if (down)

            {

                skydoom_pending_weapon =

                    wp_pistol;

            }



            break;





        case SKYDOOM_INPUT_WEAPON_SHOTGUN:



            if (down)

            {

                skydoom_pending_weapon =

                    wp_shotgun;

            }



            break;



        // SKYDOOM_REAL_CHAINGUN_V9

        case SKYDOOM_INPUT_WEAPON_CHAINGUN:



            if (down)

            {

                skydoom_pending_weapon =

                    wp_chaingun;

            }



            break;



        /*

            SKYDOOM_MELEE_SLOT_V10



            Temporary development behaviour:



                key 1 alternates chainsaw <-> fist



            Vanilla DOOM normally favours the chainsaw once owned,

            which would make the fist awkward to test while our dev

            loadout grants every weapon.



            This can return to exact inventory-driven vanilla slot

            behaviour when SkyDoom pickups are implemented.

        */



        case SKYDOOM_INPUT_WEAPON_MELEE:



            if (down)

            {

                if (

                    players[

                        consoleplayer

                    ].readyweapon ==

                    wp_chainsaw

                )

                {

                    skydoom_pending_weapon =

                        wp_fist;

                }

                else if (

                    players[

                        consoleplayer

                    ].weaponowned[

                        wp_chainsaw

                    ]

                )

                {

                    skydoom_pending_weapon =

                        wp_chainsaw;

                }

                else

                {

                    skydoom_pending_weapon =

                        wp_fist;

                }

            }



            break;






        // SKYDOOM_REAL_ROCKET_V11

        case SKYDOOM_INPUT_WEAPON_ROCKET:



            if (down)

            {

                skydoom_pending_weapon =

                    wp_missile;

            }



            break;



        // SKYDOOM_REAL_PLASMA_V12
        case SKYDOOM_INPUT_WEAPON_PLASMA:

            if (down)
            {
                skydoom_pending_weapon =
                    wp_plasma;
            }

            break;



        // SKYDOOM_REAL_BFG_V14

        case SKYDOOM_INPUT_WEAPON_BFG:

            if (down)

            {

                skydoom_pending_weapon =

                    wp_bfg;

            }



            break;


        default:

            break;
    }
}


static void SkyDoom_ConsumeInputRing(void)
{
    SkyDoomInputRing *ring;

    uint32_t head;
    uint32_t tail;

    if (skydoom_state == NULL)
    {
        return;
    }

    ring = &skydoom_state->input;

    for (;;)
    {
        head = ring->head;

        tail = ring->tail;

        if (tail == head)
        {
            break;
        }

        MemoryBarrier();

        SkyDoom_ApplyInputEvent(&ring->events[tail & SKYDOOM_INPUT_RING_MASK]);

        ring->tail = tail + 1u;
    }
}


static void SkyDoom_InjectGuestCommand(player_t *player)
{
    int forward;
    int side;

    int forward_speed;
    int side_speed;

    int attack_active;
    int use_active;

    if (player == NULL || skydoom_state == NULL)
    {
        return;
    }

    if (!skydoom_guest_mode)
    {
        return;
    }

    SkyDoom_ConsumeInputRing();

    /*
        SKYDOOM_COMPLETE_ITEMS_V15_9B1
        Publish the REAL current Doom ammo ceilings and backpack state.
        This prevents Skyrim from continuing to assume the pre-backpack
        200/50/50/300 limits.
    */
    skydoom_state->doom.maxammo_bullets =
        player->maxammo[am_clip];

    skydoom_state->doom.maxammo_shells =
        player->maxammo[am_shell];

    skydoom_state->doom.maxammo_rockets =
        player->maxammo[am_misl];

    skydoom_state->doom.maxammo_cells =
        player->maxammo[am_cell];

    skydoom_state->doom.has_backpack =
        player->backpack ?
            1u :
            0u;

    skydoom_state->doom.commercial_mode =
        gamemode == commercial ?
            1u :
            0u;

    if (!SkyDoom_SkyrimIsFresh() || skydoom_state->skyrim.paused ||
        !skydoom_state->skyrim.in_game)
    {
        SkyDoom_ClearHeldInput();
    }



        /*

        SKYDOOM_APPLY_EXTERNAL_DAMAGE_V7



        The Skyrim side has already measured and repaired the host

        Actor's health loss.



        Chocolate Doom now becomes authoritative for the actual

        player health result.



        NULL inflictor/source means the hidden guest does not invent

        a fake DOOM attacker or knockback direction.

    */



    if (

        skydoom_pending_external_damage > 0 &&

        SkyDoom_SkyrimIsFresh() &&

        !skydoom_state->skyrim.paused &&

        skydoom_state->skyrim.in_game &&

        gamestate == GS_LEVEL &&

        player->mo != NULL

    )

    {

        const int external_damage =

            skydoom_pending_external_damage;





        skydoom_pending_external_damage =

            0;





        P_DamageMobj(

            player->mo,

            NULL,

            NULL,

            external_damage

        );

    }





/*

        SKYDOOM_REAL_WEAPON_SWITCH_V6



        Setting pendingweapon uses Chocolate Doom's normal weapon

        lowering / raising animation and readyweapon transition.

    */



    if (

        skydoom_pending_weapon == wp_pistol ||
        skydoom_pending_weapon == wp_shotgun ||
        skydoom_pending_weapon == wp_chaingun ||
        skydoom_pending_weapon == wp_fist ||
        skydoom_pending_weapon == wp_chainsaw ||
        skydoom_pending_weapon == wp_missile ||
        // SKYDOOM_PLASMA_SWITCH_V12
        skydoom_pending_weapon == wp_plasma ||
        // SKYDOOM_BFG_SWITCH_V14
        skydoom_pending_weapon == wp_bfg

    )

    {

        if (

            player->weaponowned[

                skydoom_pending_weapon

            ]

        )

        {

            player->pendingweapon =

                (weapontype_t)

                    skydoom_pending_weapon;

        }



        skydoom_pending_weapon =

            -1;

    }

    if (skydoom_run_down)
    {
        forward_speed = SKYDOOM_FORWARD_RUN;

        side_speed = SKYDOOM_SIDE_RUN;
    }
    else
    {
        forward_speed = SKYDOOM_FORWARD_WALK;

        side_speed = SKYDOOM_SIDE_WALK;
    }

    forward = 0;
    side = 0;

    if (skydoom_forward_down)
    {
        forward += forward_speed;
    }

    if (skydoom_back_down)
    {
        forward -= forward_speed;
    }

    if (skydoom_right_down)
    {
        side += side_speed;
    }

    if (skydoom_left_down)
    {
        side -= side_speed;
    }

    player->cmd.forwardmove = (signed char) forward;

    player->cmd.sidemove = (signed char) side;


    /*
        Replace native DOOM attack/use input with
        the Skyrim guest-input states.
    */

    player->cmd.buttons &= (byte) ~(SKYDOOM_BT_ATTACK | SKYDOOM_BT_USE);

    attack_active = skydoom_fire_down || skydoom_fire_pulse_tics > 0;

    use_active = skydoom_use_down || skydoom_use_pulse_tics > 0;

    if (attack_active)
    {
        player->cmd.buttons |= (byte) SKYDOOM_BT_ATTACK;
    }

    if (use_active)
    {
        player->cmd.buttons |= (byte) SKYDOOM_BT_USE;
    }

    if (skydoom_fire_pulse_tics > 0)
    {
        skydoom_fire_pulse_tics--;
    }

    if (skydoom_use_pulse_tics > 0)
    {
        skydoom_use_pulse_tics--;
    }
}


/*
    ------------------------------------------------------------
    WEAPON OVERLAY CAPTURE
    ------------------------------------------------------------
*/


void SkyDoom_OverlayCaptureBefore(void)
{
    if (!skydoom_guest_mode || skydoom_state == NULL || gamestate != GS_LEVEL ||
        I_VideoBuffer == NULL)
    {
        skydoom_weapon_capture_ready = 0;

        return;
    }

    memcpy(skydoom_before_weapon, I_VideoBuffer, sizeof(skydoom_before_weapon));

    skydoom_weapon_capture_ready = 1;
}


void SkyDoom_OverlayCaptureAfter(void)
{
    SkyDoomOverlayFrame *overlay;

    const byte *playpal;

    unsigned int i;
    unsigned int x;
    unsigned int y;
    unsigned int index;

    pixel_t before_index;
    pixel_t after_index;

    if (!skydoom_weapon_capture_ready || !skydoom_guest_mode ||
        skydoom_state == NULL || I_VideoBuffer == NULL)
    {
        return;
    }

    skydoom_weapon_capture_ready = 0;

    playpal = (const byte *) W_CacheLumpName("PLAYPAL", PU_CACHE);

    if (playpal == NULL)
    {
        return;
    }

    overlay = &skydoom_state->overlay;


    /*
        Start seqlock write.
    */

    overlay->seq++;

    MemoryBarrier();


    overlay->width = SKYDOOM_OVERLAY_WIDTH;

    overlay->height = SKYDOOM_OVERLAY_HEIGHT;

    overlay->flags = SKYDOOM_OVERLAY_FLAG_WEAPON;


    for (i = 0; i < SKYDOOM_OVERLAY_WIDTH * SKYDOOM_OVERLAY_HEIGHT; ++i)
    {
        before_index = skydoom_before_weapon[i];

        after_index = I_VideoBuffer[i];

        if (before_index != after_index)
        {
            overlay->rgba[i * 4 + 0] = playpal[after_index * 3 + 0];

            overlay->rgba[i * 4 + 1] = playpal[after_index * 3 + 1];

            overlay->rgba[i * 4 + 2] = playpal[after_index * 3 + 2];

            overlay->rgba[i * 4 + 3] = 255;
        }
        else
        {
            overlay->rgba[i * 4 + 0] = 0;

            overlay->rgba[i * 4 + 1] = 0;

            overlay->rgba[i * 4 + 2] = 0;

            overlay->rgba[i * 4 + 3] = 0;
        }
    }


    /*
        SKYDOOM_WEAPON_ALPHA_PINHOLE_REPAIR_V15_9D2

        SkyDoom identifies the weapon by comparing Chocolate Doom's
        framebuffer immediately before and after the player weapon is
        drawn.

        Rarely, an opaque weapon pixel has exactly the same palette
        index as the world pixel behind it.  That pixel then looks
        unchanged and is incorrectly made transparent.

        At native 320x200 resolution this is one tiny pixel; once the
        overlay is scaled in Skyrim it becomes a clearly visible square.

        The original pre-weapon framebuffer is no longer needed after
        the comparison above, so reuse skydoom_before_weapon as a
        temporary immutable 0/1 alpha mask.

        Repair ONLY transparent pixels fully enclosed on all four direct
        sides by pixels that were genuinely detected as opaque.  This
        targets isolated internal holes without expanding the weapon's
        authentic outer silhouette.
    */

    for (
        i = 0;
        i < SKYDOOM_OVERLAY_WIDTH * SKYDOOM_OVERLAY_HEIGHT;
        ++i
    )
    {
        skydoom_before_weapon[i] =
            overlay->rgba[i * 4 + 3] != 0 ?
                1 :
                0;
    }


    for (
        y = 1;
        y + 1 < SKYDOOM_OVERLAY_HEIGHT;
        ++y
    )
    {
        for (
            x = 1;
            x + 1 < SKYDOOM_OVERLAY_WIDTH;
            ++x
        )
        {
            index =
                y * SKYDOOM_OVERLAY_WIDTH +
                x;

            if (
                skydoom_before_weapon[index] == 0 &&
                skydoom_before_weapon[index - 1] != 0 &&
                skydoom_before_weapon[index + 1] != 0 &&
                skydoom_before_weapon[
                    index - SKYDOOM_OVERLAY_WIDTH
                ] != 0 &&
                skydoom_before_weapon[
                    index + SKYDOOM_OVERLAY_WIDTH
                ] != 0
            )
            {
                after_index =
                    I_VideoBuffer[index];

                overlay->rgba[
                    index * 4 + 0
                ] =
                    playpal[
                        after_index * 3 + 0
                    ];

                overlay->rgba[
                    index * 4 + 1
                ] =
                    playpal[
                        after_index * 3 + 1
                    ];

                overlay->rgba[
                    index * 4 + 2
                ] =
                    playpal[
                        after_index * 3 + 2
                    ];

                overlay->rgba[
                    index * 4 + 3
                ] =
                    255;
            }
        }
    }

    overlay->frame_id++;


    MemoryBarrier();

    /*
        End seqlock write.
    */

    overlay->seq++;
}

void SkyDoom_OverlayCaptureHUDBefore(void)
{
    if (
        !skydoom_guest_mode ||
        skydoom_state == NULL ||
        gamestate != GS_LEVEL ||
        I_VideoBuffer == NULL
    )
    {
        skydoom_hud_capture_ready =
            0;

        memset(
            skydoom_hud_rgba,
            0,
            sizeof(
                skydoom_hud_rgba
            )
        );

        return;
    }

    memcpy(
        skydoom_before_hud,
        I_VideoBuffer,
        sizeof(
            skydoom_before_hud
        )
    );

    skydoom_hud_capture_ready =
        1;
}


void SkyDoom_OverlayCaptureHUDAfter(void)
{
    const byte *playpal;

    unsigned int i;
    unsigned int x;
    unsigned int y;

    int changed;
    int min_x;
    int max_x;
    int min_y;
    int max_y;

    pixel_t before_index;
    pixel_t after_index;

    /*
        SKYDOOM_NATIVE_PICKUP_MESSAGE_PERSIST_V15_6_R1

        The first v15.6 capture cleared skydoom_hud_rgba every frame.
        HU_Drawer does not necessarily change the same framebuffer
        pixels every render, so a valid four-second Doom message could
        look like a one-frame flash in Skyrim.

        Doom already owns the correct lifetime via message_on.
        Keep the last genuine captured glyph layer while message_on is
        true, and clear it immediately when Doom turns the message off.
    */

    if (!SkyDoom_HUDMessageActive())
    {
        memset(
            skydoom_hud_rgba,
            0,
            sizeof(
                skydoom_hud_rgba
            )
        );

        skydoom_hud_message_bounds_valid =
            0;

        skydoom_hud_capture_ready =
            0;

        return;
    }

    if (
        !skydoom_hud_capture_ready ||
        !skydoom_guest_mode ||
        skydoom_state == NULL ||
        I_VideoBuffer == NULL
    )
    {
        skydoom_hud_capture_ready =
            0;

        return;
    }

    skydoom_hud_capture_ready =
        0;

    playpal =
        (const byte *)
        W_CacheLumpName(
            "PLAYPAL",
            PU_CACHE
        );

    if (playpal == NULL)
    {
        return;
    }

    changed =
        0;

    min_x =
        SCREENWIDTH;

    max_x =
        -1;

    min_y =
        SCREENHEIGHT;

    max_y =
        -1;

    /*
        First pass: determine whether HU_Drawer produced a fresh
        visible message delta and measure its bounds.
    */
    for (
        i = 0;
        i <
            SCREENWIDTH *
            SCREENHEIGHT;
        ++i
    )
    {
        before_index =
            skydoom_before_hud[i];

        after_index =
            I_VideoBuffer[i];

        if (
            before_index ==
            after_index
        )
        {
            continue;
        }

        x =
            i %
            SCREENWIDTH;

        y =
            i /
            SCREENWIDTH;

        changed =
            1;

        if ((int) x < min_x)
        {
            min_x =
                (int) x;
        }

        if ((int) x > max_x)
        {
            max_x =
                (int) x;
        }

        if ((int) y < min_y)
        {
            min_y =
                (int) y;
        }

        if ((int) y > max_y)
        {
            max_y =
                (int) y;
        }
    }

    /*
        No new pixel delta this frame is NOT the same thing as the
        message expiring.  Doom's message_on flag above is the
        authority, so simply retain the previous captured glyphs.
    */
    if (!changed)
    {
        return;
    }

    memset(
        skydoom_hud_rgba,
        0,
        sizeof(
            skydoom_hud_rgba
        )
    );

    skydoom_hud_message_min_x =
        min_x;

    skydoom_hud_message_max_x =
        max_x;

    skydoom_hud_message_min_y =
        min_y;

    skydoom_hud_message_max_y =
        max_y;

    skydoom_hud_message_bounds_valid =
        1;

    /*
        Second pass: capture the actual Chocolate Doom pixels.
    */
    for (
        i = 0;
        i <
            SCREENWIDTH *
            SCREENHEIGHT;
        ++i
    )
    {
        before_index =
            skydoom_before_hud[i];

        after_index =
            I_VideoBuffer[i];

        if (
            before_index !=
            after_index
        )
        {
            skydoom_hud_rgba[
                i * 4 + 0
            ] =
                playpal[
                    after_index * 3 + 0
                ];

            skydoom_hud_rgba[
                i * 4 + 1
            ] =
                playpal[
                    after_index * 3 + 1
                ];

            skydoom_hud_rgba[
                i * 4 + 2
            ] =
                playpal[
                    after_index * 3 + 2
                ];

            skydoom_hud_rgba[
                i * 4 + 3
            ] =
                255;
        }
    }
}


/*
    ------------------------------------------------------------
    SHARED MEMORY
    ------------------------------------------------------------
*/



/*
    ============================================================
    SKYDOOM REAL DOOM STATUS BAR
    ============================================================

    Chocolate Doom itself generates the status bar from the
    real WAD resources and live DOOM player state.

    SkyDoom copies the genuine bottom 320x32 pixels into the
    same transparent RGBA overlay that already carries the
    first-person weapon.
*/

// SKYDOOM_REAL_STATUS_BAR_V4_BEGIN

void SkyDoom_OverlayCaptureStatusBar(void)
{
    SkyDoomOverlayFrame *overlay;

    const byte *playpal;

    unsigned int x;
    unsigned int y;
    unsigned int index;

    int panel_min_x;
    int panel_max_x;
    int panel_min_y;
    int panel_max_y;

    pixel_t palette_index;


    if (
        !skydoom_guest_mode ||
        skydoom_state == NULL ||
        gamestate != GS_LEVEL ||
        I_VideoBuffer == NULL
    )
    {
        return;
    }


    if (
        SCREENWIDTH != SKYDOOM_OVERLAY_WIDTH ||
        SCREENHEIGHT != SKYDOOM_OVERLAY_HEIGHT
    )
    {
        return;
    }


    /*
        Force Chocolate Doom to draw its genuine normal
        status bar into the hidden guest framebuffer.

        fullscreen = false
        refresh    = true
    */

    ST_Drawer(
        false,
        true
    );


    playpal =
        (const byte *)
        W_CacheLumpName(
            "PLAYPAL",
            PU_CACHE
        );


    if (playpal == NULL)
    {
        return;
    }


    overlay =
        &skydoom_state->overlay;


    /*
        Seqlock: begin write.
    */

    overlay->seq++;

    MemoryBarrier();

    /*
        ========================================================
        SKYDOOM WEAPON / HUD VERTICAL ALIGNMENT
        ========================================================

        The weapon is captured from Chocolate Doom's hidden
        fullscreen view.

        The authentic status bar occupies the bottom 32 pixels.

        Move ONLY the weapon overlay down 12 native DOOM pixels
        before copying the status bar.

        This closes the visible gap between the player's hand
        and the top edge of the DOOM HUD without moving the HUD.
    */

    // SKYDOOM_WEAPON_VERTICAL_ALIGN_V4

    memmove(
        &overlay->rgba[
            12 *
            SKYDOOM_OVERLAY_WIDTH *
            4
        ],
        &overlay->rgba[
            0
        ],
        (
            SCREENHEIGHT -
            ST_HEIGHT -
            12
        ) *
        SKYDOOM_OVERLAY_WIDTH *
        4
    );


    /*
        The top 12 rows are now vacant after moving the weapon
        image downward, so make them fully transparent.
    */

    memset(
        &overlay->rgba[
            0
        ],
        0,
        12 *
        SKYDOOM_OVERLAY_WIDTH *
        4
    );



    overlay->width =
        SKYDOOM_OVERLAY_WIDTH;

    overlay->height =
        SKYDOOM_OVERLAY_HEIGHT;

    overlay->flags |=
        SKYDOOM_OVERLAY_FLAG_WEAPON;


    /*
        Copy the genuine 320x32 DOOM status bar only.

        Everything above this remains the transparent
        weapon overlay captured earlier in the frame.
    */

    for (
        y = SCREENHEIGHT - ST_HEIGHT;
        y < SCREENHEIGHT;
        ++y
    )
    {
        for (
            x = 0;
            x < SCREENWIDTH;
            ++x
        )
        {
            index =
                y * SCREENWIDTH +
                x;


            palette_index =
                I_VideoBuffer[
                    index
                ];


            overlay->rgba[
                index * 4 + 0
            ] =
                playpal[
                    palette_index * 3 + 0
                ];


            overlay->rgba[
                index * 4 + 1
            ] =
                playpal[
                    palette_index * 3 + 1
                ];


            overlay->rgba[
                index * 4 + 2
            ] =
                playpal[
                    palette_index * 3 + 2
                ];


            overlay->rgba[
                index * 4 + 3
            ] =
                255;
        }
    }


        /*
        SKYDOOM_NATIVE_PICKUP_MESSAGES_V15_6

        The existing weapon image has already been aligned and the
        genuine status bar has already been copied.

        Merge only pixels changed by HU_Drawer.  This restores the
        authentic top-left Doom pickup text after the existing
        12-pixel weapon-only clear, without bringing across the hidden
        Doom world.
    */
    /*
        SKYDOOM_NATIVE_PICKUP_MESSAGE_PERSIST_V15_6_R1

        The reference/source-port presentation requested by Caffs
        uses a compact black backing panel behind the red Doom text.

        Doom still supplies every glyph pixel and the lifetime.
        SkyDoom adds only this presentation rectangle.

        Padding:
          2 native pixels left/top
          3 native pixels right
          2 native pixels bottom
    */
    if (
        skydoom_hud_message_bounds_valid
    )
    {
        panel_min_x =
            skydoom_hud_message_min_x -
            2;

        panel_max_x =
            skydoom_hud_message_max_x +
            3;

        panel_min_y =
            skydoom_hud_message_min_y -
            2;

        panel_max_y =
            skydoom_hud_message_max_y +
            2;

        if (panel_min_x < 0)
        {
            panel_min_x =
                0;
        }

        if (panel_min_y < 0)
        {
            panel_min_y =
                0;
        }

        if (
            panel_max_x >=
            SKYDOOM_OVERLAY_WIDTH
        )
        {
            panel_max_x =
                SKYDOOM_OVERLAY_WIDTH -
                1;
        }

        if (
            panel_max_y >=
            SKYDOOM_OVERLAY_HEIGHT
        )
        {
            panel_max_y =
                SKYDOOM_OVERLAY_HEIGHT -
                1;
        }

        for (
            y =
                (unsigned int)
                panel_min_y;
            y <=
                (unsigned int)
                panel_max_y;
            ++y
        )
        {
            for (
                x =
                    (unsigned int)
                    panel_min_x;
                x <=
                    (unsigned int)
                    panel_max_x;
                ++x
            )
            {
                index =
                    y *
                    SKYDOOM_OVERLAY_WIDTH +
                    x;

                overlay->rgba[
                    index * 4 + 0
                ] =
                    0;

                overlay->rgba[
                    index * 4 + 1
                ] =
                    0;

                overlay->rgba[
                    index * 4 + 2
                ] =
                    0;

                overlay->rgba[
                    index * 4 + 3
                ] =
                    255;
            }
        }
    }

    for (
        index = 0;
        index <
            SKYDOOM_OVERLAY_WIDTH *
            SKYDOOM_OVERLAY_HEIGHT;
        ++index
    )
    {
        if (
            skydoom_hud_rgba[
                index * 4 + 3
            ] !=
            0
        )
        {
            overlay->rgba[
                index * 4 + 0
            ] =
                skydoom_hud_rgba[
                    index * 4 + 0
                ];

            overlay->rgba[
                index * 4 + 1
            ] =
                skydoom_hud_rgba[
                    index * 4 + 1
                ];

            overlay->rgba[
                index * 4 + 2
            ] =
                skydoom_hud_rgba[
                    index * 4 + 2
                ];

            overlay->rgba[
                index * 4 + 3
            ] =
                255;
        }
    }

overlay->frame_id++;


    MemoryBarrier();


    /*
        Seqlock: end write.
    */

    overlay->seq++;
}

// SKYDOOM_REAL_STATUS_BAR_V4_END

int SkyDoom_SharedInit(void)
{
    DWORD creation_status;
    int newly_created;

    skydoom_guest_mode = M_ParmExists("-skydoomguest");

    skydoom_mapping =
        CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
                           sizeof(SkyDoomSharedState), SKYDOOM_MAPPING_NAME);

    if (skydoom_mapping == NULL)
    {
        return 0;
    }

    creation_status = GetLastError();

    newly_created = creation_status != ERROR_ALREADY_EXISTS;

    skydoom_state = (SkyDoomSharedState *) MapViewOfFile(
        skydoom_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SkyDoomSharedState));

    if (skydoom_state == NULL)
    {
        CloseHandle(skydoom_mapping);

        skydoom_mapping = NULL;

        return 0;
    }

    SkyDoom_InitializeProtocolIfNeeded(newly_created);

    SkyDoom_ClearHeldInput();

    skydoom_state->doom.running = 1;

    skydoom_state->doom.pid = GetCurrentProcessId();

    skydoom_state->doom.guest_mode = skydoom_guest_mode ? 1u : 0u;

    skydoom_state->doom.heartbeat_ms = GetTickCount64();

    atexit(SkyDoom_SharedShutdown);

    return 1;
}






// SKYDOOM_REAL_PISTOL_DAMAGE_V5

void SkyDoom_ReportPistolShotDamage(int damage)

{

    if (

        !skydoom_guest_mode ||

        skydoom_state == NULL ||

        damage <= 0

    )

    {

        return;

    }



    /*

        Publish the accumulated damage first.



        Interlocked operations provide the cross-process memory

        ordering we need before the shot serial becomes visible.

    */



    InterlockedAdd64(

        (volatile LONG64 *)

            &skydoom_state->

                doom.

                pistol_damage_total,

        (LONG64) damage

    );





    /*

        Increment this last.



        When Skyrim sees a new serial it is therefore guaranteed

        that the corresponding damage total was published first.

    */



    InterlockedIncrement64(

        (volatile LONG64 *)

            &skydoom_state->

                doom.

                pistol_shot_serial

    );

}







// SKYDOOM_SHOTGUN_COMBAT_RING_V6



static void SkyDoom_PushCombatEvent(

    uint16_t type,

    uint16_t weapon,

    int32_t damage,

    int32_t angle_offset,

    int32_t slope_offset

)

{

    SkyDoomCombatRing *ring;

    SkyDoomCombatEvent *event;



    uint32_t head;

    uint32_t tail;





    if (

        skydoom_state == NULL ||

        !skydoom_guest_mode

    )

    {

        return;

    }





    ring =

        &skydoom_state->combat;





    head =

        ring->head;



    tail =

        ring->tail;





    /*

        Single producer (Chocolate Doom) and single consumer

        (Skyrim).



        If Skyrim ever falls a full ring behind, drop only the new

        event rather than overwrite an unread pellet.

    */



    if (

        (uint32_t) (head - tail) >=

        SKYDOOM_COMBAT_RING_ENTRIES

    )

    {

        ring->dropped++;



        return;

    }





    event =

        &ring->events[

            head &

            SKYDOOM_COMBAT_RING_MASK

        ];





    event->type =

        type;



    event->weapon =

        weapon;



    event->damage =

        damage;



    event->angle_offset =

        angle_offset;



    event->slope_offset =

        slope_offset;





    MemoryBarrier();





    ring->head =

        head + 1u;

}





void SkyDoom_ReportShotgunPellet(

    int damage,

    int32_t angle_offset,

    int32_t slope_offset

)

{

    if (

        damage <= 0

    )

    {

        return;

    }





    SkyDoom_PushCombatEvent(

        SKYDOOM_COMBAT_EVENT_PELLET,

        SKYDOOM_COMBAT_WEAPON_SHOTGUN,

        damage,

        angle_offset,

        slope_offset

    );

}







void SkyDoom_ReportChaingunBullet(

    int damage,

    int32_t angle_offset,

    int32_t slope_offset

)

{

    if (damage <= 0)

    {

        return;

    }





    SkyDoom_PushCombatEvent(

        SKYDOOM_COMBAT_EVENT_PELLET,

        SKYDOOM_COMBAT_WEAPON_CHAINGUN,

        damage,

        angle_offset,

        slope_offset

    );

}







void SkyDoom_ReportFistAttack(

    int damage,

    int32_t angle_offset

)

{

    if (damage <= 0)

    {

        return;

    }





    SkyDoom_PushCombatEvent(

        SKYDOOM_COMBAT_EVENT_MELEE,

        SKYDOOM_COMBAT_WEAPON_FIST,

        damage,

        angle_offset,

        0

    );

}





void SkyDoom_ReportChainsawAttack(

    int damage,

    int32_t angle_offset

)

{

    if (damage <= 0)

    {

        return;

    }





    SkyDoom_PushCombatEvent(

        SKYDOOM_COMBAT_EVENT_MELEE,

        SKYDOOM_COMBAT_WEAPON_CHAINSAW,

        damage,

        angle_offset,

        0

    );

}







int SkyDoom_SharedActive(void)
{
    return skydoom_state != NULL;
}

/* SKYDOOM_GUEST_NATIVE_PICKUP_ISOLATION_V15_9C1 */
int SkyDoom_GuestModeActive(void)
{
    return skydoom_guest_mode != 0;
}

void SkyDoom_ReportRocketFired(void)

{

    SkyDoom_PushCombatEvent(

        SKYDOOM_COMBAT_EVENT_ROCKET_FIRED,

        SKYDOOM_COMBAT_WEAPON_ROCKET,

        0,

        0,

        0

    );

}





void SkyDoom_ReportPlasmaFired(void)
{
    SkyDoom_PushCombatEvent(
        SKYDOOM_COMBAT_EVENT_PLASMA_FIRED,
        SKYDOOM_COMBAT_WEAPON_PLASMA,
        0,
        0,
        0
    );
}

void SkyDoom_ReportBFGFired(void)

{

    SkyDoom_PushCombatEvent(

        SKYDOOM_COMBAT_EVENT_BFG_FIRED,

        SKYDOOM_COMBAT_WEAPON_BFG,

        0,

        0,

        0

    );

}


void SkyDoom_ReportBFGDamageRequest(
    int32_t request_id,
    int32_t spray
)
{
    int damage;
    int j;

    if (request_id <= 0)
    {
        return;
    }

    if (spray)
    {
        /*
            Exact A_BFGSpray damage:
              15 independent rolls of
              (P_Random() & 7) + 1.

            Range: 15..120.
        */
        damage = 0;

        for (j = 0; j < 15; ++j)
        {
            damage +=
                (P_Random() & 7) + 1;
        }

        SkyDoom_PushCombatEvent(
            SKYDOOM_COMBAT_EVENT_ROCKET_DAMAGE,
            SKYDOOM_COMBAT_WEAPON_BFG,
            damage,
            request_id,
            2
        );

        return;
    }

    /*
        SKYDOOM_BFG_DAMAGE_RNG_V14_3

        Exact vanilla missile direct-hit rule:
          ((P_Random() % 8) + 1) * info->damage

        MT_BFG info->damage == 100.
        Genuine direct damage: 100..800 in steps of 100.
    */
    damage =
        ((P_Random() % 8) + 1) *
        100;

    SkyDoom_PushCombatEvent(
        SKYDOOM_COMBAT_EVENT_ROCKET_DAMAGE,
        SKYDOOM_COMBAT_WEAPON_BFG,
        damage,
        request_id,
        1
    );
}

void SkyDoom_ReportPlasmaDamageRequest(

    int32_t request_id

)

{

    int damage;



    if (request_id <= 0)

    {

        return;

    }



    /*

        SKYDOOM_PLASMA_DAMAGE_V13



        Exact vanilla DOOM missile direct-hit rule:

            ((P_Random() % 8) + 1) * info->damage



        MT_PLASMA info->damage == 5.

        Therefore the genuine result is:

            5, 10, 15, 20, 25, 30, 35 or 40.

    */

    damage =

        ((P_Random() % 8) + 1) *

        5;



    SkyDoom_PushCombatEvent(

        SKYDOOM_COMBAT_EVENT_ROCKET_DAMAGE,

        SKYDOOM_COMBAT_WEAPON_PLASMA,

        damage,

        request_id,

        0

    );

}



void SkyDoom_ReportRocketDamageRequest(

    int32_t request_id

)

{

    int damage;



    if (request_id <= 0)

    {

        return;

    }





    /*

        Exact vanilla DOOM missile direct-hit damage rule.



        MT_ROCKET info->damage == 20.



        Missile collision does:



            ((P_Random() % 8) + 1) * info->damage

    */



    damage =

        ((P_Random() % 8) + 1) *

        20;





    SkyDoom_PushCombatEvent(

        SKYDOOM_COMBAT_EVENT_ROCKET_DAMAGE,

        SKYDOOM_COMBAT_WEAPON_ROCKET,

        damage,

        request_id,

        0

    );

}



void SkyDoom_SharedUpdate(void)
{
    player_t *player;

    if (skydoom_state == NULL)
    {
        return;
    }

    player = &players[consoleplayer];



    /*

        SKYDOOM_DEV_SHOTGUN_LOADOUT_V6



        Temporary testing support only.



        This runs once per map, not every frame, so firing still

        genuinely consumes Chocolate Doom shells.

    */



    if (

        skydoom_guest_mode &&

        gamestate == GS_LEVEL &&

        (

            skydoom_dev_loadout_episode != gameepisode ||

            skydoom_dev_loadout_map != gamemap

        )

    )

    {

        player->weaponowned[

            wp_shotgun

        ] =

            1;



        /*

            SKYDOOM_DEV_CHAINGUN_LOADOUT_V9



            Temporary testing support.



            This does not refill bullets continuously. It runs only

            when the existing once-per-map dev-loadout block runs.

        */



        player->weaponowned[

            wp_chaingun

        ] =

            1;



        /*

            SKYDOOM_DEV_CHAINSAW_LOADOUT_V10



            Temporary only.



            The chainsaw will eventually come from genuine bridged

            inventory / pickup state instead.

        */



        player->weaponowned[

            wp_chainsaw

        ] =

            1;



        /*

            SKYDOOM_DEV_ROCKET_LOADOUT_V11



            Temporary testing support only.



            Real pickup/inventory bridging will replace this later.

        */



        player->weaponowned[

            wp_missile

        ] =

            1;





        if (

            player->ammo[

                am_misl

            ] <

            50

        )

        {

            player->ammo[

                am_misl

            ] =

                50;

        }





        /*
            SKYDOOM_DEV_PLASMA_LOADOUT_V12

            Temporary testing support only.
            Real pickup/inventory bridging will replace this later.
        */

        player->weaponowned[
            wp_plasma
        ] =
            1;

        /* SKYDOOM_DEV_BFG_LOADOUT_V14 */
        player->weaponowned[wp_bfg] = 1;

        if (
            player->ammo[
                am_cell
            ] <
            300
        )
        {
            player->ammo[
                am_cell
            ] =
                300;
        }


        if (

            player->ammo[

                am_clip

            ] <

            200

        )

        {

            player->ammo[

                am_clip

            ] =

                200;

        }



        if (

            player->ammo[

                am_shell

            ] <

            50

        )

        {

            player->ammo[

                am_shell

            ] =

                50;

        }



        skydoom_dev_loadout_episode =

            gameepisode;



        skydoom_dev_loadout_map =

            gamemap;

    }

    SkyDoom_InjectGuestCommand(player);

    skydoom_state->doom.running = 1;

    skydoom_state->doom.pid = GetCurrentProcessId();

    skydoom_state->doom.guest_mode = skydoom_guest_mode ? 1u : 0u;

    skydoom_state->doom.in_level = gamestate == GS_LEVEL ? 1u : 0u;

    skydoom_state->doom.episode = (uint32_t) gameepisode;

    skydoom_state->doom.map = (uint32_t) gamemap;

    skydoom_state->doom.health = player->health;

    skydoom_state->doom.armor = player->armorpoints;

    skydoom_state->doom.weapon = (int32_t) player->readyweapon;

    skydoom_state->doom.ammo_bullets = player->ammo[am_clip];

    skydoom_state->doom.ammo_shells = player->ammo[am_shell];

    skydoom_state->doom.ammo_rockets = player->ammo[am_misl];

    skydoom_state->doom.ammo_cells = player->ammo[am_cell];

    skydoom_state->doom.accepted_forward = (int32_t) player->cmd.forwardmove;

    skydoom_state->doom.accepted_side = (int32_t) player->cmd.sidemove;

    skydoom_state->doom.accepted_buttons = (uint32_t) player->cmd.buttons;

    skydoom_state->doom.heartbeat_ms = GetTickCount64();

    skydoom_state->doom.tick_counter++;
}


void SkyDoom_SharedShutdown(void)
{
    SkyDoom_ClearHeldInput();

    if (skydoom_state != NULL)
    {
        skydoom_state->doom.running = 0;

        skydoom_state->doom.accepted_forward = 0;

        skydoom_state->doom.accepted_side = 0;

        skydoom_state->doom.accepted_buttons = 0;

        skydoom_state->doom.heartbeat_ms = GetTickCount64();

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






void SkyDoom_ReportPistolShotDamage(int damage)

{

    (void) damage;

}







void SkyDoom_ReportShotgunPellet(

    int damage,

    int32_t angle_offset,

    int32_t slope_offset

)

{

    (void) damage;

    (void) angle_offset;

    (void) slope_offset;

}







void SkyDoom_ReportChaingunBullet(

    int damage,

    int32_t angle_offset,

    int32_t slope_offset

)

{

    (void) damage;

    (void) angle_offset;

    (void) slope_offset;

}







void SkyDoom_ReportFistAttack(

    int damage,

    int32_t angle_offset

)

{

    (void) damage;

    (void) angle_offset;

}





void SkyDoom_ReportChainsawAttack(

    int damage,

    int32_t angle_offset

)

{

    (void) damage;

    (void) angle_offset;

}







int SkyDoom_SharedActive(void)
{
    return 0;
}

/* SKYDOOM_GUEST_NATIVE_PICKUP_ISOLATION_V15_9C1 */
int SkyDoom_GuestModeActive(void)
{
    return 0;
}

void SkyDoom_ReportRocketFired(void)

{

}





void SkyDoom_ReportPlasmaFired(void)
{
}

void SkyDoom_ReportBFGFired(void)

{

}


void SkyDoom_ReportBFGDamageRequest(
    int32_t request_id,
    int32_t spray
)
{
    (void) request_id;
    (void) spray;
}

void SkyDoom_ReportPlasmaDamageRequest(

    int32_t request_id

)

{

    (void) request_id;

}



void SkyDoom_ReportRocketDamageRequest(

    int32_t request_id

)

{

    (void) request_id;

}



void SkyDoom_SharedUpdate(void)
{
}


void SkyDoom_SharedShutdown(void)
{
}


void SkyDoom_OverlayCaptureBefore(void)
{
}


void SkyDoom_OverlayCaptureAfter(void)
{
}

void SkyDoom_OverlayCaptureStatusBar(void)
{
}

void SkyDoom_OverlayCaptureHUDBefore(void)
{
}


void SkyDoom_OverlayCaptureHUDAfter(void)
{
}


#endif


