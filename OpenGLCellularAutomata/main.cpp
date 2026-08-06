#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <random>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cassert>

// Global config
bool limitFramerate = true;         //Controls initial state of FPS limiter. User Control with 'F' key.
const double TARGET_FPS = 60.0;
const double TARGET_FRAME_TIME = 1.0 / TARGET_FPS;
const double STATS_POLL_INTERVAL = 5.0; //Seconds between console stats reports (FPS + per-team territory counts)

const int GRID_SIZE = 512;         //Simulation size (square); window is now sized to the screen at runtime

const int NUM_TEAMS = 6;            //Number of competing slime colonies (must be <= palette size in screen.frag)
const int MAX_TEAMS = 8;            //Size of the per-team uniform arrays in cellular.comp (matches its color palette)
// NOTE: reproduction used to have a flat per-tick success-chance gate here (EXPAND_CHANCE)
// on top of the roll itself. Removed as part of the two-phase "highest bid wins" migration
// (design doc §12.3) -- an eligible empty tile is now claimed deterministically on the very
// first tick it qualifies, not probabilistically over ~12+ ticks like before. See
// reproductionWillingness's comment in cellular.comp for the model this replaced it with.

// Team-ID sentinels shared with cellular.comp: sources/deposits live in the main grid now
// (see DEPOSIT_ID/SOURCE_ID in cellular.comp), not a separate texture.
const uint16_t DEPOSIT_ID = 254;
const uint16_t SOURCE_ID = 255;

const float HOME_SOURCE_STRENGTH = 2000.0f;   //Energy/tick generated at each team's colony-center source
const float FREE_SOURCE_STRENGTH = 2000.0f;   //Energy/tick generated at each scattered neutral source
const int NUM_FREE_SOURCES = 18;            //Count of scattered neutral energy sources placed across the map
const float MIN_SOURCE_TO_BLOB_DIST = 40.0f;   //Placement rejection radius (px) around blob centers -- lowered from 75 so free sources can land close enough to a blob's frontier to matter, instead of only the far-away home source ever being in reach
const float MIN_SOURCE_TO_SOURCE_DIST = 60.0f; //Placement rejection radius (px) between sources

// Shared by every team -- no longer an archetype differentiator, just the shape of the
// economy everyone plays within.
const float ENERGY_CAPACITY = 2000.0f; //Max storable energy per cell, same for all teams
const float UPKEEP_COST = 0.0f;       //Energy spent per tick just to stay alive, same for all teams

// A successful land claim costs its parent startEnergy * this multiplier (see reproDebit in
// cellular.comp), same for every team -- a team's reproductionWillingness (see SlimeClass
// below) changes how OFTEN it succeeds at claiming empty land, not how much each success
// costs, keeping the playing field even on energy economics while still letting teams differ
// in how eagerly they reproduce.
const float REPRODUCTION_COST_MULTIPLIER = 1.2f;

// Fraction of a committed cell-to-cell transfer the receiver actually gets; the rest is real
// loss routed to waste, not rounding (see cellular.comp's sharing section). Raised from 0.95:
// that loss compounds multiplicatively over each hop from a source to the frontier (0.95^40 =
// ~13% survives a 40-hop trip), which was the dominant drag on how long it took surplus to
// reach contested territory at all. First-guess value -- expect to need empirical tuning,
// same as the other shared-economy constants here.
const float TRANSFER_EFFICIENCY = 0.99f;

// Conductivity reinforcement: a per-cell, per-DIRECTION memory of recent energy throughput
// (see cellular.comp) that boosts a cell's effective sharing rate in that specific direction
// the more it's been relaying energy that way, and decays back down when it isn't -- lets
// supply routes between a source and an active frontier organize into reinforced "trunk
// lines" instead of staying a plain diffusion gradient. Shared by every team, not a
// per-archetype dial, same as the two constants above. First-guess starting values -- expect
// to need empirical tuning.
const float CONDUCTIVITY_DECAY = 0.02f; //Per-tick decay fraction (~35-tick half-life)
const float CONDUCTIVITY_GAIN = 0.05f;  //Scales this tick's throughput into conductivity growth -- raised 10x from 0.005, which built up too slowly to meaningfully boost long-haul routes within the first few thousand ticks
const float CONDUCTIVITY_BOOST = 1.0f;   //Multiplier in effectiveRateQ16; at max conductivity (4.0) this triples the effective sharing rate

// Per-color profile: purely behavioral now (no stat archetypes like durability/burst damage --
// energyCapacity/upkeepCost are shared globals above, and there's no ally-bonus or lifespan
// mechanic). A cell dies only one way: its energy is driven to zero (upkeep, sharing, and/or
// combat damage) and it reverts to unclaimed land. See the field comments below and the
// fuller explanation above DEFAULT_TEAM_CLASSES for how
// each behavioral dial works.
struct SlimeClass {
    const char* name;
    float startEnergy;          // energy a newly-claimed cell begins with
    float energySatisfied;      // target energy level cells push toward/shed surplus above, NOT a cap -- see cellular.comp's sharing section
    float transferRate;         // altruism
    float reproductionWillingness; // scales how often (not how much) a team attempts/succeeds at claiming unclaimed land -- see REPRODUCTION_COST_MULTIPLIER
    float aggressionFraction;   // damage dealt per hostile neighbor touched, and the cost of maintaining it
    float defenseFraction;      // percentage reduction applied to incoming combat damage, capped in cellular.comp so it can't exceed 0.9
};

