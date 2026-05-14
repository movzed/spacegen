#version 410 core

// Fullscreen triangle — no VBO. Positions are generated from gl_VertexID.
// Draw with glDrawArrays(GL_TRIANGLES, 0, 3) bound to a null VAO.
//
// Vertex positions (NDC):
//   0: (-1, -1)
//   1: ( 3, -1)
//   2: (-1,  3)
//
// UV range [0,1] across the viewport, [0,2] outside — clipped by rasteriser.

out vec2 vUV;

void main() {
    vec2 pos = vec2(
        (gl_VertexID == 1) ? 3.0 : -1.0,
        (gl_VertexID == 2) ? 3.0 : -1.0
    );
    vUV         = pos * 0.5 + 0.5;
    gl_Position = vec4(pos, 0.0, 1.0);
}
