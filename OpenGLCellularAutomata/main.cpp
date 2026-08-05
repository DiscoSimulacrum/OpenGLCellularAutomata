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

// Global config
bool limitFramerate = true;         //Controls initial state of FPS limiter. User Control with 'F' key.
const double TARGET_FPS = 60.0;
const double TARGET_FRAME_TIME = 1.0 / TARGET_FPS;
const double STATS_POLL_INTERVAL = 5.0; //Seconds between console stats reports (FPS + per-team territory counts)

const int GRID_SIZE = 1024;         //Simulation size (square); window is now sized to the screen at runtime

const int NUM_TEAMS = 6;            //Number of competing slime colonies (must be <= palette size in screen.frag)
const int MAX_TEAMS = 8;            //Size of the per-team uniform arrays in cellular.comp (matches its color palette)
const float EXPAND_CHANCE = 0.08f;    //Action-chance multiplier when claiming unclaimed territory (combat itself is deterministic, no chance involved -- see cellular.comp)

const float HOME_SOURCE_STRENGTH = 40.0f;   //Energy/tick generated at each team's colony-center source
const float FREE_SOURCE_STRENGTH = 25.0f;   //Energy/tick generated at each scattered neutral source
const int NUM_FREE_SOURCES = 18;            //Count of scattered neutral energy sources placed across the map
const float MIN_SOURCE_TO_BLOB_DIST = 75.0f;   //Placement rejection radius (px) around blob centers
const float MIN_SOURCE_TO_SOURCE_DIST = 60.0f; //Placement rejection radius (px) between sources

// Shared by every team -- no longer an archetype differentiator, just the shape of the
// economy everyone plays within.
const float ENERGY_CAPACITY = 1500.0f; //Max storable energy per cell, same for all teams
const float UPKEEP_COST = 0.3f;       //Energy spent per tick just to stay alive, same for all teams

// Reproduction's EXPECTED cost (see totalAttackSpend in cellular.comp) is startEnergy *
// this multiplier, same for every team -- a team's reproductionWillingness (see SlimeClass
// below) changes how OFTEN it succeeds at claiming empty land, not how much each success
// costs on average, keeping the playing field even on energy economics while still letting
// teams differ in how eagerly they reproduce.
const float REPRODUCTION_COST_MULTIPLIER = 1.2f;

// Conductivity reinforcement: a per-cell memory of recent energy throughput (see
// cellular.comp) that boosts a cell's effective sharing rate the more it's been relaying
// energy, and decays back down when it isn't -- lets supply routes between a source and an
// active frontier organize into reinforced "trunk lines" instead of staying a plain
// diffusion gradient. Shared by every team, not a per-archetype dial, same as the two
// constants above. First-guess starting values -- expect to need empirical tuning.
const float CONDUCTIVITY_DECAY = 0.02f; //Per-tick decay fraction (~35-tick half-life)
const float CONDUCTIVITY_GAIN = 0.01f; //Scales this tick's throughput into conductivity growth (boosted 20x from 0.0005 -- first attempt showed no visible reinforcement)
const float CONDUCTIVITY_BOOST = 1.0f;   //Multiplier in effectiveRate; at max conductivity (4.0) this triples the effective sharing rate