// Starting points only -- expect these to get tuned (by hand or by an evolutionary
// algorithm) once real matches are observed. startEnergy and the expected cost of a
// reproduction success (REPRODUCTION_COST_MULTIPLIER, shared by all teams) are kept
// perfectly even across teams -- differences come entirely from behavior: how altruistic
// (transferRate), reproductive (reproductionWillingness), aggressive (aggressionFraction),
// and defensive (defenseFraction) each team is.
//
// reproductionWillingness sets a team's bid strength for contested empty tiles: bid =
// candidateEnergy * reproductionWillingness (see resolveReproWinner in cellular.comp). An
// eager team (>1.0) outbids a reluctant one (<1.0) for the same tile, but every eligible
// empty tile is still claimed deterministically on the very first tick it qualifies -- there
// is no longer any per-tick success chance to tune. Reproduction and combat are both fully
// deterministic now (see below).
//
// aggressionFraction/defenseFraction govern combat, which is a continuous, deterministic
// erosion process, not a coin flip: every tick, a cell takes damage = (each hostile
// neighbor's energy * that neighbor's aggressionFraction), summed across all hostile
// neighbors, then reduced by this cell's own defenseFraction as a straight percentage
// mitigation. A cell dies (reverts to unclaimed land) whenever its energy is driven to zero
// by any combination of upkeep, sharing, and combat damage -- there's no separate
// "conquered" event or free energy grant on death; the vacated tile just gets recontested
// through the normal reproduction path like any other empty cell.
// Previously all six were pulled ~65% of the way toward Expander's dial values, which left
// them as minor variations on one archetype rather than genuinely different strategies. This
// pass pushes each team hard toward a distinct corner of the behavior space instead -- wide
// spread on every dial (transfer 0.01-0.15, reproductionWillingness 0.9-2.2, aggression
// 0.04-0.35, defense 0.06-0.45) so the archetypes actually play differently: a fast fragile
// land-grabber, a trunk-line-building networker, a glass-cannon brawler, a turtle that barely
// expands but is nearly unkillable once dug in, an all-around aggressive conqueror, and a
// reckless fanatic that out-aggros and out-breeds everyone at the cost of almost no defense.
//
// energySatisfied (added alongside the surplus-based sharing model in cellular.comp) also
// varies per team now: how much a team wants banked before it starts pushing surplus onward
// doubles as a hoarding/generosity dial, so it's set to roughly track each archetype's
// personality -- generous networkers and fast expanders keep little (400-450), the turtle
// keeps the most of anyone (1100), and the rest fall in between.
//
// Still unvalidated against real matches -- expect another round of tuning once observed.
const SlimeClass DEFAULT_TEAM_CLASSES[NUM_TEAMS] = {
    { "Expander",    200.0f, 750.0f,  0.1f, 2.2f, 0.05f, 0.0f }, // blitzes into empty land faster than anyone, invests almost nothing in combat either way -- fast but fragile alone
    { "Cooperator",  200.0f, 700.0f,  0.8f, 1.2f, 0.05f, 0.0f }, // heavy altruism to drive conductivity trunk-lines (see cellular.comp), modest defense, avoids fights, wins through network efficiency
    { "Raider",      200.0f, 1000.0f,  0.2f, 1.3f, 0.35f, 0.0f }, // the glass-cannon brawler: highest aggression of the six, but nearly undefended -- devastating on offense, dies fast if it doesn't keep winning
    { "Diplomat",    200.0f, 1500.0f, 0.3f, 0.9f, 0.04f, 0.0f }, // barely expands and rarely fights -- hoards the most energy of anyone behind the highest defense of the six, wins by simply outlasting everyone else
    { "Warlord",     200.0f, 950.0f,  0.4f, 1.8f, 0.25f, 0.0f }, // the all-around threat: high reproduction, high aggression, and real defense to back it up -- no single glaring weakness
    { "Zealot",      200.0f, 800.0f,  0.2f, 2.0f, 0.30f, 0.0f }, // reckless fanatic: expands and attacks almost as hard as the two specialists combined, but defense is nearly nonexistent
};

// Mutable at runtime: starts as a copy of DEFAULT_TEAM_CLASSES, optionally overwritten by
// a --stats command-line argument (see parseArgs) so an external evolutionary-algorithm
// driver can feed in candidate parameter sets without recompiling.
SlimeClass TEAM_CLASSES[NUM_TEAMS];

// Color palettes: one is chosen at random each run and uploaded to screen.frag.
// MAX_PALETTE_COLORS must match the array size declared there.
const int MAX_PALETTE_COLORS = 9;

struct Color { float r, g, b; };

Color hexColor(uint32_t hex) {
    return Color{
        ((hex >> 16) & 0xFF) / 255.0f,
        ((hex >> 8) & 0xFF) / 255.0f,
        (hex & 0xFF) / 255.0f
    };
}

