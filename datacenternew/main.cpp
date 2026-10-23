// DATACENTER SHIFT: LLM OPS  -  a first-person 3D game (C++17 + raylib 5)
// Keep the GPU cluster training & serving your language model for one 8-hour shift.
#include "raylib.h"
#include "raymath.h"
#include <vector>
#include <string>
#include <cmath>
#include <cstdio>
#include <cstdlib>

enum Kind  { K_GPU, K_CRAC, K_PDU, K_STORE, K_NOC, K_DOCK };
enum Fault { FT_NONE, FT_GPU, FT_IB, FT_HEAT, FT_BREAKER, FT_DISK, FT_JOB, FT_DELIVERY };

struct Station {
    Kind kind; Vector3 pos, size; const char* name;
    Fault fault = FT_NONE; float prog = 0, age = 0; int log = 0;
};
struct LogLine { const char* text; int answer; };

static const LogLine LOGS[] = {
    {"CUDA error: uncorrectable ECC error on GPU 5", 2},
    {"NCCL WARN: all-reduce timeout, rank 412 hung", 1},
    {"step 48211: loss=NaN, grad_norm=inf",          3},
    {"RuntimeError: CUDA out of memory (KV cache)",  1},
    {"Straggler: node 17 running at 40% throughput", 2},
};
static const char* OPTIONS[] = {
    "[1] Kill job, resume from last checkpoint",
    "[2] Cordon node, run DCGM diagnostics",
    "[3] Roll back, lower LR, skip bad data batch"};

static const char* faultText(Fault f) {
    switch (f) {
        case FT_GPU:      return "XID 79: GPU fell off the bus - swap H100 (needs spare)";
        case FT_IB:       return "InfiniBand link flapping - reseat OSFP cable";
        case FT_HEAT:     return "Cold aisle 31C - clean CRAC filters";
        case FT_BREAKER:  return "PDU breaker tripped - reset it";
        case FT_DISK:     return "Checkpoint array 98% full - purge old checkpoints";
        case FT_JOB:      return "Training job crashed - triage at NOC terminal";
        case FT_DELIVERY: return "Delivery at dock - receive 4 spare GPUs";
        default:          return "";
    }
}
static float workTime(Fault f) {
    switch (f) {
        case FT_GPU: return 3.5f; case FT_IB: return 2.0f; case FT_HEAT: return 4.0f;
        case FT_BREAKER: return 1.5f; case FT_DISK: return 3.0f; case FT_DELIVERY: return 2.5f;
        default: return 1.0f;
    }
}

// ---------------- game state ----------------
static std::vector<Station> st;
static std::vector<float> history;
static int   state = 0;              // 0 title, 1 playing, 2 report
static float shiftT = 0, spawnT = 4, histT = 0, uptimeAcc = 0, tokens = 0;
static int   spares = 2, fixedCount = 0, mistakes = 0;
static float cap = 1;
static std::string toast; static float toastT = 0;
static const float SHIFT_LEN = 360.0f;

static void say(const char* m) { toast = m; toastT = 3.0f; }

static void resetGame() {
    st.clear(); history.clear();
    for (int r = 0; r < 2; r++)
        for (int i = 0; i < 4; i++) {
            static char names[8][16]; int id = r * 4 + i;
            snprintf(names[id], 16, "GPU Rack %c%d", 'A' + r, i + 1);
            st.push_back({K_GPU, {-7.5f + i * 5.0f, 1.6f, r ? 3.0f : -3.0f}, {2.0f, 3.2f, 1.2f}, names[id]});
        }
    st.push_back({K_CRAC,  {-11.5f, 1.4f, 0},   {1.6f, 2.8f, 3.0f}, "CRAC Cooling Unit"});
    st.push_back({K_PDU,   {11.5f, 1.1f, -4.0f}, {1.2f, 2.2f, 1.8f}, "Power Distribution"});
    st.push_back({K_STORE, {11.5f, 1.3f, 4.0f},  {1.6f, 2.6f, 1.8f}, "Checkpoint Storage"});
    st.push_back({K_NOC,   {0, 0.5f, -9.0f},     {4.0f, 1.0f, 1.5f}, "NOC Terminal"});
    st.push_back({K_DOCK,  {0, 0.05f, 9.5f},     {5.0f, 0.1f, 3.0f}, "Loading Dock"});
    shiftT = 0; spawnT = 4; histT = 0; uptimeAcc = 0; tokens = 0;
    spares = 2; fixedCount = 0; mistakes = 0; cap = 1; toastT = 0;
}

