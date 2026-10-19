// ============================================================================
//  DATACENTER OPS
//  A small 3D datacenter management game in C++ / raylib
// ----------------------------------------------------------------------------
//  Build (Linux/macOS):   g++ -O2 main.cpp -o dcops -lraylib -lm -lpthread -ldl
//  Build (Windows/MSVC):  cl main.cpp raylib.lib /std:c++17
//  (raylib must be installed: https://www.raylib.com/)
// ============================================================================

#include "raylib.h"
#include "raymath.h"

#include <vector>
#include <string>
#include <cmath>
#include <cstdlib>

// ---------------------------------------------------------------- tuning ----
static constexpr int   SLOTS_PER_RACK = 6;
static constexpr float ROOM_HALF_X    = 10.0f;
static constexpr float ROOM_HALF_Z    = 7.0f;
static constexpr float ROOM_HEIGHT    = 4.0f;

static constexpr float RACK_W         = 1.4f;
static constexpr float RACK_H         = 2.6f;
static constexpr float RACK_D         = 1.0f;

static constexpr float PLAYER_RADIUS  = 0.35f;
static constexpr float EYE_HEIGHT     = 1.65f;
static constexpr float MOVE_SPEED     = 4.2f;
static constexpr float RUN_MULT       = 1.7f;
static constexpr float MOUSE_SENS     = 0.0022f;
static constexpr float INTERACT_RANGE = 3.2f;

static constexpr float ORDER_COST     = 125.0f;  // $ for a crate of chips
static constexpr int   ORDER_AMOUNT   = 5;
static constexpr float ORDER_TIME     = 2.5f;    // delivery delay

static float Rand01()   { return rand() / (float)RAND_MAX; }
static float RandDecay(){ return 0.0015f + 0.0025f * Rand01(); } // ~250..650 s lifetime

// ------------------------------------------------------------- entities ----
struct Chip {
    bool  present = true;
    float health  = 1.0f;     // 1 = fresh, 0 = dead
    float decay   = 0.0025f;  // health lost per second

    bool failed() const { return  present && health <= 0.0f; }
    bool ok()     const { return  present && health >  0.0f; }
};

struct Rack {
    Vector3 base{};           // floor-level centre
    float   facing = 1.0f;    // +1: front face points +Z, -1: points -Z
    Chip    slots[SLOTS_PER_RACK];
};

// -------------------------------------------------------------- helpers ----
static Vector3 SlotCenter(const Rack& r, int i)
{
    return { r.base.x,
             0.55f + i * 0.34f,
             r.base.z + r.facing * (RACK_D * 0.5f + 0.06f) };
}

static BoundingBox SlotBox(const Rack& r, int i)
{
    Vector3 c = SlotCenter(r, i);
    return { { c.x - 0.34f, c.y - 0.14f, c.z - 0.07f },
             { c.x + 0.34f, c.y + 0.14f, c.z + 0.07f } };
}

static void ResolveRackCollision(const Rack& r, Vector3& p)
{
    const float ex = RACK_W * 0.5f + PLAYER_RADIUS;
    const float ez = RACK_D * 0.5f + PLAYER_RADIUS;
    const float dx = p.x - r.base.x;
    const float dz = p.z - r.base.z;
    const float px = ex - fabsf(dx);
    const float pz = ez - fabsf(dz);
    if (px > 0.0f && pz > 0.0f) {
        if (px < pz) p.x += (dx >= 0.0f) ? px : -px;
        else         p.z += (dz >= 0.0f) ? pz : -pz;
    }
}