// Enables ANSI/VT escape sequence processing on the Windows console so colored text
// (see ansiColor below) renders correctly instead of printing literal escape codes.
void enableAnsiConsole() {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (hOut != INVALID_HANDLE_VALUE && GetConsoleMode(hOut, &mode)) {
        SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
}

// 24-bit ANSI foreground color escape sequence for the given color.
std::string ansiColor(const Color& c) {
    std::ostringstream oss;
    oss << "\x1b[38;2;" << (int)(c.r * 255.0f) << ";" << (int)(c.g * 255.0f) << ";" << (int)(c.b * 255.0f) << "m";
    return oss.str();
}
const char* ANSI_RESET = "\x1b[0m";

struct ColorPalette {
    const char* name;
    std::vector<Color> colors;
};

// energyFP scale: see cellular.comp's ENERGY_SCALE comment -- a plain integer count in units
// of 1/256th of a "display" energy unit, reproducing the old whole+fractional-channel
// resolution as a flat integer rather than a whole/frac split.
const int32_t ENERGY_SCALE = 256;
// Q16.16 fixed-point scale for rate uniforms consumed by cellular.comp's mulShift.
const uint32_t Q16_SCALE = 65536;

int32_t toEnergyFP(float realUnits) {
    return (int32_t)std::lround(std::max(realUnits, 0.0f) * (float)ENERGY_SCALE);
}

uint32_t toQ16(float rate) {
    return (uint32_t)std::lround(std::max(rate, 0.0f) * (float)Q16_SCALE);
}

// Mirrors cellular.comp's packEnergy: scales a real energy value into an energyFP integer
// and splits it into the high (B) / low (A) 16-bit channel pair used to seed a cell's stored
// energy.
void encodeEnergy(float realUnits, uint16_t& hi, uint16_t& lo) {
    uint32_t fp = (uint32_t)toEnergyFP(realUnits);
    hi = (uint16_t)((fp >> 16) & 0xFFFFu);
    lo = (uint16_t)(fp & 0xFFFFu);
}

const std::vector<ColorPalette> COLOR_PALETTES = {
    { "Sunset Ocean", {
        hexColor(0x005F73), hexColor(0x0A9396), hexColor(0x94D2BD),
        hexColor(0xE9D8A6), hexColor(0xEE9B00), hexColor(0xCA6702),
        hexColor(0xBB3E03), hexColor(0xAE2012), hexColor(0x9B2226)
    } },
    { "Cherry Blossom", {
        hexColor(0x590D22), hexColor(0x800F2F), hexColor(0xA4133C),
        hexColor(0xC9184A), hexColor(0xFF4D6D), hexColor(0xFF758F),
        hexColor(0xFF8FA3), hexColor(0xFFB3C1), hexColor(0xFFCCD5)
    } },
    { "Dreamscape", {
        hexColor(0xA4F4F2), hexColor(0xA0C4FF), hexColor(0xFFD6E0),
        hexColor(0xFFADAD), hexColor(0xFFEF9F), hexColor(0xFDFFB6),
        hexColor(0xC1FBA4), hexColor(0x7BF1A8), hexColor(0xFFFFFF)
    } },
    { "Vibe", {
        hexColor(0xF94144), hexColor(0xF3722C), hexColor(0xF8961E),
        hexColor(0xF9844A), hexColor(0xF9C74F), hexColor(0x90BE6D),
        hexColor(0x43AA8B), hexColor(0x4D908E), hexColor(0x577590)
    } },
};

class SlimeMold {
private:
    // Two-phase tick (design doc §12): cellular.comp compiled twice from the same source,
    // once plain (Phase A -- "propose") and once with CA_PHASE_B defined (Phase B --
    // "resolve"). See step() for the two-dispatch sequencing.
    GLuint phaseAProgram = 0, phaseBProgram = 0;
    GLuint renderProgram = 0;
    GLuint textureA = 0, textureB = 0;
    GLuint conductivityA = 0, conductivityB = 0; // per-direction conductivity, ping-ponged in lockstep with textureA/B
    // Intent buffer (design doc §4): single-buffered, not persisted -- Phase A overwrites
    // every texel every tick before Phase B reads any of it (see step()'s mid-tick barrier).
    GLuint intentTransferTex = 0;  // GL_RGBA32UI, per-cardinal-direction give-amount (energyFP)
    GLuint intentReproBidTex = 0;  // GL_R32UI, direction-independent reproduction bid
    GLuint wasteBuffer = 0; // SSBO, one running uint counter (see WasteBuffer in cellular.comp)
    GLuint VAO = 0, VBO = 0;
    int width, height;
    bool useTextureA = true;
    uint32_t simFrame = 0;
    bool quiet = false; // suppresses startup/status console output (used for batch/headless runs)
    std::vector<Color> teamColors; // team index -> resolved color from this run's chosen palette

    double lastTotalEnergy = 0.0;
    uint32_t lastEnergyCheckFrame = 0;
    bool energyBaselineSet = false; // false until the first checkEnergyConservation() call, so it doesn't report a bogus delta against frame 0

    // Sum of every real-team cell's starting energy, computed once in initializeGrid() --
    // the fixed baseline the exact invariant in checkEnergyConservation() is measured
    // against (Σcells + Σdeposits == initialCellEnergy + injected - waste).
    double initialCellEnergy = 0.0;

    float quadVertices[24] = {
        -1.0f,  1.0f, 0.0f, 1.0f,  -1.0f, -1.0f, 0.0f, 0.0f,  1.0f, -1.0f, 1.0f, 0.0f,
        -1.0f,  1.0f, 0.0f, 1.0f,   1.0f, -1.0f, 1.0f, 0.0f,  1.0f,  1.0f, 1.0f, 1.0f
    };

public:
    SlimeMold(int w, int h, bool quietMode = false) : width(w), height(h), quiet(quietMode) {}

    bool initialize() {
        if (!createComputeShader(phaseAProgram, nullptr) ||
            !createComputeShader(phaseBProgram, "CA_PHASE_B") ||
            !createRenderShader()) return false;
        uploadTeamClasses();
        uploadPalette();
        createTextures();
        createWasteBuffer();
        initializeGrid();
        setupQuad();
        return true;
    }

    void step() {
        GLuint inputTex = useTextureA ? textureA : textureB;
        GLuint outputTex = useTextureA ? textureB : textureA;
        GLuint inputCond = useTextureA ? conductivityA : conductivityB;
        GLuint outputCond = useTextureA ? conductivityB : conductivityA;

        // ---- Phase A: propose -- reads only persisted start-of-tick state, publishes
        // intents to its own slot only (no cross-cell writes). ----
        glUseProgram(phaseAProgram);
        glBindImageTexture(0, inputTex, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16UI);
        glBindImageTexture(2, inputCond, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16UI);
        glBindImageTexture(4, intentTransferTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA32UI);
        glBindImageTexture(5, intentReproBidTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32UI);
        glUniform1ui(glGetUniformLocation(phaseAProgram, "uFrame"), simFrame);
        glDispatchCompute((width + 15) / 16, (height + 15) / 16, 1);
        // Phase A never touches the waste/injected SSBO (no atomics happen there), so only
        // the image-access barrier is needed here -- it's what makes Phase B's subsequent
        // imageLoad calls on the intent textures see Phase A's writes (RAW ordering).
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

        // ---- Phase B: resolve -- reads persisted state + Phase A's now-frozen intents,
        // writes the final new state by gather. ----
        glUseProgram(phaseBProgram);
        glBindImageTexture(0, inputTex, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16UI);
        glBindImageTexture(1, outputTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16UI);
        glBindImageTexture(2, inputCond, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16UI);
        glBindImageTexture(3, outputCond, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16UI);
        glBindImageTexture(4, intentTransferTex, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA32UI);
        glBindImageTexture(5, intentReproBidTex, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32UI);
        glUniform1ui(glGetUniformLocation(phaseBProgram, "uFrame"), simFrame);
        glDispatchCompute((width + 15) / 16, (height + 15) / 16, 1);
        // SHADER_STORAGE_BARRIER_BIT covers the waste SSBO's atomicAdd writes -- without it,
        // a later glGetBufferSubData (readWaste()) can read stale data, since the image-access
        // barrier alone only orders image2D loads/stores, not shader storage buffer access.
        // The cross-tick (WAR) hazard on the intent textures -- does next tick's Phase A need
        // to wait for this Phase B's reads to finish before overwriting them -- needs no new
        // barrier: it inherits the same guarantee the outputImage->inputImage ping-pong
        // already relies on via this same end-of-step() barrier.
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);

        useTextureA = !useTextureA;
        simFrame++;
    }

    void render() {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f); // black letterbox bars outside the square viewport
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(renderProgram);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, useTextureA ? textureA : textureB);
        glUniform1i(glGetUniformLocation(renderProgram, "uTexture"), 0);

        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, useTextureA ? conductivityA : conductivityB);
        glUniform1i(glGetUniformLocation(renderProgram, "uConductivity"), 1);

        glUniform1f(glGetUniformLocation(renderProgram, "uTime"), (float)glfwGetTime());

        glBindVertexArray(VAO);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);
    }

    // Reads back the current territory texture and tallies cells per team (index 0 =
    // unclaimed). Forces a GPU sync, so callers should only do this periodically, not
    // every frame.
    std::vector<long long> countTerritory() {
        GLuint currentTex = useTextureA ? textureA : textureB;
        std::vector<uint16_t> teamIds(width * height);
        glBindTexture(GL_TEXTURE_2D, currentTex);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RED_INTEGER, GL_UNSIGNED_SHORT, teamIds.data());

        std::vector<long long> counts(NUM_TEAMS + 1, 0);
        for (uint16_t team : teamIds) {
            if (team <= (uint16_t)NUM_TEAMS) counts[team]++; // skip source/deposit sentinels
        }
        return counts;
    }

    // Sums stored energy across every claimed (real-team) cell and every deposit's currently-
    // banked pool -- exactly the two conserved buckets the exact invariant in
    // checkEnergyConservation() is measured against (sources are excluded: their stored value
    // is a generation budget, not banked energy -- see cellular.comp's ledger comment).
    // Forces a GPU sync like countTerritory(), so callers should only do this periodically.
    struct EnergyStats {
        double totalEnergy = 0.0;    // real-team cells only
        double totalDeposits = 0.0;  // DEPOSIT_ID cells' currently-banked pool (a real, conserved bucket -- sources are not)
        double maxCellEnergy = 0.0;  // diagnostic: richest real-team cell anywhere on the grid
        long long cellsAboveMinCBirth = 0; // diagnostic: real-team cells at or above the CHEAPEST team's C_birth (a loose lower bound on "how many cells could plausibly be eligible somewhere")
    };

    EnergyStats computeEnergyStats() {
        GLuint currentTex = useTextureA ? textureA : textureB;
        std::vector<uint16_t> cellData(width * height * 4);
        glBindTexture(GL_TEXTURE_2D, currentTex);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA_INTEGER, GL_UNSIGNED_SHORT, cellData.data());

        auto energyAt = [&](int i) -> double {
            // Mirrors cellular.comp's energyOf: B/A are the high/low 16 bits of one packed
            // energyFP integer, converted back to real units via ENERGY_SCALE.
            double energyFP = (double)cellData[i * 4 + 2] * 65536.0 + (double)cellData[i * 4 + 3];
            return energyFP / (double)ENERGY_SCALE;
        };

        double minCBirth = 1e18;
        for (int t = 0; t < NUM_TEAMS; t++) {
            minCBirth = std::min(minCBirth, (double)TEAM_CLASSES[t].startEnergy * REPRODUCTION_COST_MULTIPLIER);
        }

        EnergyStats stats;
        for (int i = 0; i < width * height; i++) {
            uint16_t team = cellData[i * 4 + 0];
            if (team == 0 || team == SOURCE_ID) continue;

            if (team == DEPOSIT_ID) {
                stats.totalDeposits += energyAt(i);
            } else {
                double e = energyAt(i);
                stats.totalEnergy += e;
                stats.maxCellEnergy = std::max(stats.maxCellEnergy, e);
                if (e >= minCBirth) stats.cellsAboveMinCBirth++;
            }
        }
        return stats;
    }

    // Exact invariant: Σcells + Σdeposits == initialCellEnergy + injected - waste, every
    // term tracked precisely (initialCellEnergy fixed at grid-init time; injected/waste are
    // running SSBO counters the shader itself maintains via atomicAdd -- see LedgerBuffer in
    // cellular.comp).
    //
    // Reproduction contest resolution (design doc §12's two-phase "propose then resolve" tick)
    // closed the one remaining known approximation this comment used to describe: previously,
    // "who wins empty cell E" and "did candidate P win E" were two different single-pass
    // computations that could disagree once multiple candidates were simultaneously eligible,
    // silently leaking/duplicating energy. Now both questions are answered by the identical
    // resolveReproWinner oracle (cellular.comp) over Phase A's frozen, already-published bids
    // -- there's nothing left for two observers to disagree about. See reproDebit's comment in
    // cellular.comp for the one remaining subtlety this uncovered (natural insolvency's
    // reproDebit/child-mint ledger accounting, now handled explicitly rather than assumed).
    //
    // The only known residual source of drift left is the pre-existing, deliberately
    // untouched case where a single cell's transfer-give and reproDebit commitments jointly
    // exceed its own energy (each is individually bounded by currentEnergy, but not bounded
    // against each other -- see the "remaining" comment in cellular.comp). Observed behavior
    // post-fix is small, RARE step-jumps (a specific joint-overcommit combination actually
    // occurring) rather than the old smooth, ever-accelerating growth with claim activity --
    // e.g. exactly 0 for tens of thousands of claims in one run, then a handful of jumps
    // totaling well under 0.1% over a further 20000-tick/47000-claim stretch in another. 0.1%
    // comfortably covers that observed rate with real headroom to spare, a full 10x tighter
    // than the pre-fix tolerance this project used to need. Sized as a fraction of total
    // energy rather than a fixed absolute number, so it scales with how much economic
    // activity is actually happening: a fixed number would need re-tuning every time the
    // economy gets more productive (it already needed exactly that once, before this fix).
    static constexpr double ENERGY_INVARIANT_TOLERANCE_FRACTION = 0.001;
    static constexpr double ENERGY_INVARIANT_TOLERANCE_FLOOR = 50.0; // absolute floor for early-run checks when total energy is still small

    void checkEnergyConservation() {
        EnergyStats stats = computeEnergyStats();
        double waste = readWaste();
        double injected = readInjected();

        double actual = stats.totalEnergy + stats.totalDeposits;
        double expected = initialCellEnergy + injected - waste;
        double drift = actual - expected;
        double tolerance = std::max(ENERGY_INVARIANT_TOLERANCE_FLOOR, std::abs(actual) * ENERGY_INVARIANT_TOLERANCE_FRACTION);

        if (energyBaselineSet) {
            uint32_t ticksElapsed = simFrame - lastEnergyCheckFrame;
            double measuredGrowth = actual - lastTotalEnergy;

            std::cout << "Total energy: " << (long long)actual
                       << " (" << (measuredGrowth >= 0 ? "+" : "") << (long long)measuredGrowth
                       << " over " << ticksElapsed << " ticks) | injected: " << (long long)injected
                       << " | waste: " << (long long)waste
                       << " | maxCell: " << (long long)stats.maxCellEnergy
                       << " | cellsAboveMinCBirth: " << stats.cellsAboveMinCBirth
                       << " | claims: " << readClaimCount()
                       << " | drift: " << drift << " (" << (drift / std::max(actual, 1.0) * 100.0) << "%)";
            if (std::abs(drift) > tolerance) {
                std::cout << "  [!] exceeds invariant tolerance";
            }
            std::cout << std::endl;
        }

#ifdef _DEBUG
        assert(std::abs(drift) <= tolerance
               && "Energy conservation invariant violated: Sigma(cells) + Sigma(deposits) != "
                  "initialCellEnergy + injected - waste (see checkEnergyConservation comment)");
#endif

        lastTotalEnergy = actual;
        lastEnergyCheckFrame = simFrame;
        energyBaselineSet = true;
    }

    // Prints FPS plus a color-coded per-team cell count. Called periodically (every
    // STATS_POLL_INTERVAL) during an interactive run.
    void printStats(double fps) {
        std::vector<long long> counts = countTerritory();

        std::cout << "FPS: " << (int)std::round(fps) << (limitFramerate ? " (Limited)" : " (Unlimited)") << std::endl;
        for (int i = 0; i < NUM_TEAMS; i++) {
            std::cout << "  " << ansiColor(teamColors[i]) << TEAM_CLASSES[i].name << ANSI_RESET
                       << ": " << counts[i + 1] << std::endl;
        }

        checkEnergyConservation();
    }

    // Prints a single machine-parseable line with each team's final territory count, in
    // team-index order. Used by batch/headless runs so an external evolutionary-algorithm
    // driver can capture fitness from stdout without scraping human-oriented output.
    void printFinalResult() {
        std::vector<long long> counts = countTerritory();

        std::cout << "RESULT:";
        for (int i = 0; i < NUM_TEAMS; i++) {
            if (i > 0) std::cout << ",";
            std::cout << counts[i + 1];
        }
        std::cout << std::endl;
    }

    void cleanup() {
        if (phaseAProgram) glDeleteProgram(phaseAProgram);
        if (phaseBProgram) glDeleteProgram(phaseBProgram);
        if (renderProgram) glDeleteProgram(renderProgram);
        if (textureA) glDeleteTextures(1, &textureA);
        if (textureB) glDeleteTextures(1, &textureB);
        if (conductivityA) glDeleteTextures(1, &conductivityA);
        if (conductivityB) glDeleteTextures(1, &conductivityB);
        if (intentTransferTex) glDeleteTextures(1, &intentTransferTex);
        if (intentReproBidTex) glDeleteTextures(1, &intentReproBidTex);
        if (wasteBuffer) glDeleteBuffers(1, &wasteBuffer);
        if (VAO) glDeleteVertexArrays(1, &VAO);
        if (VBO) glDeleteBuffers(1, &VBO);
    }

