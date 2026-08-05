#version 430 core

out vec4 FragColor;

in vec2 TexCoord;

uniform usampler2D uTexture;
uniform sampler2D uSourceMap; // energy-source strength at this location (0 = not a source)
uniform float uTime;          // wall-clock seconds, drives the source-marker pulse

const int MAX_PALETTE_COLORS = 9;
uniform vec3 uPalette[MAX_PALETTE_COLORS]; // colors of the palette chosen for this run
uniform int uPaletteSize;                  // how many entries of uPalette are actually valid

uniform float uEnergyCapacity; // shared by every team now, used to normalize energy brightness

void main() {
    uvec4 cell = texture(uTexture, TexCoord);
    uint team = cell.r;
    float sourceStrength = texture(uSourceMap, TexCoord).r;

    vec3 color;
    vec3 base = vec3(0.0);
    if (team == 0u) {
        color = vec3(0.0); // unclaimed territory
    } else {
        base = uPalette[int(team - 1u) % uPaletteSize];
        float energy = float(cell.b) + float(cell.a) / 256.0; // mirrors cellular.comp's decodeEnergy
        float energyFrac = clamp(energy / uEnergyCapacity, 0.0, 1.0);
        color = base * energyFrac; // no floor: fades all the way to black as energy runs out, so death is a smooth fade rather than a pop

        // Debug/verification overlay: tint toward white proportional to conductivity (see
        // cellular.comp's decodeConductivity -- same 16383.0 scale, range [0,4.0]), so
        // reinforced "trunk line" cells visibly stand out from ordinary territory of the
        // same team/energy level. Useful for confirming whether the mechanism actually
        // produces a thin channel versus a diffuse blur.
        float conductivity = float(cell.g) / 16383.0;
        float conductivityFrac = clamp(conductivity / 4.0, 0.0, 1.0);
        color = mix(color, vec3(1.0), conductivityFrac * 0.6);
    }

    if (sourceStrength > 0.0) {
        // Pulsing marker so strategically important source tiles read clearly on screen,
        // whether currently neutral (pulses white) or owned (pulses toward the owner's
        // full-brightness color, distinct from the possibly energy-dimmed base color).
        float pulse = 0.6 + 0.4 * sin(uTime * 2.0);
        vec3 markerColor = (team == 0u) ? vec3(1.0) : base;
        color = mix(color, markerColor, pulse * 0.5);
    }

    FragColor = vec4(color, 1.0);
}