static bool blocked(Vector3 p) {
    if (fabsf(p.x) > 12.4f || fabsf(p.z) > 11.4f) return true;
    for (auto& s : st) {
        if (s.kind == K_DOCK) continue;
        if (fabsf(p.x - s.pos.x) < s.size.x / 2 + 0.4f && fabsf(p.z - s.pos.z) < s.size.z / 2 + 0.4f) return true;
    }
    return false;
}

static void spawnFault() {
    int active = 0; for (auto& s : st) if (s.fault != FT_NONE) active++;
    if (active >= 6) return;
    for (int tries = 0; tries < 30; tries++) {
        Station& s = st[rand() % st.size()];
        if (s.fault != FT_NONE) continue;
        switch (s.kind) {
            case K_GPU:   s.fault = (rand() % 2) ? FT_GPU : FT_IB; break;
            case K_CRAC:  s.fault = FT_HEAT; break;
            case K_PDU:   s.fault = FT_BREAKER; break;
            case K_STORE: s.fault = FT_DISK; break;
            case K_NOC:   s.fault = FT_JOB; s.log = rand() % 5; break;
            case K_DOCK:  s.fault = FT_DELIVERY; break;
        }
        s.prog = 0; s.age = 0; return;
    }
}

static void updateSim(float dt) {
    shiftT += dt;
    spawnT -= dt;
    if (spawnT <= 0) { spawnFault(); spawnT = Lerp(7.0f, 3.5f, shiftT / SHIFT_LEN) + (rand() % 20) / 10.0f; }
    int gpuOk = 0; cap = 1;
    for (auto& s : st) {
        if (s.fault != FT_NONE) s.age += dt;
        if (s.kind == K_GPU && s.fault == FT_NONE) gpuOk++;
        if (s.kind == K_CRAC  && s.fault) cap *= 0.6f;
        if (s.kind == K_PDU   && s.fault) cap *= 0.75f;
        if (s.kind == K_STORE && s.fault) cap *= 0.8f;
        if (s.kind == K_NOC   && s.fault) cap *= 0.1f;
    }
    cap *= gpuOk / 8.0f;
    tokens += cap * 1.2f * dt;               // millions of tokens
    uptimeAcc += cap * dt;
    histT += dt;
    if (histT >= 2.0f) { histT = 0; history.push_back(cap); }
}

