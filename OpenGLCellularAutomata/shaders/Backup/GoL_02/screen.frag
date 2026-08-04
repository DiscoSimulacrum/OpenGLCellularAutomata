#version 430 core

out vec4 FragColor;

in vec2 TexCoord;

uniform usampler2D uTexture;

void main() {
    uvec4 cell = texture(uTexture, TexCoord);
    float alpha = float(cell.a) / 255.0;

    if (cell.r == 1u) {
        FragColor = vec4(1.0, 1.0, 1.0, 1.0);          // white material = alive (full opacity)
    } else if (cell.r == 2u) {
        FragColor = vec4(0.0, 0.4, 1.0, 1.0);          // blue material = alive (full opacity)
    } else if (cell.r == 3u) {
        FragColor = vec4(1.0, 1.0, 0.0, 1.0);          // yellow material = alive (full opacity)
    } else if (cell.r == 4u) {
        FragColor = vec4(1.0, 0.0, 0.0, 1.0);          // red material = alive (full opacity)
    } else {
        // Dead cell - use the stored dead type (in G channel) for color
        if (alpha > 0.0) {
            if (cell.g == 1u) {
                FragColor = vec4(1.0, 1.0, 1.0, alpha);    // fading white
            } else if (cell.g == 2u) {
                FragColor = vec4(0.0, 0.4, 1.0, alpha);    // fading blue
            } else if (cell.g == 3u) {
                FragColor = vec4(1.0, 1.0, 0.0, alpha);    // fading yellow
            } else {
                FragColor = vec4(1.0, 1.0, 1.0, alpha);    // default fading white
            }
        } else {
            FragColor = vec4(0.0, 0.0, 0.0, 1.0);          // fully transparent = black background
        }
    }
}