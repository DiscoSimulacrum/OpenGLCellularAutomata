#version 430 core

out vec4 FragColor;

in vec2 TexCoord;

uniform usampler2D uTexture;

const int MAX_PALETTE_COLORS = 9;
uniform vec3 uPalette[MAX_PALETTE_COLORS]; // colors of the palette chosen for this run
uniform int uPaletteSize;                  // how many entries of uPalette are actually valid

void main() {
    uvec4 cell = texture(uTexture, TexCoord);
    uint team = cell.r;

    if (team == 0u) {
        FragColor = vec4(0.0, 0.0, 0.0, 1.0); // unclaimed territory: matches the fully-faded HP=0 color below
    } else {
        vec3 color = uPalette[int(team - 1u) % uPaletteSize];
        float hpFrac = float(cell.b) / 255.0; // dim young/dying cells, brighten mature/healthy ones
        color *= hpFrac; // no floor: fades all the way to black as HP approaches 0, so death is a smooth fade rather than a pop
        FragColor = vec4(color, 1.0);
    }
}