static void drawStation(const Station& s, float t) {
    Vector3 p = s.pos, z = s.size;
    switch (s.kind) {
    case K_GPU: {
        DrawCube(p, z.x, z.y, z.z, {28, 30, 36, 255});
        DrawCubeWires(p, z.x, z.y, z.z, {70, 75, 90, 255});
        float sg = p.z < 0 ? 1.f : -1.f, fz = p.z + sg * (z.z / 2 + 0.01f);
        for (int i = 0; i < 8; i++) for (int j = 0; j < 3; j++) {
            Color c; bool on = sinf(t * 3 + i * 1.7f + j * 2.3f + p.x) > -0.3f;
            if (s.fault == FT_GPU) c = (fmodf(t, 0.6f) < 0.3f) ? RED : MAROON;
            else if (s.fault == FT_IB && i >= 6) c = (fmodf(t, 0.4f) < 0.2f) ? ORANGE : BROWN;
            else c = on ? GREEN : DARKGREEN;
            DrawCube({p.x - 0.6f + j * 0.6f, 0.5f + i * 0.36f, fz}, 0.3f, 0.14f, 0.02f, c);
        }
    } break;
    case K_CRAC:
        DrawCube(p, z.x, z.y, z.z, {120, 150, 175, 255});
        DrawCylinder({p.x, p.y + z.y / 2, p.z}, 0.6f, 0.6f, 0.1f, 20, DARKGRAY);
        DrawCube({p.x + 0.81f, p.y, p.z}, 0.02f, 1.6f, 2.4f, s.fault ? ORANGE : SKYBLUE);
        break;
    case K_PDU:
        DrawCube(p, z.x, z.y, z.z, {90, 90, 95, 255});
        DrawCube({p.x - 0.61f, p.y + 0.5f, p.z}, 0.02f, 0.3f, 0.3f, s.fault ? RED : LIME);
        DrawCube({p.x - 0.61f, p.y - 0.2f, p.z}, 0.02f, 0.5f, 0.2f, YELLOW);
        break;
    case K_STORE:
        DrawCube(p, z.x, z.y, z.z, {20, 22, 28, 255});
        for (int i = 0; i < 10; i++) for (int j = 0; j < 4; j++)
            DrawCube({p.x - 0.61f - 0.01f, 0.4f + i * 0.22f, p.z - 0.6f + j * 0.4f}, 0.02f, 0.1f, 0.3f,
                     s.fault ? ((i + j) % 2 ? RED : MAROON) : SKYBLUE);
        break;
    case K_NOC:
        DrawCube(p, z.x, z.y, z.z, {110, 80, 55, 255});
        for (int i = -1; i <= 1; i++) {
            DrawCube({p.x + i * 1.2f, 1.5f, p.z - 0.3f}, 1.0f, 0.65f, 0.06f, BLACK);
            DrawCube({p.x + i * 1.2f, 1.5f, p.z - 0.26f}, 0.9f, 0.55f, 0.02f,
                     s.fault ? ((fmodf(t, 0.5f) < 0.25f) ? RED : MAROON) : DARKGREEN);
        }
        break;
    case K_DOCK:
        DrawCube(p, z.x, 0.04f, z.z, {200, 170, 30, 255});
        DrawCube({p.x, p.y + 0.02f, p.z}, z.x - 0.4f, 0.05f, z.z - 0.4f, {50, 50, 55, 255});
        if (s.fault) for (int i = 0; i < 4; i++)
            DrawCube({p.x - 1.5f + i, 0.5f, p.z}, 0.8f, 0.9f, 0.8f, {160, 120, 70, 255});
        break;
    }
    if (s.fault) {
        float pulse = 0.5f + 0.5f * sinf(t * 6);
        DrawCube({p.x, p.y + z.y / 2 + 0.5f, p.z}, 0.3f, 0.3f, 0.3f, Fade(RED, 0.4f + 0.6f * pulse));
    }
}