// Per-color profile: purely behavioral now (no stat archetypes like durability/burst damage --
// energyCapacity/upkeepCost are shared globals above, and there's no ally-bonus or lifespan
// mechanic). A cell dies only one way: its energy is driven to zero (upkeep, sharing, and/or
// combat damage) and it reverts to unclaimed land. See the field comments below and the
// fuller explanation above DEFAULT_TEAM_CLASSES for how
// each behavioral dial works.
struct SlimeClass {
    const char* name;
    float startEnergy;          // energy a newly-claimed cell begins with
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
// reproductionWillingness multiplies the flat EXPAND_CHANCE odds of claiming an unclaimed
// neighbor each tick -- an eager team (>1.0) succeeds sooner on average, a reluctant team
// (<1.0) succeeds later, but because the per-tick cost scales with that same effective
// chance, the EXPECTED total energy spent per successful claim is startEnergy *
// REPRODUCTION_COST_MULTIPLIER regardless of willingness (see totalAttackSpend in
// cellular.comp). Claiming empty land is still the only probabilistic part of the sim --
// combat itself is fully deterministic (see below).
//
// aggressionFraction/defenseFraction govern combat, which is a continuous, deterministic
// erosion process, not a coin flip: every tick, a cell takes damage = (each hostile
// neighbor's energy * that neighbor's aggressionFraction), summed across all hostile
// neighbors, then reduced by this cell's own defenseFraction as a straight percentage
// mitigation. A cell dies (reverts to unclaimed land) whenever its energy is driven to zero
// by any combination of upkeep, sharing, and combat damage -- there's no separate
// "conquered" event or free energy grant on death; the vacated tile just gets recontested
// through the normal reproduction path like any other empty cell.
// Expander (aggressive reproduction + low aggression + moderate defense) empirically produced
// the healthiest-looking results under the deterministic siege combat model -- the other five
// were pulled ~65% of the way toward its transferRate/reproductionWillingness/aggression/
// defense values below, keeping just enough spread for identity rather than full duplicates.
const SlimeClass DEFAULT_TEAM_CLASSES[NUM_TEAMS] = {
    { "Expander",    600.0f, 0.03f, 2.0f, 0.08f, 0.15f }, // spreads into empty land fast, doesn't share, doesn't fight, turtles a bit if touched
    { "Cooperator",  600.0f, 0.05f, 1.4f, 0.08f, 0.15f }, // shares a bit more than most, doesn't fight much, leans on allies + modest defense
    { "Raider",      600.0f, 0.04f, 1.4f, 0.19f, 0.12f }, // still the most aggressive of the six, but far less of a glass cannon than before
    { "Diplomat",    600.0f, 0.04f, 1.7f, 0.08f, 0.17f }, // grows and shares peacefully, avoids fights, relies on defense to survive contact
    { "Warlord",     600.0f, 0.04f, 1.8f, 0.17f, 0.13f }, // expands and fights hard, moderate defense
    { "Zealot",      600.0f, 0.04f, 1.4f, 0.17f, 0.15f }, // doesn't expand as eagerly as Expander, still leans aggressive
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

// Mirrors cellular.comp's encodeEnergy: splits a float energy value into the integer (B)
// and fractional 1/256ths (A) channel pair used to seed a cell's stored energy.
void encodeEnergy(float energy, uint16_t& whole, uint16_t& frac) {
    float e = std::max(energy, 0.0f);
    whole = (uint16_t)std::min(std::floor(e), 65535.0f);
    frac = (uint16_t)std::min(std::max((e - std::floor(e)) * 256.0f, 0.0f), 255.0f);
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
    GLuint computeProgram = 0, renderProgram = 0;
    GLuint textureA = 0, textureB = 0;
    GLuint sourceMapTexture = 0; // static per-cell energy-generation strength; never ping-ponged, bound once
    GLuint VAO = 0, VBO = 0;
    int width, height;
    bool useTextureA = true;
    uint32_t simFrame = 0;
    bool quiet = false; // suppresses startup/status console output (used for batch/headless runs)
    std::vector<Color> teamColors; // team index -> resolved color from this run's chosen palette

    float quadVertices[24] = {
        -1.0f,  1.0f, 0.0f, 1.0f,  -1.0f, -1.0f, 0.0f, 0.0f,  1.0f, -1.0f, 1.0f, 0.0f,
        -1.0f,  1.0f, 0.0f, 1.0f,   1.0f, -1.0f, 1.0f, 0.0f,  1.0f,  1.0f, 1.0f, 1.0f
    };

public:
    SlimeMold(int w, int h, bool quietMode = false) : width(w), height(h), quiet(quietMode) {}

    bool initialize() {
        if (!createComputeShader() || !createRenderShader()) return false;
        uploadTeamClasses();
        uploadPalette();
        createTextures();
        initializeGrid();
        setupQuad();

        // The source map never changes after this point, so bind it once here rather
        // than re-binding every step() like the ping-pong pair (which does rotate).
        glBindImageTexture(2, sourceMapTexture, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32F);
        return true;
    }

    void step() {
        glUseProgram(computeProgram);

        GLuint inputTex = useTextureA ? textureA : textureB;
        GLuint outputTex = useTextureA ? textureB : textureA;

        glBindImageTexture(0, inputTex, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16UI);
        glBindImageTexture(1, outputTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16UI);

        glUniform1ui(glGetUniformLocation(computeProgram, "uFrame"), simFrame);
        glUniform1f(glGetUniformLocation(computeProgram, "uExpandChance"), EXPAND_CHANCE);

        glDispatchCompute((width + 15) / 16, (height + 15) / 16, 1);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

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
        glBindTexture(GL_TEXTURE_2D, sourceMapTexture);
        glUniform1i(glGetUniformLocation(renderProgram, "uSourceMap"), 1);

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
        for (uint16_t team : teamIds) counts[team]++;
        return counts;
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
        if (computeProgram) glDeleteProgram(computeProgram);
        if (renderProgram) glDeleteProgram(renderProgram);
        if (textureA) glDeleteTextures(1, &textureA);
        if (textureB) glDeleteTextures(1, &textureB);
        if (sourceMapTexture) glDeleteTextures(1, &sourceMapTexture);
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

    bool createComputeShader() {
        std::string source = loadShader("shaders/cellular.comp");
        if (source.empty()) return false;

        const char* src = source.c_str();
        GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
        glShaderSource(shader, 1, &src, NULL);
        glCompileShader(shader);

        if (!compileShader(shader)) return false;

        computeProgram = glCreateProgram();
        glAttachShader(computeProgram, shader);
        glLinkProgram(computeProgram);
        glDeleteShader(shader);

        return linkProgram(computeProgram);
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

    // Uploads the shared economy constants and each team's behavioral constants to the
    // compute shader once; these never change during a run, so there's no need to re-set
    // them every step().
    void uploadTeamClasses() {
        glUseProgram(computeProgram);

        glUniform1f(glGetUniformLocation(computeProgram, "uEnergyCapacity"), ENERGY_CAPACITY);
        glUniform1f(glGetUniformLocation(computeProgram, "uUpkeepCost"), UPKEEP_COST);
        glUniform1f(glGetUniformLocation(computeProgram, "uReproductionCostMultiplier"), REPRODUCTION_COST_MULTIPLIER);
        glUniform1f(glGetUniformLocation(computeProgram, "uConductivityDecay"), CONDUCTIVITY_DECAY);
        glUniform1f(glGetUniformLocation(computeProgram, "uConductivityGain"), CONDUCTIVITY_GAIN);
        glUniform1f(glGetUniformLocation(computeProgram, "uConductivityBoost"), CONDUCTIVITY_BOOST);

        float startEnergy[MAX_TEAMS] = {};
        float transferRate[MAX_TEAMS] = {};
        float reproductionWillingness[MAX_TEAMS] = {};
        float aggressionFraction[MAX_TEAMS] = {};
        float defenseFraction[MAX_TEAMS] = {};

        for (int i = 0; i < NUM_TEAMS; i++) {
            const SlimeClass& c = TEAM_CLASSES[i];
            startEnergy[i] = c.startEnergy;
            transferRate[i] = c.transferRate;
            reproductionWillingness[i] = c.reproductionWillingness;
            aggressionFraction[i] = c.aggressionFraction;
            defenseFraction[i] = c.defenseFraction;
        }

        glUniform1fv(glGetUniformLocation(computeProgram, "uStartEnergy"), MAX_TEAMS, startEnergy);
        glUniform1fv(glGetUniformLocation(computeProgram, "uTransferRate"), MAX_TEAMS, transferRate);
        glUniform1fv(glGetUniformLocation(computeProgram, "uReproductionWillingness"), MAX_TEAMS, reproductionWillingness);
        glUniform1fv(glGetUniformLocation(computeProgram, "uAggressionFraction"), MAX_TEAMS, aggressionFraction);
        glUniform1fv(glGetUniformLocation(computeProgram, "uDefenseFraction"), MAX_TEAMS, defenseFraction);
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

        for (GLuint tex : {textureA, textureB}) {
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16UI, width, height, 0, GL_RGBA_INTEGER, GL_UNSIGNED_SHORT, NULL);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        }

        // Static energy-source strength map: never ping-ponged, populated once in
        // initializeGrid() and never touched again.
        glGenTextures(1, &sourceMapTexture);
        glBindTexture(GL_TEXTURE_2D, sourceMapTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, width, height, 0, GL_RED, GL_FLOAT, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    }

    // Seeds NUM_TEAMS randomly placed, organically-shaped blobs (circles deformed by
    // a couple of random sine harmonics in polar coordinates) on an otherwise empty grid,
    // plus the static energy-source map: one source at each blob's center, and a handful
    // of additional neutral sources scattered elsewhere for colonies to fight over.
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
            uint16_t startEnergyWhole, startEnergyFrac;
            encodeEnergy(cls.startEnergy, startEnergyWhole, startEnergyFrac);

            float baseRadius = (float)radiusDist(gen);
            int freq1 = freqDist(gen), freq2 = freqDist(gen);
            float amp1 = ampDist(gen), amp2 = ampDist(gen);
            float phase1 = phaseDist(gen), phase2 = phaseDist(gen);

            int bound = (int)(baseRadius * 1.5f) + 1;
            for (int dy = -bound; dy <= bound; dy++) {
                for (int dx = -bound; dx <= bound; dx++) {
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
                        data[idx + 2] = startEnergyWhole;        // energy (integer part)
                        data[idx + 3] = startEnergyFrac;         // energy (fractional part)
                    }
                }
            }
        }

        glBindTexture(GL_TEXTURE_2D, textureA);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA_INTEGER, GL_UNSIGNED_SHORT, data.data());

        // Energy-source map: one home source per blob center (reusing the centers already
        // computed above), plus scattered neutral sources rejection-sampled away from every
        // blob center and every other source.
        std::vector<float> sourceData(width * height, 0.0f);
        auto placeSource = [&](int x, int y, float strength) {
            sourceData[y * width + x] = strength;
        };
        for (auto& c : centers) placeSource(c.first, c.second, HOME_SOURCE_STRENGTH);

        std::vector<std::pair<int, int>> allSources = centers;
        for (int i = 0; i < NUM_FREE_SOURCES; i++) {
            int sx = 0, sy = 0;
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
                if (farEnough) break;
            }
            allSources.push_back({ sx, sy });
            placeSource(sx, sy, FREE_SOURCE_STRENGTH);
        }

        glBindTexture(GL_TEXTURE_2D, sourceMapTexture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RED, GL_FLOAT, sourceData.data());
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
//   --stats <csv> Comma-separated list of NUM_TEAMS * 5 floats (startEnergy, transferRate,
//                 reproductionWillingness, aggressionFraction, defenseFraction, repeated per
//                 team in team order) overriding the compiled-in defaults. Lets an external
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

            if ((int)values.size() != NUM_TEAMS * 5) {
                std::cerr << "--stats expects " << (NUM_TEAMS * 5) << " comma-separated values, got "
                          << values.size() << std::endl;
                std::exit(1);
            }

            for (int t = 0; t < NUM_TEAMS; t++) {
                const float* v = &values[t * 5];
                TEAM_CLASSES[t].startEnergy             = v[0];
                TEAM_CLASSES[t].transferRate             = v[1];
                TEAM_CLASSES[t].reproductionWillingness  = v[2];
                TEAM_CLASSES[t].aggressionFraction       = v[3];
                TEAM_CLASSES[t].defenseFraction          = v[4];
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
        for (int i = 0; i < config.ticksToRun; i++) sim.step();
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