// ================================================================== main ===
int main()
{
    SetExitKey(KEY_NULL);                       // we handle ESC ourselves
    InitWindow(1280, 720, "Datacenter Ops");
    SetTargetFPS(144);
    DisableCursor();

    // ---------------------------------------------------------- world ----
    std::vector<Rack> racks;
    const float xs[] = { -7.5f, -4.5f, -1.5f, 1.5f, 4.5f, 7.5f };
    for (float x : xs) {
        Rack a; a.base = { x, 0.0f, -3.5f }; a.facing =  1.0f;  // faces corridor
        Rack b; b.base = { x, 0.0f,  3.5f }; b.facing = -1.0f;
        racks.push_back(a);
        racks.push_back(b);
    }
    for (auto& r : racks)
        for (auto& c : r.slots) {
            c.present = true;
            c.health  = 0.35f + 0.65f * Rand01();     // some already worn
            c.decay   = RandDecay();
        }

    const int totalSlots = (int)racks.size() * SLOTS_PER_RACK;

    // ----------------------------------------------------- game state ----
    Vector3 player = { 0.0f, 0.0f, 0.0f };
    float   yaw    = 0.0f;      // 0 -> looking towards +Z
    float   pitch  = 0.0f;

    float money        = 600.0f;
    int   spares       = 8;
    int   pendingChips = 0;
    float pendingTimer = 0.0f;

    int   hoverRack = -1, hoverSlot = -1;
    float holdProgress = 0.0f;
    bool  holdLock     = false;   // must release E between actions

    float playtime = 0.0f;
    bool  paused   = false;

    std::string message  = "Welcome to Datacenter Ops. Hold [E] to service a chip.";
    float       msgTimer = 6.0f;
    auto say = [&](const std::string& s, float t = 3.0f) { message = s; msgTimer = t; };

    // ------------------------------------------------------- main loop ----
    while (!WindowShouldClose())
    {
        float dt = GetFrameTime();
        if (dt > 0.1f) dt = 0.1f;                 // clamp after stalls

        // ---- pause -------------------------------------------------------
        if (IsKeyPressed(KEY_ESCAPE)) {
            paused = !paused;
            if (paused) EnableCursor(); else DisableCursor();
        }
        if (paused && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            paused = false;
            DisableCursor();
        }

        // ---- look --------------------------------------------------------
        if (!paused) {
            Vector2 md = GetMouseDelta();
            yaw   -= md.x * MOUSE_SENS;
            pitch -= md.y * MOUSE_SENS;
            pitch  = Clamp(pitch, -1.45f, 1.45f);
        }

        const Vector3 fwd   = { sinf(yaw) * cosf(pitch), sinf(pitch), cosf(yaw) * cosf(pitch) };
        const Vector3 flat  = Vector3Normalize({ fwd.x, 0.0f, fwd.z });
        const Vector3 right = Vector3Normalize(Vector3CrossProduct(flat, { 0.0f, 1.0f, 0.0f }));

        // ---- move --------------------------------------------------------
        if (!paused) {
            Vector3 move = { 0, 0, 0 };
            if (IsKeyDown(KEY_W)) move = Vector3Add(move, flat);
            if (IsKeyDown(KEY_S)) move = Vector3Subtract(move, flat);
            if (IsKeyDown(KEY_D)) move = Vector3Add(move, right);
            if (IsKeyDown(KEY_A)) move = Vector3Subtract(move, right);

            if (Vector3Length(move) > 0.0001f) {
                move = Vector3Normalize(move);
                float spd = MOVE_SPEED * (IsKeyDown(KEY_LEFT_SHIFT) ? RUN_MULT : 1.0f);
                player = Vector3Add(player, Vector3Scale(move, spd * dt));
            }
            for (auto& r : racks) ResolveRackCollision(r, player);
            player.x = Clamp(player.x, -ROOM_HALF_X + PLAYER_RADIUS, ROOM_HALF_X - PLAYER_RADIUS);
            player.z = Clamp(player.z, -ROOM_HALF_Z + PLAYER_RADIUS, ROOM_HALF_Z - PLAYER_RADIUS);
        }

        // ---- camera ------------------------------------------------------
        const Vector3 eye = { player.x, EYE_HEIGHT, player.z };
        Camera3D cam = { eye, Vector3Add(eye, fwd), { 0.0f, 1.0f, 0.0f }, 65.0f, CAMERA_PERSPECTIVE };

        // ---- pick the slot under the crosshair ---------------------------
        hoverRack = -1; hoverSlot = -1;
        if (!paused) {
            Ray ray = GetMouseRay({ GetScreenWidth() * 0.5f, GetScreenHeight() * 0.5f }, cam);
            float bestT = INTERACT_RANGE;
            for (int ri = 0; ri < (int)racks.size(); ++ri)
                for (int si = 0; si < SLOTS_PER_RACK; ++si) {
                    RayCollision hit = GetRayCollisionBox(ray, SlotBox(racks[ri], si));
                    if (hit.hit && hit.distance < bestT) {
                        bestT = hit.distance;
                        hoverRack = ri;
                        hoverSlot = si;
                    }
                }
        }

        // ---- hold-to-interact -------------------------------------------
        if (!IsKeyDown(KEY_E)) holdLock = false;

        if (hoverRack >= 0 && !paused) {
            Chip& c = racks[hoverRack].slots[hoverSlot];

            float need   = 0.0f;
            bool  canAct = false;

            if (c.failed())          { need = 1.2f; canAct = true; }
            else if (!c.present)     { need = 0.7f; canAct = (spares > 0); }

            if (canAct && IsKeyDown(KEY_E) && !holdLock) {
                holdProgress += dt;
                if (holdProgress >= need) {
                    holdProgress = 0.0f;
                    holdLock     = true;
                    if (c.failed()) {                      // pull the dead chip
                        c.present = false;
                        c.health  = 0.0f;
                        say("Failed chip removed - slot is empty.", 2.5f);
                    } else {                               // install a spare
                        c.present = true;
                        c.health  = 1.0f;
                        c.decay   = RandDecay();
                        spares--;
                        say("New chip installed.", 2.5f);
                    }
                }
            } else {
                holdProgress = 0.0f;
            }
        } else {
            holdProgress = 0.0f;
        }

        // ---- ordering spares --------------------------------------------
        if (!paused) {
            if (IsKeyPressed(KEY_Q)) {
                if (pendingChips > 0)        say("An order is already in transit!", 2.0f);
                else if (money < ORDER_COST) say("Not enough money for an order.", 2.0f);
                else {
                    money       -= ORDER_COST;
                    pendingChips = ORDER_AMOUNT;
                    pendingTimer = ORDER_TIME;
                    say(TextFormat("Ordered %d chips ($%.0f).", ORDER_AMOUNT, ORDER_COST), 2.5f);
                }
            }
            if (pendingChips > 0) {
                pendingTimer -= dt;
                if (pendingTimer <= 0.0f) {
                    spares += pendingChips;
                    say(TextFormat("Delivery arrived: +%d chips.", pendingChips), 2.5f);
                    pendingChips = 0;
                }
            }
        }

        // ---- simulation --------------------------------------------------
        int working = 0, failedTotal = 0;
        if (!paused) {
            playtime += dt;
            for (auto& r : racks) {
                int rackFailed = 0;
                for (auto& c : r.slots) if (c.failed()) rackFailed++;
                const float heatMul = 1.0f + 0.35f * rackFailed;   // heat cascade

                for (auto& c : r.slots) {
                    if (c.ok()) {
                        c.health -= c.decay * heatMul * dt;
                        if (c.health < 0.0f) c.health = 0.0f;
                    }
                    if (c.ok())          working++;
                    else if (c.failed()) failedTotal++;
                }
            }
            money += (working * 0.55f - failedTotal * 2.0f) * dt;
            if (money < 0.0f) money = 0.0f;
        }

        if (msgTimer > 0.0f) msgTimer -= dt;

        // ================================================== rendering ====
        BeginDrawing();
        ClearBackground({ 10, 12, 16, 255 });

        BeginMode3D(cam);
        {
            // floor + grid
            DrawPlane({ 0.0f, -0.02f, 0.0f },
                      { ROOM_HALF_X * 2.0f, ROOM_HALF_Z * 2.0f }, { 28, 31, 38, 255 });
            DrawGrid(40, 1.0f);

            // walls
            const Color wall = { 34, 38, 46, 255 };
            DrawCubeV({ -ROOM_HALF_X - 0.1f, ROOM_HEIGHT * 0.5f, 0.0f },
                      { 0.2f, ROOM_HEIGHT, ROOM_HALF_Z * 2.0f }, wall);
            DrawCubeV({  ROOM_HALF_X + 0.1f, ROOM_HEIGHT * 0.5f, 0.0f },
                      { 0.2f, ROOM_HEIGHT, ROOM_HALF_Z * 2.0f }, wall);
            DrawCubeV({ 0.0f, ROOM_HEIGHT * 0.5f, -ROOM_HALF_Z - 0.1f },
                      { ROOM_HALF_X * 2.0f, ROOM_HEIGHT, 0.2f }, wall);
            DrawCubeV({ 0.0f, ROOM_HEIGHT * 0.5f,  ROOM_HALF_Z + 0.1f },
                      { ROOM_HALF_X * 2.0f, ROOM_HEIGHT, 0.2f }, wall);
            DrawCubeV({ 0.0f, ROOM_HEIGHT + 0.1f, 0.0f },
                      { ROOM_HALF_X * 2.0f, 0.2f, ROOM_HALF_Z * 2.0f }, { 20, 22, 27, 255 });

            // ceiling light strips
            for (int i = -1; i <= 1; ++i)
                DrawCubeV({ i * 5.0f, ROOM_HEIGHT - 0.08f, 0.0f },
                          { 0.35f, 0.06f, ROOM_HALF_Z * 1.4f }, { 210, 220, 235, 255 });

            // racks
            for (auto& r : racks) {
                const Vector3 rc = { r.base.x, RACK_H * 0.5f, r.base.z };
                DrawCubeV(rc, { RACK_W, RACK_H, RACK_D }, { 38, 42, 50, 255 });
                DrawCubeWiresV(rc, { RACK_W, RACK_H, RACK_D }, { 62, 70, 84, 255 });

                // recessed front panel
                DrawCubeV({ r.base.x, RACK_H * 0.5f, r.base.z + r.facing * (RACK_D * 0.5f + 0.005f) },
                          { RACK_W * 0.92f, RACK_H * 0.94f, 0.02f }, { 21, 24, 30, 255 });

                for (int i = 0; i < SLOTS_PER_RACK; ++i) {
                    const Chip& c  = r.slots[i];
                    const Vector3 sc = SlotCenter(r, i);

                    if (!c.present) {
                        DrawCubeV(sc, { 0.68f, 0.26f, 0.06f }, { 13, 14, 17, 255 });
                        DrawCubeWiresV(sc, { 0.68f, 0.26f, 0.06f }, { 46, 50, 58, 255 });
                    } else {
                        Color col;
                        if (c.failed()) {
                            const float blink = 0.5f + 0.5f * sinf(playtime * 12.0f);
                            col = { (unsigned char)(110 + 145 * blink), 28, 28, 255 };
                        } else {
                            const float h = c.health;                       // 1 -> green, 0 -> red
                            col = { (unsigned char)(40  + 215 * (1.0f - h)),
                                    (unsigned char)(30  + 225 * h),
                                    (unsigned char)(50  + 20  * h), 255 };
                        }
                        const Vector3 ssz = { 0.68f, 0.26f, 0.10f };
                        DrawCubeV(sc, ssz, col);
                        DrawCubeWiresV(sc, ssz, { 0, 0, 0, 130 });

                        const Vector3 led = { sc.x + 0.26f, sc.y, sc.z + r.facing * 0.07f };
                        DrawCubeV(led, { 0.05f, 0.05f, 0.03f },
                                  c.failed() ? RED : (c.health < 0.35f ? YELLOW : LIME));
                    }
                }
            }

            // hovered slot highlight
            if (hoverRack >= 0) {
                const Vector3 sc = SlotCenter(racks[hoverRack], hoverSlot);
                DrawCubeWiresV(sc, { 0.76f, 0.34f, 0.18f }, YELLOW);
            }
        }
        EndMode3D();

        // ---------------------------------------------------------- HUD ---
        const int sw = GetScreenWidth(), sh = GetScreenHeight();

        // crosshair
        DrawCircle(sw / 2, sh / 2, 2.5f, { 235, 235, 235, 210 });
        DrawCircleLines(sw / 2, sh / 2, 7.0f, { 235, 235, 235, 90 });

        // status panel
        DrawRectangle(12, 12, 320, 128, { 0, 0, 0, 150 });
        DrawRectangleLines(12, 12, 320, 128, { 70, 78, 92, 200 });
        DrawText(TextFormat("$ %.0f", money), 26, 22, 28, { 120, 255, 160, 255 });
        DrawText(TextFormat("Spare chips: %d", spares), 26, 58, 20, RAYWHITE);
        DrawText(TextFormat("Failed chips: %d / %d", failedTotal, totalSlots),
                 26, 84, 20, failedTotal > 0 ? Color{ 255, 110, 110, 255 } : Color{ 170, 180, 195, 255 });
        if (pendingChips > 0)
            DrawText(TextFormat("Delivery in %.1fs", pendingTimer), 26, 110, 18, SKYBLUE);

        // prompt
        const char* prompt = nullptr;
        Color promptCol = { 160, 170, 185, 255 };
        if (hoverRack >= 0) {
            const Chip& c = racks[hoverRack].slots[hoverSlot];
            if (c.failed()) {
                prompt = "Hold [E]  -  pull failed chip";
                promptCol = { 255, 120, 120, 255 };
            } else if (!c.present) {
                if (spares > 0) { prompt = "Hold [E]  -  install spare chip"; promptCol = { 150, 255, 180, 255 }; }
                else            { prompt = "No spares left!  Press [Q] to order"; promptCol = { 255, 180, 80, 255 }; }
            } else {
                prompt = "Chip operating normally";
                promptCol = { 150, 255, 180, 255 };
            }
        } else {
            prompt = "WASD move   |   Shift run   |   E service   |   Q order chips   |   Esc pause";
        }

        int tw = MeasureText(prompt, 20);
        DrawText(prompt, sw / 2 - tw / 2, sh - 56, 20, promptCol);

        // hold bar
        if (holdProgress > 0.0f) {
            float need = 1.0f;
            if (hoverRack >= 0) {
                const Chip& c = racks[hoverRack].slots[hoverSlot];
                need = c.failed() ? 1.2f : 0.7f;
            }
            const float frac = Clamp(holdProgress / need, 0.0f, 1.0f);
            const int   bw = 260, bh = 12;
            const int   bx = sw / 2 - bw / 2, by = sh - 92;
            DrawRectangle(bx, by, bw, bh, { 0, 0, 0, 170 });
            DrawRectangle(bx, by, (int)(bw * frac), bh, { 120, 220, 255, 255 });
            DrawRectangleLines(bx, by, bw, bh, { 90, 100, 115, 220 });
        }

        // transient message
        if (msgTimer > 0.0f) {
            const float a = Clamp(msgTimer, 0.0f, 1.0f);
            const int   mw = MeasureText(message.c_str(), 22);
            DrawRectangle(sw / 2 - mw / 2 - 14, 16, mw + 28, 38,
                          { 0, 0, 0, (unsigned char)(170 * a) });
            DrawText(message.c_str(), sw / 2 - mw / 2, 24, 22,
                     { 230, 235, 245, (unsigned char)(255 * a) });
        }

        // pause overlay
        if (paused) {
            DrawRectangle(0, 0, sw, sh, { 0, 0, 0, 170 });
            const char* t1 = "PAUSED";
            const char* t2 = "Click to resume";
            DrawText(t1, sw / 2 - MeasureText(t1, 48) / 2, sh / 2 - 50, 48, RAYWHITE);
            DrawText(t2, sw / 2 - MeasureText(t2, 20) / 2, sh / 2 + 10, 20, { 170, 180, 195, 255 });
        }

        DrawFPS(sw - 90, 12);
        EndDrawing();
    }

    EnableCursor();
    CloseWindow();
    return 0;
}