int main() {
    InitWindow(1280, 720, "Datacenter Shift: LLM Ops");
    SetTargetFPS(60);
    Camera3D cam{};
    cam.position = {0, 1.7f, 0}; cam.target = {0, 1.7f, -1}; cam.up = {0, 1, 0};
    cam.fovy = 70; cam.projection = CAMERA_PERSPECTIVE;
    resetGame();

    while (!WindowShouldClose()) {
        float dt = GetFrameTime(), t = (float)GetTime();
        Station* near = nullptr;

        if (state == 0 && IsKeyPressed(KEY_ENTER)) { state = 1; resetGame(); DisableCursor(); }
        if (state == 2 && IsKeyPressed(KEY_R)) { state = 1; resetGame(); cam.position = {0, 1.7f, 0}; cam.target = {0, 1.7f, -1}; DisableCursor(); }

        if (state == 1) {
            Vector3 oldP = cam.position;
            UpdateCamera(&cam, CAMERA_FIRST_PERSON);
            if (blocked(cam.position)) {
                Vector3 d = Vector3Subtract(cam.position, oldP);
                cam.position = oldP; cam.target = Vector3Subtract(cam.target, d);
            }
            updateSim(dt);
            if (shiftT >= SHIFT_LEN) { state = 2; EnableCursor(); }

            float best = 1e9f;
            for (auto& s : st) if (s.fault != FT_NONE) {
                float d = Vector2Distance({cam.position.x, cam.position.z}, {s.pos.x, s.pos.z}) - fmaxf(s.size.x, s.size.z) * 0.5f;
                if (d < 2.2f && d < best) { best = d; near = &s; }
            }
            for (auto& s : st) if (&s != near && s.prog > 0) s.prog = fmaxf(0, s.prog - dt);
            if (near) {
                if (near->kind == K_NOC) {
                    int key = IsKeyPressed(KEY_ONE) ? 1 : IsKeyPressed(KEY_TWO) ? 2 : IsKeyPressed(KEY_THREE) ? 3 : 0;
                    if (key) {
                        if (key == LOGS[near->log].answer) { near->fault = FT_NONE; fixedCount++; say("Job resumed. Tokens flowing again!"); }
                        else { mistakes++; near->log = rand() % 5; say("Wrong fix! New alert, cluster still down."); }
                    }
                } else if (IsKeyDown(KEY_E)) {
                    if (near->fault == FT_GPU && spares <= 0) say("No spare GPUs! Wait for / receive a delivery at the dock.");
                    else {
                        near->prog += dt / workTime(near->fault);
                        if (near->prog >= 1) {
                            if (near->fault == FT_GPU) spares--;
                            if (near->fault == FT_DELIVERY) spares += 4;
                            near->fault = FT_NONE; near->prog = 0; fixedCount++;
                            say("Task complete.");
                        }
                    }
                } else near->prog = fmaxf(0, near->prog - dt * 2);
            }
        }

        // ---------------- draw ----------------
        BeginDrawing();
        ClearBackground({8, 10, 14, 255});
        BeginMode3D(cam);
        DrawPlane({0, -0.01f, 0}, {26, 24}, {45, 48, 55, 255});
        DrawGrid(26, 1.0f);
        DrawCube({0, 0.005f, -3.0f * 0 - 0.0f}, 20, 0.01f, 1.0f, {60, 40, 20, 255});   // hot aisle
        DrawCube({0, 0.005f, 0},  18, 0.012f, 0.3f, {30, 70, 120, 255});               // cold aisle stripe
        Color wall = {35, 38, 46, 255};
        DrawCube({0, 2.5f, -12.5f}, 26, 5, 0.5f, wall);
        DrawCube({0, 2.5f, 12.5f},  26, 5, 0.5f, wall);
        DrawCube({-13, 2.5f, 0}, 0.5f, 5, 25, wall);
        DrawCube({13, 2.5f, 0},  0.5f, 5, 25, wall);
        DrawCube({0, 5.2f, 0}, 26, 0.3f, 25, {25, 27, 32, 255});
        for (int x = -9; x <= 9; x += 6) for (int z = -8; z <= 8; z += 8)
            DrawCube({(float)x, 5.0f, (float)z}, 2.5f, 0.05f, 0.6f, {235, 240, 255, 255});
        // wall dashboard: tokens/s history
        DrawCube({0, 3.6f, -12.2f}, 12.4f, 2.4f, 0.1f, BLACK);
        for (size_t i = 0; i < history.size(); i++) {
            float h = fmaxf(0.05f, history[i] * 2.0f);
            Color c = history[i] > 0.8f ? GREEN : history[i] > 0.5f ? YELLOW : RED;
            DrawCube({-6.0f + i * 0.066f, 2.5f + h / 2, -12.1f}, 0.055f, h, 0.05f, c);
        }
        for (auto& s : st) drawStation(s, t);
        EndMode3D();

        // world labels
        if (state == 1) {
            Vector3 fwd = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
            for (auto& s : st) {
                Vector3 head = {s.pos.x, s.pos.y + s.size.y / 2 + 0.9f, s.pos.z};
                Vector3 to = Vector3Subtract(head, cam.position);
                if (Vector3DotProduct(fwd, to) > 0 && Vector3Length(to) < 16) {
                    Vector2 sp = GetWorldToScreen(head, cam);
                    int w = MeasureText(s.name, 16);
                    DrawText(s.name, (int)sp.x - w / 2, (int)sp.y, 16, s.fault ? RED : Fade(WHITE, 0.6f));
                }
            }
            // HUD
            int mins = 480 + (int)(shiftT / SHIFT_LEN * 480);
            DrawRectangle(10, 10, 330, 150, Fade(BLACK, 0.6f));
            DrawText(TextFormat("Shift clock  %02d:%02d", mins / 60, mins % 60), 20, 18, 22, WHITE);
            DrawText(TextFormat("Serving      %.2f M tok/s", cap * 1.2f), 20, 46, 20, cap > 0.8f ? GREEN : cap > 0.5f ? YELLOW : RED);
            DrawText(TextFormat("Tokens       %.0f M", tokens), 20, 70, 20, WHITE);
            float loss = 0.9f + 3.0f * expf(-tokens / 150.0f);
            DrawText(TextFormat("Train loss   %.3f", loss), 20, 94, 20, SKYBLUE);
            DrawText(TextFormat("Spare GPUs %d   Fixed %d   Errors %d", spares, fixedCount, mistakes), 20, 122, 18, LIGHTGRAY);
            // ticket queue
            int y = 18; DrawText("TICKET QUEUE", GetScreenWidth() - 520, y, 20, ORANGE); y += 26;
            for (auto& s : st) if (s.fault) {
                DrawText(TextFormat("%s: %s", s.name, faultText(s.fault)), GetScreenWidth() - 520, y, 15, s.age > 40 ? RED : WHITE);
                y += 20;
            }
            DrawCircle(GetScreenWidth() / 2, GetScreenHeight() / 2, 3, WHITE);
            if (near) {
                int cx = GetScreenWidth() / 2, cy = GetScreenHeight() - 150;
                if (near->kind == K_NOC) {
                    DrawRectangle(cx - 300, cy - 110, 600, 150, Fade(BLACK, 0.85f));
                    DrawText(TextFormat("ALERT: %s", LOGS[near->log].text), cx - 285, cy - 100, 20, LIME);
                    for (int i = 0; i < 3; i++) DrawText(OPTIONS[i], cx - 285, cy - 60 + i * 28, 20, WHITE);
                } else {
                    DrawText(TextFormat("Hold [E]: %s", faultText(near->fault)), cx - 260, cy, 20, YELLOW);
                    DrawRectangle(cx - 150, cy + 30, 300, 14, Fade(WHITE, 0.2f));
                    DrawRectangle(cx - 150, cy + 30, (int)(300 * near->prog), 14, LIME);
                }
            }
            if (toastT > 0) { toastT -= dt; DrawText(toast.c_str(), GetScreenWidth() / 2 - MeasureText(toast.c_str(), 22) / 2, 220, 22, ORANGE); }
        }

        if (state == 0) {
            DrawRectangle(0, 0, 1280, 720, Fade(BLACK, 0.85f));
            DrawText("DATACENTER SHIFT: LLM OPS", 340, 90, 40, SKYBLUE);
            const char* lines[] = {
                "You run the floor of a GPU cluster training & serving a large language model.",
                "Every minute of downtime is lost tokens. Survive an 8-hour shift.",
                "",
                "WASD + mouse: move/look        E (hold): perform task        1/2/3: triage at NOC",
                "",
                "GPU racks   - swap dead H100s (needs spares) / reseat InfiniBand cables",
                "CRAC unit   - clean cooling filters before GPUs thermal-throttle",
                "PDU         - reset tripped breakers          Storage - purge old checkpoints",
                "NOC desk    - read the logs, choose the right fix for crashed training jobs",
                "Loading dock - receive spare-GPU deliveries (restock your inventory)",
                "",
                "Pulsing red cubes = open tickets. Follow the ticket queue (top right).",
                "Press ENTER to clock in"};
            for (int i = 0; i < 13; i++) DrawText(lines[i], 200, 160 + i * 30, 20, i == 12 ? YELLOW : WHITE);
        }
        if (state == 2) {
            float avg = uptimeAcc / SHIFT_LEN;
            const char* grade = avg >= 0.9f ? "S" : avg >= 0.8f ? "A" : avg >= 0.65f ? "B" : avg >= 0.5f ? "C" : "D";
            DrawRectangle(0, 0, 1280, 720, Fade(BLACK, 0.88f));
            DrawText("SHIFT COMPLETE - HANDOVER REPORT", 300, 130, 36, SKYBLUE);
            DrawText(TextFormat("Tokens served:     %.0f M", tokens), 400, 230, 26, WHITE);
            DrawText(TextFormat("Cluster uptime:    %.1f %%", avg * 100), 400, 270, 26, WHITE);
            DrawText(TextFormat("Tickets resolved:  %d", fixedCount), 400, 310, 26, WHITE);
            DrawText(TextFormat("Triage mistakes:   %d", mistakes), 400, 350, 26, WHITE);
            DrawText(TextFormat("Grade: %s", grade), 400, 420, 56, YELLOW);
            DrawText("Press R for another shift, ESC to quit", 400, 520, 22, LIGHTGRAY);
        }
        EndDrawing();
    }
    CloseWindow();
    return 0;
}