private:
    std::string loadShader(const std::string& filepath) {
        std::ifstream file(filepath);
        if (!file.is_open()) {
            std::cerr << "Failed to open: " << filepath << std::endl;
            return "";
        }
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    // Splices a #define right after the source's first line (its #version directive, which
    // GLSL requires to be the very first thing in the shader) -- lets one file compile into
    // two different programs (see CA_PHASE_B in cellular.comp) without duplicating it.
    std::string injectDefine(const std::string& src, const char* macro) {
        if (!macro) return src;
        size_t nl = src.find('\n');
        if (nl == std::string::npos) return src;
        return src.substr(0, nl + 1) + "#define " + macro + "\n" + src.substr(nl + 1);
    }

    bool compileShader(GLuint shader) {
        GLint success;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
        if (!success) {
            char log[512];
            glGetShaderInfoLog(shader, 512, NULL, log);
            std::cerr << "Shader compilation failed: " << log << std::endl;
        }
        return success;
    }

    bool linkProgram(GLuint program) {
        GLint success;
        glGetProgramiv(program, GL_LINK_STATUS, &success);
        if (!success) {
            char log[512];
            glGetProgramInfoLog(program, 512, NULL, log);
            std::cerr << "Program linking failed: " << log << std::endl;
        }
        return success;
    }

    // Compiles shaders/cellular.comp into `outProgram`, optionally with `#define <macro>`
    // spliced in right after the #version line (see injectDefine) -- this is how the same
    // source file becomes both phaseAProgram (macro == nullptr) and phaseBProgram
    // (macro == "CA_PHASE_B") without duplicating the file.
    bool createComputeShader(GLuint& outProgram, const char* macro) {
        std::string source = injectDefine(loadShader("shaders/cellular.comp"), macro);
        if (source.empty()) return false;

        const char* src = source.c_str();
        GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
        glShaderSource(shader, 1, &src, NULL);
        glCompileShader(shader);

        if (!compileShader(shader)) return false;

        outProgram = glCreateProgram();
        glAttachShader(outProgram, shader);
        glLinkProgram(outProgram);
        glDeleteShader(shader);

        return linkProgram(outProgram);
    }

    bool createRenderShader() {
        std::string vertSource = loadShader("shaders/screen.vert");
        std::string fragSource = loadShader("shaders/screen.frag");
        if (vertSource.empty() || fragSource.empty()) return false;

        const char* vertSrc = vertSource.c_str();
        const char* fragSrc = fragSource.c_str();

        GLuint vertShader = glCreateShader(GL_VERTEX_SHADER);
        GLuint fragShader = glCreateShader(GL_FRAGMENT_SHADER);

        glShaderSource(vertShader, 1, &vertSrc, NULL);
        glShaderSource(fragShader, 1, &fragSrc, NULL);
        glCompileShader(vertShader);
        glCompileShader(fragShader);

        if (!compileShader(vertShader) || !compileShader(fragShader)) return false;

        renderProgram = glCreateProgram();
        glAttachShader(renderProgram, vertShader);
        glAttachShader(renderProgram, fragShader);
        glLinkProgram(renderProgram);

        glDeleteShader(vertShader);
        glDeleteShader(fragShader);

        return linkProgram(renderProgram);
    }

    // Uploads the shared economy constants and each team's behavioral constants once; these
    // never change during a run, so there's no need to re-set them every step(). Uniform
    // locations/values are per-program-object in OpenGL, so this has to run once per phase
    // program -- uploadTeamClasses() below just calls this twice. A uniform unused by a given
    // program (e.g. uAggressionFraction in Phase A) resolves to location -1, and glUniform*()
    // on -1 is a documented no-op, so it's safe to call the identical body against both
    // programs unconditionally rather than maintaining two separate uniform subsets.
    void uploadTeamClassesTo(GLuint program) {
        glUseProgram(program);

        // Energy-valued: converted to energyFP (see ENERGY_SCALE/toEnergyFP above). Rate-
        // valued: converted to Q16.16 (see Q16_SCALE/toQ16 above) -- cellular.comp's mulShift
        // consumes these without ever round-tripping through float, which is what makes two
        // invocations computing "the same" quantity guaranteed bit-identical.
        glUniform1i(glGetUniformLocation(program, "uEnergyCapacity"), toEnergyFP(ENERGY_CAPACITY));
        glUniform1i(glGetUniformLocation(program, "uUpkeepCost"), toEnergyFP(UPKEEP_COST));
        glUniform1ui(glGetUniformLocation(program, "uReproductionCostMultiplier"), toQ16(REPRODUCTION_COST_MULTIPLIER));
        glUniform1ui(glGetUniformLocation(program, "uTransferEfficiency"), toQ16(TRANSFER_EFFICIENCY));
        glUniform1f(glGetUniformLocation(program, "uConductivityDecay"), CONDUCTIVITY_DECAY);
        glUniform1f(glGetUniformLocation(program, "uConductivityGain"), CONDUCTIVITY_GAIN);
        glUniform1f(glGetUniformLocation(program, "uConductivityBoost"), CONDUCTIVITY_BOOST);

        GLint startEnergy[MAX_TEAMS] = {};
        GLint energySatisfied[MAX_TEAMS] = {};
        GLuint transferRate[MAX_TEAMS] = {};
        GLfloat reproductionWillingness[MAX_TEAMS] = {}; // stays float: only ever scales a probability, not an energy amount
        GLuint aggressionFraction[MAX_TEAMS] = {};
        GLuint defenseFraction[MAX_TEAMS] = {};

        for (int i = 0; i < NUM_TEAMS; i++) {
            const SlimeClass& c = TEAM_CLASSES[i];
            startEnergy[i] = toEnergyFP(c.startEnergy);
            energySatisfied[i] = toEnergyFP(c.energySatisfied);
            transferRate[i] = toQ16(c.transferRate);
            reproductionWillingness[i] = c.reproductionWillingness;
            aggressionFraction[i] = toQ16(c.aggressionFraction);
            defenseFraction[i] = toQ16(c.defenseFraction);
        }

        glUniform1iv(glGetUniformLocation(program, "uStartEnergy"), MAX_TEAMS, startEnergy);
        glUniform1iv(glGetUniformLocation(program, "uEnergySatisfied"), MAX_TEAMS, energySatisfied);
        glUniform1uiv(glGetUniformLocation(program, "uTransferRate"), MAX_TEAMS, transferRate);
        glUniform1fv(glGetUniformLocation(program, "uReproductionWillingness"), MAX_TEAMS, reproductionWillingness);
        glUniform1uiv(glGetUniformLocation(program, "uAggressionFraction"), MAX_TEAMS, aggressionFraction);
        glUniform1uiv(glGetUniformLocation(program, "uDefenseFraction"), MAX_TEAMS, defenseFraction);
    }

    void uploadTeamClasses() {
        uploadTeamClassesTo(phaseAProgram);
        uploadTeamClassesTo(phaseBProgram);
    }

    // Picks one of COLOR_PALETTES at random and uploads it to the render shader.
    void uploadPalette() {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<size_t> paletteDist(0, COLOR_PALETTES.size() - 1);
        const ColorPalette& palette = COLOR_PALETTES[paletteDist(gen)];

        teamColors.clear();
        for (int i = 0; i < NUM_TEAMS; i++) {
            teamColors.push_back(palette.colors[i % palette.colors.size()]);
        }

        if (!quiet) {
            std::cout << "Color palette: " << palette.name << std::endl;
            for (int i = 0; i < NUM_TEAMS; i++) {
                std::cout << "  " << ansiColor(teamColors[i]) << TEAM_CLASSES[i].name << ANSI_RESET << std::endl;
            }
        }

        std::vector<float> flatColors;
        flatColors.reserve(MAX_PALETTE_COLORS * 3);
        for (int i = 0; i < MAX_PALETTE_COLORS; i++) {
            Color c = (i < (int)palette.colors.size()) ? palette.colors[i] : Color{ 0.0f, 0.0f, 0.0f };
            flatColors.push_back(c.r);
            flatColors.push_back(c.g);
            flatColors.push_back(c.b);
        }

        glUseProgram(renderProgram);
        glUniform3fv(glGetUniformLocation(renderProgram, "uPalette"), MAX_PALETTE_COLORS, flatColors.data());
        glUniform1i(glGetUniformLocation(renderProgram, "uPaletteSize"), (int)palette.colors.size());

        // Needed to normalize the energy-fraction brightness in screen.frag; shared by every
        // team now, and doesn't change during a run, so it's uploaded here alongside the
        // palette rather than every frame.
        glUniform1f(glGetUniformLocation(renderProgram, "uEnergyCapacity"), ENERGY_CAPACITY);
    }

    void createTextures() {
        glGenTextures(1, &textureA);
        glGenTextures(1, &textureB);
        glGenTextures(1, &conductivityA);
        glGenTextures(1, &conductivityB);

        for (GLuint tex : {textureA, textureB, conductivityA, conductivityB}) {
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16UI, width, height, 0, GL_RGBA_INTEGER, GL_UNSIGNED_SHORT, NULL);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        }

        // Intent buffer (design doc §4): single-buffered, not persisted -- see the comment
        // above intentTransferTex/intentReproBidTex's declarations in cellular.comp.
        glGenTextures(1, &intentTransferTex);
        glBindTexture(GL_TEXTURE_2D, intentTransferTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32UI, width, height, 0, GL_RGBA_INTEGER, GL_UNSIGNED_INT, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

        glGenTextures(1, &intentReproBidTex);
        glBindTexture(GL_TEXTURE_2D, intentReproBidTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, width, height, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    }

    // Two running (lo, hi) uint counter pairs -- waste and injected -- plus a plain claim
    // counter (see LedgerBuffer in cellular.comp) -- bound once at a fixed SSBO binding point
    // (distinct namespace from the image-unit bindings used by textureA/B, so binding=0
    // doesn't collide) and never rebound per-step, since the shader accumulates into them via
    // atomicAdd rather than reading/writing them like the ping-pong pair. Cleared to 0 once
    // here; never reset again for the lifetime of the run.
    void createWasteBuffer() {
        glGenBuffers(1, &wasteBuffer);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, wasteBuffer);
        GLuint zeros[5] = { 0, 0, 0, 0, 0 };
        glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(zeros), zeros, GL_DYNAMIC_COPY);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, wasteBuffer);
    }

    // Diagnostics: successful reproductions since the run started (see claimCount in
    // cellular.comp's LedgerBuffer) -- useful for telling "expansion is slow" apart from
    // "expansion isn't happening at all."
    GLuint readLedgerCounter(int wordIndex) {
        GLuint raw = 0;
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, wasteBuffer);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, sizeof(GLuint) * wordIndex, sizeof(GLuint), &raw);
        return raw;
    }
    GLuint readClaimCount() { return readLedgerCounter(4); }

    // Reads the current waste/injected totals (a few bytes, not a full-grid sync) -- real
    // energy units, converted from energyFP (see ENERGY_SCALE), split across lo/hi 32-bit
    // words (see LedgerBuffer in cellular.comp for why).
    double readLedgerPair(GLintptr byteOffset) {
        GLuint raw[2] = { 0, 0 };
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, wasteBuffer);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, byteOffset, sizeof(raw), raw);
        double combined = (double)raw[1] * 4294967296.0 + (double)raw[0];
        return combined / (double)ENERGY_SCALE;
    }
    double readWaste() { return readLedgerPair(0); }
    double readInjected() { return readLedgerPair(sizeof(GLuint) * 2); }

    // Seeds NUM_TEAMS randomly placed, organically-shaped blobs (circles deformed by
    // a couple of random sine harmonics in polar coordinates) on an otherwise empty grid,
    // plus energy sources: one home source at each blob's own center (a single reserved
    // hole carved out of the blob, since a source can't be team-owned territory), and a
    // handful of additional neutral sources scattered elsewhere for colonies to fight over.
    // Sources live directly in the grid now (team id SOURCE_ID) rather than a separate
    // texture -- see cellular.comp's harvest logic, which treats any claimed cell adjacent
    // to one as automatically drawing a share of its strength.
    void initializeGrid() {
        std::vector<uint16_t> data(width * height * 4, 0);

        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<int> posDist(0, width - 1);
        std::uniform_int_distribution<int> radiusDist(25, 50);
        std::uniform_int_distribution<int> freqDist(2, 5);
        std::uniform_real_distribution<float> ampDist(0.08f, 0.22f);
        std::uniform_real_distribution<float> phaseDist(0.0f, 6.28318f);

        std::vector<std::pair<int, int>> centers;
        const int minCenterDist = std::min(width, height) / (NUM_TEAMS / 2 + 2);

        for (int team = 1; team <= NUM_TEAMS; team++) {
            int cx = 0, cy = 0;
            for (int attempt = 0; attempt < 50; attempt++) {
                cx = posDist(gen);
                cy = posDist(gen);
                bool farEnough = true;
                for (auto& c : centers) {
                    int dx = cx - c.first, dy = cy - c.second;
                    if (dx * dx + dy * dy < minCenterDist * minCenterDist) { farEnough = false; break; }
                }
                if (farEnough) break;
            }
            centers.push_back({ cx, cy });

            const SlimeClass& cls = TEAM_CLASSES[team - 1];
            uint16_t startEnergyHi, startEnergyLo;
            encodeEnergy(cls.startEnergy, startEnergyHi, startEnergyLo);

            float baseRadius = (float)radiusDist(gen);
            int freq1 = freqDist(gen), freq2 = freqDist(gen);
            float amp1 = ampDist(gen), amp2 = ampDist(gen);
            float phase1 = phaseDist(gen), phase2 = phaseDist(gen);

            int bound = (int)(baseRadius * 1.5f) + 1;
            int placedCellCount = 0;
            for (int dy = -bound; dy <= bound; dy++) {
                for (int dx = -bound; dx <= bound; dx++) {
                    if (dx == 0 && dy == 0) continue; // reserved for this team's home source, placed below

                    float dist = std::sqrt((float)(dx * dx + dy * dy));
                    float angle = std::atan2((float)dy, (float)dx);
                    float edge = baseRadius * (1.0f + amp1 * std::sin(freq1 * angle + phase1)
                                                     + amp2 * std::sin(freq2 * angle + phase2));
                    if (dist <= edge) {
                        int px = ((cx + dx) % width + width) % width;
                        int py = ((cy + dy) % height + height) % height;
                        int idx = (py * width + px) * 4;
                        data[idx + 0] = (uint16_t)team;         // team
                        data[idx + 1] = 0;                       // age
                        data[idx + 2] = startEnergyHi;           // energyFP high 16 bits
                        data[idx + 3] = startEnergyLo;           // energyFP low 16 bits
                        placedCellCount++;
                    }
                }
            }
            initialCellEnergy += (double)placedCellCount * (double)cls.startEnergy;
        }

        // Energy sources: one home source per blob center (the pixel reserved above), plus
        // scattered neutral sources rejection-sampled away from every blob center and every
        // other source.
        auto placeSource = [&](int x, int y, float strength) {
            uint16_t hi, lo;
            encodeEnergy(strength, hi, lo);
            int idx = (y * width + x) * 4;
            data[idx + 0] = SOURCE_ID;
            data[idx + 1] = 0;
            data[idx + 2] = hi;
            data[idx + 3] = lo;
        };
        for (auto& c : centers) placeSource(c.first, c.second, HOME_SOURCE_STRENGTH);

        std::vector<std::pair<int, int>> allSources = centers;
        for (int i = 0; i < NUM_FREE_SOURCES; i++) {
            int sx = 0, sy = 0;
            bool placed = false;
            for (int attempt = 0; attempt < 50; attempt++) {
                sx = posDist(gen);
                sy = posDist(gen);
                bool farEnough = true;
                for (auto& c : centers) {
                    float dx = (float)(sx - c.first), dy = (float)(sy - c.second);
                    if (dx * dx + dy * dy < MIN_SOURCE_TO_BLOB_DIST * MIN_SOURCE_TO_BLOB_DIST) { farEnough = false; break; }
                }
                if (farEnough) {
                    for (auto& s : allSources) {
                        float dx = (float)(sx - s.first), dy = (float)(sy - s.second);
                        if (dx * dx + dy * dy < MIN_SOURCE_TO_SOURCE_DIST * MIN_SOURCE_TO_SOURCE_DIST) { farEnough = false; break; }
                    }
                }
                // Safety net on top of the distance-based rejection above: the blobs' sine-
                // deformed edges are only approximately circular, so a candidate can pass the
                // distance checks while still landing on already-claimed territory (harmless
                // before, when sources lived in a separate texture -- now it would silently
                // erase a team's cell, so re-roll instead).
                if (farEnough && data[(sy * width + sx) * 4 + 0] != 0) farEnough = false;
                if (farEnough) { placed = true; break; }
            }
            if (!placed) continue; // couldn't find a free spot after 50 attempts; skip this one
            allSources.push_back({ sx, sy });
            placeSource(sx, sy, FREE_SOURCE_STRENGTH);
        }

        glBindTexture(GL_TEXTURE_2D, textureA);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA_INTEGER, GL_UNSIGNED_SHORT, data.data());

        // Conductivity starts at 0 everywhere (no relay history yet) -- explicitly zeroed on
        // both ping-pong buffers rather than relying on glTexImage2D's undefined initial
        // content, since useTextureA starts true and the very first step() reads conductivityA
        // as input.
        std::vector<uint16_t> zeroCond(width * height * 4, 0);
        glBindTexture(GL_TEXTURE_2D, conductivityA);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA_INTEGER, GL_UNSIGNED_SHORT, zeroCond.data());
        glBindTexture(GL_TEXTURE_2D, conductivityB);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA_INTEGER, GL_UNSIGNED_SHORT, zeroCond.data());
    }

    void setupQuad() {
        glGenVertexArrays(1, &VAO);
        glGenBuffers(1, &VBO);
        glBindVertexArray(VAO);
        glBindBuffer(GL_ARRAY_BUFFER, VBO);
        glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_STATIC_DRAW);

        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glBindVertexArray(0);
    }
};

