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
const float EXPAND_CHANCE = 0.08f;    //Per-neighbor chance per tick to claim unclaimed territory
const float INVADE_CHANCE = 0.25f;    //Aggressiveness multiplier applied to the attacker-attack-vs-defender-HP ratio
const float REPRODUCTION_CUTOFF = 0.8f; //Fraction of a cell's lifespan past which it can no longer expand or invade

// Per-color archetype: controls a colony's starting hit points/attack and how those stats
// grow with age (stat(age) = min(maxStat, baseStat + growth * age)); HP peaks the same way
// but then declines linearly back to 0 as age approaches lifespan, so cells visibly weaken
// and fade out as death nears. At age == lifespan a cell dies outright (hard cutoff),
// reverting to unclaimed territory and letting long-held interiors hollow out over time.
// allyHPBonus/allyAttackBonus grant extra HP/attack per same-team neighbor (0-8), rewarding
// dense, contiguous blobs over thin, isolated tendrils.
struct SlimeClass {
    const char* name;
    float baseHP, maxHP, hpGrowth;
    float baseAttack, maxAttack, attackGrowth;
    float lifespan;
    float allyHPBonus, allyAttackBonus;
};

// Note: age is stored as a 16-bit channel in the cell texture (saturates at 65535), giving
// plenty of headroom for lifespans well beyond the old 8-bit ceiling. Longer lifespans mean
// longer HP-decline windows too, which is what makes the pre-death fade look smooth instead
// of abrupt.
const SlimeClass DEFAULT_TEAM_CLASSES[NUM_TEAMS] = {
    { "Tank",         180.0f, 220.0f, 3.0f,  60.0f,  120.0f, 3.0f, 880.0f, 5.0f, 4.0f }, // very tough, weak attacker, ages slowly, modest cohesion
    { "Berserker",      60.0f, 140.0f, 1.0f, 120.0f, 255.0f, 4.0f, 520.0f, 1.0f, 1.0f }, // fragile, lethal fast, burns out young, fights alone
    { "Balanced",       120.0f, 200.0f, 2.0f, 90.0f, 150.0f, 2.0f, 720.0f, 5.0f, 5.0f }, // no strong strengths or weaknesses
    { "Glass Cannon",   40.0f,  90.0f, 0.5f, 150.0f, 255.0f, 5.0f, 400.0f, 3.0f, 1.0f }, // devastating once mature, shortest-lived, fights alone
    { "Bulwark",        200.0f, 240.0f, 1.0f,  60.0f, 90.0f, 0.5f, 980.0f, 7.0f, 4.0f }, // extremely tough, longest-lived, strong fortress bonus
    { "Swarm",          70.0f, 150.0f, 2.5f,  80.0f, 180.0f, 3.0f, 640.0f, 9.0f, 9.0f }, // moderate stats, thrives most on numbers
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
        glUniform1f(glGetUniformLocation(computeProgram, "uInvadeChance"), INVADE_CHANCE);
        glUniform1f(glGetUniformLocation(computeProgram, "uReproductionCutoff"), REPRODUCTION_CUTOFF);

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

    // Uploads each team's growth-curve constants to the compute shader once; these never
    // change during a run, so there's no need to re-set them every step().
    void uploadTeamClasses() {
        glUseProgram(computeProgram);

        float baseHP[MAX_TEAMS] = {}, maxHP[MAX_TEAMS] = {}, hpGrowth[MAX_TEAMS] = {};
        float baseAtk[MAX_TEAMS] = {}, maxAtk[MAX_TEAMS] = {}, atkGrowth[MAX_TEAMS] = {};
        float lifespan[MAX_TEAMS] = {};
        float allyHPBonus[MAX_TEAMS] = {}, allyAttackBonus[MAX_TEAMS] = {};

        for (int i = 0; i < NUM_TEAMS; i++) {
            const SlimeClass& c = TEAM_CLASSES[i];
            baseHP[i] = c.baseHP;   maxHP[i] = c.maxHP;   hpGrowth[i] = c.hpGrowth;
            baseAtk[i] = c.baseAttack; maxAtk[i] = c.maxAttack; atkGrowth[i] = c.attackGrowth;
            lifespan[i] = c.lifespan;
            allyHPBonus[i] = c.allyHPBonus; allyAttackBonus[i] = c.allyAttackBonus;
        }

        glUniform1fv(glGetUniformLocation(computeProgram, "uBaseHP"), MAX_TEAMS, baseHP);
        glUniform1fv(glGetUniformLocation(computeProgram, "uMaxHP"), MAX_TEAMS, maxHP);
        glUniform1fv(glGetUniformLocation(computeProgram, "uHPGrowth"), MAX_TEAMS, hpGrowth);
        glUniform1fv(glGetUniformLocation(computeProgram, "uBaseAttack"), MAX_TEAMS, baseAtk);
        glUniform1fv(glGetUniformLocation(computeProgram, "uMaxAttack"), MAX_TEAMS, maxAtk);
        glUniform1fv(glGetUniformLocation(computeProgram, "uAttackGrowth"), MAX_TEAMS, atkGrowth);
        glUniform1fv(glGetUniformLocation(computeProgram, "uLifespan"), MAX_TEAMS, lifespan);
        glUniform1fv(glGetUniformLocation(computeProgram, "uAllyHPBonus"), MAX_TEAMS, allyHPBonus);
        glUniform1fv(glGetUniformLocation(computeProgram, "uAllyAttackBonus"), MAX_TEAMS, allyAttackBonus);
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
    }

    // Seeds NUM_TEAMS randomly placed, organically-shaped blobs (circles deformed by
    // a couple of random sine harmonics in polar coordinates) on an otherwise empty grid.
    // Each blob starts at age 0 with its team class's base HP/attack.
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
            uint16_t startHP = (uint16_t)std::min(255.0f, std::max(0.0f, cls.baseHP));
            uint16_t startAttack = (uint16_t)std::min(255.0f, std::max(0.0f, cls.baseAttack));

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
                        data[idx + 0] = (uint16_t)team;   // team
                        data[idx + 1] = 0;                 // age
                        data[idx + 2] = startHP;           // hit points
                        data[idx + 3] = startAttack;       // attack
                    }
                }
            }
        }

        glBindTexture(GL_TEXTURE_2D, textureA);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA_INTEGER, GL_UNSIGNED_SHORT, data.data());
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
//   --stats <csv> Comma-separated list of NUM_TEAMS * 9 floats (baseHP, maxHP, hpGrowth,
//                 baseAttack, maxAttack, attackGrowth, lifespan, allyHPBonus,
//                 allyAttackBonus, repeated per team in team order) overriding the
//                 compiled-in defaults. Lets an external evolutionary-algorithm driver feed
//                 in candidate parameter sets without recompiling. Works in either mode.
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

            if ((int)values.size() != NUM_TEAMS * 9) {
                std::cerr << "--stats expects " << (NUM_TEAMS * 9) << " comma-separated values, got "
                          << values.size() << std::endl;
                std::exit(1);
            }

            for (int t = 0; t < NUM_TEAMS; t++) {
                const float* v = &values[t * 9];
                TEAM_CLASSES[t].baseHP          = v[0];
                TEAM_CLASSES[t].maxHP           = v[1];
                TEAM_CLASSES[t].hpGrowth        = v[2];
                TEAM_CLASSES[t].baseAttack      = v[3];
                TEAM_CLASSES[t].maxAttack       = v[4];
                TEAM_CLASSES[t].attackGrowth    = v[5];
                TEAM_CLASSES[t].lifespan        = v[6];
                TEAM_CLASSES[t].allyHPBonus     = v[7];
                TEAM_CLASSES[t].allyAttackBonus = v[8];
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
