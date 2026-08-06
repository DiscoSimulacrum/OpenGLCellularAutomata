#version 430 core

out vec4 FragColor;

in vec2 TexCoord;

uniform usampler2D uTexture;
uniform usampler2D uConductivity; // R=N, G=E, B=S, A=W -- see cellular.comp
uniform float uTime; // wall-clock seconds, drives the source/deposit marker pulse

const int MAX_PALETTE_COLORS = 9;
uniform vec3 uPalette[MAX_PALETTE_COLORS]; // colors of the palette chosen for this run
uniform int uPaletteSize;                  // how many entries of uPalette are actually valid

uniform float uEnergyCapacity; // shared by every team now, used to normalize energy brightness

// Team-ID sentinels shared with cellular.comp -- sources/deposits live in the main grid now.
const uint DEPOSIT_ID = 254u;
const uint SOURCE_ID = 255u;

void main() {
    uvec4 cell = texture(uTexture, TexCoord);
    uint team = cell.r;

    vec3 color;
    if (team == 0u) {
        color = vec3(0.0); // unclaimed territory
    } else if (team == SOURCE_ID || team == DEPOSIT_ID) {
        color = vec3(0.0); // the pulsing marker below provides all the visual signal
    } else {
        vec3 base = uPalette[int(team - 1u) % uPaletteSize];
        // B/A are the high/low 16 bits of one packed energyFP integer (see cellular.comp's
        // energyOf); ENERGY_SCALE (256) converts back to the real units uEnergyCapacity is
        // expressed in.
        float energy = (float(cell.b) * 65536.0 + float(cell.a)) / 256.0;
        float energyFrac = clamp(energy / uEnergyCapacity, 0.0, 1.0);

        // Cube-root curve, not a straight lerp: most cells live well under uEnergyCapacity
        // (it's sized for the theoretical max, not typical holdings -- e.g. startEnergy 300 /
        // capacity 1500 is a 0.2 fraction), so a linear map left the whole sim looking dim and
        // washed out most of the time. pow(x, 1/3) boosts low/mid fractions hard (0.2 -> ~0.58)
        // while still hitting exactly 0 at 0 and 1 at capacity, so death is still a smooth fade
        // to black rather than a pop -- just far brighter in the range cells actually live in.
        float brightness = pow(energyFrac, 1.0 / 3.0);
        color = base * brightness;

        // Debug/verification overlay: tint toward white proportional to the STRONGEST of the
        // 4 directional conductivities (same 16383.0 scale, range [0,4.0] per channel, as
        // cellular.comp's decodeConductivity), so reinforced "trunk line" cells visibly stand
        // out from ordinary territory of the same team/energy level regardless of which
        // direction the trunk runs. Useful for confirming whether the mechanism actually
        // produces thin channels versus a diffuse blur, and whether routes are specializing
        // per-direction rather than reinforcing uniformly in all 4 at once.
        uvec4 cond = texture(uConductivity, TexCoord);
        uint strongest = max(max(cond.r, cond.g), max(cond.b, cond.a));
        float conductivity = float(strongest) / 16383.0;
        float conductivityFrac = clamp(conductivity / 4.0, 0.0, 1.0);
        color = mix(color, vec3(1.0), conductivityFrac * 0.6);
    }

    // Pulsing markers so sources/deposits read clearly on screen: white for a renewable
    // source, dim amber for a lootable deposit -- distinct so the two don't read as the same
    // thing, since neither has a team color to lean on anymore (both are communal/ownerless).
    if (team == SOURCE_ID) {
        float pulse = 0.6 + 0.4 * sin(uTime * 2.0);
        color = mix(color, vec3(1.0), pulse);
    } else if (team == DEPOSIT_ID) {
        float pulse = 0.6 + 0.4 * sin(uTime * 3.0);
        color = mix(color, vec3(0.55, 0.35, 0.1), pulse);
    }

    FragColor = vec4(color, 1.0);
}