struct RunConfig {
    bool batchMode = false;
    int ticksToRun = 0;
};

// Parses CLI args and initializes the global TEAM_CLASSES from DEFAULT_TEAM_CLASSES, with
// optional overrides. Supported flags:
//   --ticks <N>   Run N simulation ticks headlessly (no window/rendering/framerate limit),
//                 print final per-team territory counts as "RESULT:c1,c2,...,cN" to stdout,
//                 then exit. Omit this flag to run the normal interactive fullscreen mode.
//   --stats <csv> Comma-separated list of NUM_TEAMS * 6 floats (startEnergy, energySatisfied,
//                 transferRate, reproductionWillingness, aggressionFraction, defenseFraction,
//                 repeated per team in team order) overriding the compiled-in defaults. Lets an external
//                 evolutionary-algorithm driver feed in candidate parameter sets without
//                 recompiling. Works in either mode. (ENERGY_CAPACITY/UPKEEP_COST/REPRODUCTION_
//                 COST_MULTIPLIER are shared globals, not per-team, so they aren't part of
//                 this list.)
RunConfig parseArgs(int argc, char** argv) {
    for (int i = 0; i < NUM_TEAMS; i++) TEAM_CLASSES[i] = DEFAULT_TEAM_CLASSES[i];

    RunConfig config;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--ticks" && i + 1 < argc) {
            config.ticksToRun = std::atoi(argv[++i]);
            config.batchMode = true;
        } else if (arg == "--stats" && i + 1 < argc) {
            std::vector<float> values;
            std::stringstream ss(argv[++i]);
            std::string token;
            while (std::getline(ss, token, ',')) values.push_back(std::stof(token));

            if ((int)values.size() != NUM_TEAMS * 6) {
                std::cerr << "--stats expects " << (NUM_TEAMS * 6) << " comma-separated values, got "
                          << values.size() << std::endl;
                std::exit(1);
            }

            for (int t = 0; t < NUM_TEAMS; t++) {
                const float* v = &values[t * 6];
                TEAM_CLASSES[t].startEnergy             = v[0];
                TEAM_CLASSES[t].energySatisfied          = v[1];
                TEAM_CLASSES[t].transferRate             = v[2];
                TEAM_CLASSES[t].reproductionWillingness  = v[3];
                TEAM_CLASSES[t].aggressionFraction       = v[4];
                TEAM_CLASSES[t].defenseFraction          = v[5];
            }
        }
    }

    return config;
}

int main(int argc, char** argv) {
    enableAnsiConsole();

    RunConfig config = parseArgs(argc, argv);

    if (!glfwInit()) return -1;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* window = nullptr;
    GLFWmonitor* monitor = nullptr;
    const GLFWvidmode* mode = nullptr;

    if (config.batchMode) {
        // Headless: an invisible window purely to host the GL context the compute shader
        // needs. No rendering, no fullscreen setup, no framerate limiting.
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        window = glfwCreateWindow(1, 1, "Adversarial Slime Mold (headless)", NULL, NULL);
    } else {
        monitor = glfwGetPrimaryMonitor();
        mode = glfwGetVideoMode(monitor);

        // Borderless windowed fullscreen: an undecorated window sized/positioned to cover the
        // whole screen. Avoids the display-mode switch (and its flicker/alt-tab quirks) that
        // comes with exclusive fullscreen.
        glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
        glfwWindowHint(GLFW_RED_BITS, mode->redBits);
        glfwWindowHint(GLFW_GREEN_BITS, mode->greenBits);
        glfwWindowHint(GLFW_BLUE_BITS, mode->blueBits);
        glfwWindowHint(GLFW_REFRESH_RATE, mode->refreshRate);

        window = glfwCreateWindow(mode->width, mode->height, "Adversarial Slime Mold", NULL, NULL);
        if (window) {
            int monitorX, monitorY;
            glfwGetMonitorPos(monitor, &monitorX, &monitorY);
            glfwSetWindowPos(window, monitorX, monitorY);
        }
    }

    if (!window) { glfwTerminate(); return -1; }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(0);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) return -1;

    if (!config.batchMode) {
        std::cout << "OpenGL: " << glGetString(GL_VERSION) << " | GPU: " << glGetString(GL_RENDERER) << std::endl;
    }

    SlimeMold sim(GRID_SIZE, GRID_SIZE, config.batchMode /* quiet */);
    if (!sim.initialize()) return -1;

    if (config.batchMode) {
        // Run the requested number of ticks as fast as possible: no rendering, no vsync
        // wait, no window event pumping. Then report final territory counts and exit.
        char* debugEnergyEnv = nullptr;
        size_t debugEnergyLen = 0;
        _dupenv_s(&debugEnergyEnv, &debugEnergyLen, "SLIME_DEBUG_ENERGY");
        bool debugEnergy = debugEnergyEnv != nullptr;
        free(debugEnergyEnv);

        for (int i = 0; i < config.ticksToRun; i++) {
            sim.step();
            if (debugEnergy && i % 100 == 0) sim.printStats(0);
        }
        sim.printFinalResult();
        sim.cleanup();
        glfwTerminate();
        return 0;
    }

    // The simulation grid is square; letterbox it centered on screen instead of stretching
    // it, so blob shapes stay true to their real proportions on widescreen monitors.
    int squareSize = std::min(mode->width, mode->height);
    int viewportX = (mode->width - squareSize) / 2;
    int viewportY = (mode->height - squareSize) / 2;
    glViewport(viewportX, viewportY, squareSize, squareSize);

    double lastTime = glfwGetTime(), lastFrameTime = glfwGetTime();
    int frameCount = 0;
    bool fKeyPressed = false;

    while (!glfwWindowShouldClose(window)) {
        double currentTime = glfwGetTime();
        glfwPollEvents();

        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;

        if (glfwGetKey(window, GLFW_KEY_F) == GLFW_PRESS && !fKeyPressed) {
            limitFramerate = !limitFramerate;
            std::cout << "Framerate: " << (limitFramerate ? "Limited" : "Unlimited") << std::endl;
            fKeyPressed = true;
        }
        if (glfwGetKey(window, GLFW_KEY_F) == GLFW_RELEASE) fKeyPressed = false;

        if (!limitFramerate || currentTime - lastFrameTime >= TARGET_FRAME_TIME) {
            frameCount++;
            if (currentTime - lastTime >= STATS_POLL_INTERVAL) {
                double fps = frameCount / (currentTime - lastTime);
                sim.printStats(fps);
                frameCount = 0;
                lastTime = currentTime;
            }

            sim.step();
            sim.render();
            glfwSwapBuffers(window);
            lastFrameTime = currentTime;
        }
    }

    sim.cleanup();
    glfwTerminate();
    return 0;
}
