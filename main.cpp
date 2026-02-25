#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <string>
#include <iostream>
#include <chrono>

#include <emscripten/emscripten.h>

#define GLFW_INCLUDE_ES3
#include <GLFW/glfw3.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

// ---------- Settings ----------
const int SCR_W_DEFAULT = 1280;
const int SCR_H_DEFAULT = 720;

const double G_CONST = 6.67430e-11;
const double DISTANCE_SCALE = 1e10;
const double MASS_SCALE = 1e20;
const double TIME_SCALE = 60*60*24; // 1 sec -> 1 day
double timeScaleMultiplier = 1.0;

const int SPHERE_LAT = 32;
const int SPHERE_LONG = 32;
const int SUN_SPHERE_LAT = 48;
const int SUN_SPHERE_LONG = 48;

float fov = 80.0f;

// ---------- Embedded shaders (GLSL ES 3.00) ----------

// --- Starfield shaders ---
const char *starVertexShaderSrc = R"glsl(#version 300 es
precision highp float;
layout(location = 0) in vec3 aPos;
layout(location = 1) in float aBrightness;
layout(location = 2) in float aSize;

uniform mat4 uView;
uniform mat4 uProj;

out float vBrightness;

void main() {
    vBrightness = aBrightness;
    gl_Position = uProj * uView * vec4(aPos, 1.0);
    gl_PointSize = aSize;
}
)glsl";

const char *starFragmentShaderSrc = R"glsl(#version 300 es
precision highp float;
in float vBrightness;
out vec4 FragColor;

void main() {
    // Soft circular point
    vec2 pc = gl_PointCoord * 2.0 - 1.0;
    float d = dot(pc, pc);
    if (d > 1.0) discard;
    float alpha = vBrightness * (1.0 - d * d);
    // Slight blue-white tint
    vec3 color = mix(vec3(0.8, 0.85, 1.0), vec3(1.0, 0.95, 0.8), vBrightness);
    FragColor = vec4(color * alpha, alpha);
}
)glsl";

// --- Sun surface shader (procedural fiery surface) ---
const char *sunVertexShaderSrc = R"glsl(#version 300 es
precision highp float;
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProj;

out vec3 vNormal;
out vec3 vObjPos;

void main() {
    vObjPos = aPos;
    vNormal = aNormal;
    gl_Position = uProj * uView * uModel * vec4(aPos, 1.0);
}
)glsl";

const char *sunFragmentShaderSrc = R"glsl(#version 300 es
precision highp float;
in vec3 vNormal;
in vec3 vObjPos;
out vec4 FragColor;

uniform float uTime;

// Simplex-like hash noise
vec3 hash3(vec3 p) {
    p = vec3(dot(p, vec3(127.1, 311.7, 74.7)),
             dot(p, vec3(269.5, 183.3, 246.1)),
             dot(p, vec3(113.5, 271.9, 124.6)));
    return -1.0 + 2.0 * fract(sin(p) * 43758.5453123);
}

float noise3d(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    vec3 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(dot(hash3(i + vec3(0,0,0)), f - vec3(0,0,0)),
                       dot(hash3(i + vec3(1,0,0)), f - vec3(1,0,0)), u.x),
                   mix(dot(hash3(i + vec3(0,1,0)), f - vec3(0,1,0)),
                       dot(hash3(i + vec3(1,1,0)), f - vec3(1,1,0)), u.x), u.y),
               mix(mix(dot(hash3(i + vec3(0,0,1)), f - vec3(0,0,1)),
                       dot(hash3(i + vec3(1,0,1)), f - vec3(1,0,1)), u.x),
                   mix(dot(hash3(i + vec3(0,1,1)), f - vec3(0,1,1)),
                       dot(hash3(i + vec3(1,1,1)), f - vec3(1,1,1)), u.x), u.y), u.z);
}

float fbm(vec3 p) {
    float v = 0.0;
    float a = 0.5;
    for (int i = 0; i < 5; i++) {
        v += a * noise3d(p);
        p *= 2.0;
        a *= 0.5;
    }
    return v;
}

void main() {
    vec3 p = vObjPos * 3.0 + vec3(0.0, 0.0, uTime * 0.15);
    float n = fbm(p) * 0.5 + 0.5;
    float n2 = fbm(p * 2.0 + vec3(uTime * 0.1)) * 0.5 + 0.5;

    // Hot core colors: white -> yellow -> orange -> red
    vec3 col1 = vec3(1.0, 0.95, 0.8);   // white-yellow (hottest)
    vec3 col2 = vec3(1.0, 0.7, 0.2);    // orange
    vec3 col3 = vec3(1.0, 0.3, 0.05);   // deep orange-red

    float t = n * n2;
    vec3 col = mix(col3, col2, smoothstep(0.2, 0.5, t));
    col = mix(col, col1, smoothstep(0.5, 0.85, t));

    // Limb darkening
    float rim = 1.0 - max(dot(normalize(vNormal), normalize(vObjPos + vec3(0.0, 0.0, 1.0))), 0.0);
    col *= mix(1.0, 0.5, rim * rim);

    // Make it bright (HDR feel)
    col *= 1.3;

    FragColor = vec4(col, 1.0);
}
)glsl";

// --- Sun glow billboard shader ---
const char *glowVertexShaderSrc = R"glsl(#version 300 es
precision highp float;
layout(location = 0) in vec2 aPos;

uniform mat4 uView;
uniform mat4 uProj;
uniform vec3 uCenter;
uniform float uSize;

out vec2 vUV;

void main() {
    vUV = aPos; // -1..1
    // Billboard: extract camera right & up from view matrix
    vec3 right = vec3(uView[0][0], uView[1][0], uView[2][0]);
    vec3 up    = vec3(uView[0][1], uView[1][1], uView[2][1]);
    vec3 worldPos = uCenter + right * aPos.x * uSize + up * aPos.y * uSize;
    gl_Position = uProj * uView * vec4(worldPos, 1.0);
}
)glsl";

const char *glowFragmentShaderSrc = R"glsl(#version 300 es
precision highp float;
in vec2 vUV;
out vec4 FragColor;

uniform vec3 uColor;

void main() {
    float d = length(vUV);
    // Multi-layered glow
    float glow = 0.0;
    glow += 0.6 * exp(-d * 2.5);       // wide outer glow
    glow += 0.4 * exp(-d * 6.0);       // medium glow
    glow += 0.3 * exp(-d * 15.0);      // tight inner glow
    glow = clamp(glow, 0.0, 1.0);
    FragColor = vec4(uColor * glow, glow);
}
)glsl";

// --- Planet shader ---
const char *vertexShaderSrc = R"glsl(#version 300 es
precision highp float;
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProj;

out vec3 vNormal;
out vec3 vWorldPos;

void main() {
    vec4 worldPos = uModel * vec4(aPos, 1.0);
    vWorldPos = worldPos.xyz;
    vNormal = mat3(transpose(inverse(uModel))) * aNormal;
    gl_Position = uProj * uView * worldPos;
}
)glsl";

const char *fragmentShaderSrc = R"glsl(#version 300 es
precision highp float;
in vec3 vNormal;
in vec3 vWorldPos;
out vec4 FragColor;

uniform vec3 uColor;
uniform vec3 uLightPos;

void main() {
    vec3 N = normalize(vNormal);
    vec3 L = normalize(uLightPos - vWorldPos);
    float diff = max(dot(N, L), 0.0);
    vec3 viewDir = normalize(-vWorldPos);
    vec3 reflectDir = reflect(-L, N);
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), 16.0);
    vec3 ambient = 0.08 * uColor;
    vec3 color = ambient + (0.9 * diff) * uColor + 0.4 * spec * vec3(1.0);
    FragColor = vec4(color, 1.0);
}
)glsl";

const char *orbitVertexShaderSrc = R"glsl(#version 300 es
precision highp float;
layout(location = 0) in vec3 aPos;
layout(location = 1) in float aAlpha;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProj;

out float vAlpha;

void main() {
    vAlpha = aAlpha;
    gl_Position = uProj * uView * uModel * vec4(aPos, 1.0);
}
)glsl";

const char *orbitFragmentShaderSrc = R"glsl(#version 300 es
precision highp float;
in float vAlpha;
out vec4 FragColor;

uniform vec3 uColor;

void main() {
    FragColor = vec4(uColor, vAlpha);
}
)glsl";

// ---------- GL helpers ----------
static void checkCompile(GLuint shader) {
    GLint ok; glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if(!ok) {
        char buf[8192]; glGetShaderInfoLog(shader, sizeof(buf), nullptr, buf);
        std::cerr << "Shader compile error: " << buf << std::endl;
    }
}
static void checkLink(GLuint prog) {
    GLint ok; glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if(!ok) {
        char buf[8192]; glGetProgramInfoLog(prog, sizeof(buf), nullptr, buf);
        std::cerr << "Program link error: " << buf << std::endl;
    }
}

// ---------- Mesh (UV sphere) ----------
struct Mesh {
    std::vector<float> verts; // x,y,z,nx,ny,nz
    std::vector<unsigned int> idx;
    GLuint vao=0, vbo=0, ebo=0;
    void build(int lat, int lon) {
        verts.clear(); idx.clear();
        for(int y=0;y<=lat;y++){
            float v = (float)y / lat;
            float theta1 = v * glm::pi<float>();
            for(int x=0;x<=lon;x++){
                float u = (float)x / lon;
                float theta2 = u * glm::two_pi<float>();
                float sx = sin(theta1)*cos(theta2);
                float sy = cos(theta1);
                float sz = sin(theta1)*sin(theta2);
                verts.push_back(sx); verts.push_back(sy); verts.push_back(sz);
                verts.push_back(sx); verts.push_back(sy); verts.push_back(sz);
            }
        }
        for(int y=0;y<lat;y++){
            for(int x=0;x<lon;x++){
                int i1 = y*(lon+1) + x;
                int i2 = i1 + lon + 1;
                idx.push_back(i1); idx.push_back(i2); idx.push_back(i1+1);
                idx.push_back(i1+1); idx.push_back(i2); idx.push_back(i2+1);
            }
        }
        if(vao==0) glGenVertexArrays(1,&vao);
        if(vbo==0) glGenBuffers(1,&vbo);
        if(ebo==0) glGenBuffers(1,&ebo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, verts.size()*sizeof(float), verts.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size()*sizeof(unsigned int), idx.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6*sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6*sizeof(float), (void*)(3*sizeof(float)));
        glEnableVertexAttribArray(1);
        glBindVertexArray(0);
    }
    void draw() {
        glBindVertexArray(vao);
        glDrawElements(GL_TRIANGLES, (GLsizei)idx.size(), GL_UNSIGNED_INT, nullptr);
        glBindVertexArray(0);
    }
} sphereMesh, sunMesh;

// ---------- Starfield ----------
const int NUM_STARS = 8000;
const int NUM_MILKYWAY_EXTRA = 4000; // extra dense milky way band stars
GLuint starVAO = 0, starVBO = 0;
GLuint gStarProg = 0;
GLint locStarView, locStarProj;

void buildStarfield() {
    // 5 floats per star: x,y,z, brightness, size
    std::vector<float> data;
    data.reserve((NUM_STARS + NUM_MILKYWAY_EXTRA) * 5);

    // Galactic plane tilt: 60 degrees around X axis
    // This makes the milky way band sweep diagonally across the sky
    const float galTilt = glm::radians(60.0f);
    const float cosT = cos(galTilt), sinT = sin(galTilt);
    // Returns distance from the tilted galactic plane for a given (x,y,z) direction
    auto galacticDist = [&](float x, float y, float z) -> float {
        // Rotate point by -tilt around X, then measure y component
        float ry = y * cosT + z * sinT;
        float len = sqrt(x*x + y*y + z*z);
        if (len < 0.001f) return 1.0f;
        return ry / len; // normalized distance from plane (-1..1)
    };

    // Simple LCG random for deterministic starfield
    unsigned int seed = 42u;
    auto rng = [&seed]() -> float {
        seed = seed * 1103515245u + 12345u;
        return (float)((seed >> 16) & 0x7FFF) / 32767.0f;
    };

    for (int i = 0; i < NUM_STARS; i++) {
        // Random direction on sphere
        float u = rng() * 2.0f - 1.0f;
        float theta = rng() * glm::two_pi<float>();
        float r2 = sqrt(1.0f - u * u);

        float x = r2 * cos(theta);
        float y = u;
        float z = r2 * sin(theta);

        // Place at large distance
        float dist = 400.0f + rng() * 100.0f;
        x *= dist; y *= dist; z *= dist;

        // Milky way band: denser and brighter near tilted galactic plane
        float gd = galacticDist(x, y, z);
        float galacticFactor = exp(-(gd * gd) / 0.02f);
        float brightness = rng() * 0.3f + 0.1f;
        brightness += galacticFactor * 0.5f * rng();
        brightness = std::min(brightness, 1.0f);

        float size = 1.0f + rng() * 2.0f;
        // Milky way stars can be slightly larger
        if (galacticFactor > 0.5f) size += rng() * 1.5f;

        data.push_back(x);
        data.push_back(y);
        data.push_back(z);
        data.push_back(brightness);
        data.push_back(size);
    }

    // Extra milky way band stars - concentrated along tilted galactic plane
    for (int i = 0; i < NUM_MILKYWAY_EXTRA; i++) {
        float lon = rng() * glm::two_pi<float>();
        // Gaussian-ish distribution in latitude, tight around galactic plane
        float lat = (rng() + rng() + rng()) / 3.0f * 2.0f - 1.0f; // central limit
        lat *= 0.15f; // narrow band

        float dist = 400.0f + rng() * 100.0f;
        float cosLat = cos(lat);
        float x = cosLat * cos(lon) * dist;
        float y = sin(lat) * dist;
        float z = cosLat * sin(lon) * dist;

        // Rotate by galactic tilt around X axis
        float ry = y * cosT - z * sinT;
        float rz = y * sinT + z * cosT;
        y = ry;
        z = rz;

        float brightness = 0.15f + rng() * 0.5f;
        float size = 0.8f + rng() * 1.5f;

        // Some brighter clumps
        if (rng() > 0.85f) {
            brightness = 0.6f + rng() * 0.4f;
            size += 1.0f;
        }

        data.push_back(x);
        data.push_back(y);
        data.push_back(z);
        data.push_back(std::min(brightness, 1.0f));
        data.push_back(size);
    }

    glGenVertexArrays(1, &starVAO);
    glGenBuffers(1, &starVBO);
    glBindVertexArray(starVAO);
    glBindBuffer(GL_ARRAY_BUFFER, starVBO);
    glBufferData(GL_ARRAY_BUFFER, data.size() * sizeof(float), data.data(), GL_STATIC_DRAW);
    // position
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), 0);
    glEnableVertexAttribArray(0);
    // brightness
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    // size
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(4 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glBindVertexArray(0);
}

void drawStars(const glm::mat4 &view, const glm::mat4 &proj) {
    glUseProgram(gStarProg);
    // Recenter the star skybox on the camera so it never moves
    glm::mat4 skyView = glm::mat4(glm::mat3(view)); // strip translation
    glUniformMatrix4fv(locStarView, 1, GL_FALSE, glm::value_ptr(skyView));
    glUniformMatrix4fv(locStarProj, 1, GL_FALSE, glm::value_ptr(proj));
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE); // additive for stars
    glDepthMask(GL_FALSE); // don't write to depth
    glBindVertexArray(starVAO);
    glDrawArrays(GL_POINTS, 0, NUM_STARS + NUM_MILKYWAY_EXTRA);
    glBindVertexArray(0);
    glDepthMask(GL_TRUE);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

// ---------- Sun glow billboard ----------
GLuint glowVAO = 0, glowVBO = 0;
GLuint gGlowProg = 0;
GLint locGlowView, locGlowProj, locGlowCenter, locGlowSize, locGlowColor;

void buildGlowQuad() {
    float quad[] = {
        -1.0f, -1.0f,
         1.0f, -1.0f,
        -1.0f,  1.0f,
         1.0f, -1.0f,
         1.0f,  1.0f,
        -1.0f,  1.0f,
    };
    glGenVertexArrays(1, &glowVAO);
    glGenBuffers(1, &glowVBO);
    glBindVertexArray(glowVAO);
    glBindBuffer(GL_ARRAY_BUFFER, glowVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), 0);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);
}

void drawSunGlow(const glm::mat4 &view, const glm::mat4 &proj, const glm::vec3 &sunPos, float size) {
    glUseProgram(gGlowProg);
    glUniformMatrix4fv(locGlowView, 1, GL_FALSE, glm::value_ptr(view));
    glUniformMatrix4fv(locGlowProj, 1, GL_FALSE, glm::value_ptr(proj));
    glUniform3fv(locGlowCenter, 1, glm::value_ptr(sunPos));
    glUniform1f(locGlowSize, size);
    glUniform3f(locGlowColor, 1.0f, 0.85f, 0.4f);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE); // additive glow
    glDepthMask(GL_FALSE);
    glBindVertexArray(glowVAO);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glDepthMask(GL_TRUE);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

// ---------- Sun shader program ----------
GLuint gSunProg = 0;
GLint locSunM, locSunV, locSunP, locSunTime;
float gTime = 0.0f;

// ---------- Physics bodies ----------
const int ORBIT_TRAIL_MAX = 2048;  // max trail points per body
const int ORBIT_TRAIL_RECORD_INTERVAL = 3; // record every N frames
const int FUTURE_TRAIL_POINTS = 512; // number of predicted future positions
const int FUTURE_PREDICT_INTERVAL = 60; // re-predict every N frames
const double FUTURE_STEP_DT = TIME_SCALE * 2.0; // simulate ~2 days per step (~2.8 yr total)

struct Body {
    std::string name;
    double mass;
    glm::dvec3 pos;
    glm::dvec3 vel;
    float radius;
    glm::vec3 color;
    bool isStar = false;

    // orbit trail - stored as interleaved (x, y, z, alpha) per vertex
    std::vector<float> trailData; // interleaved: x,y,z,alpha
    std::vector<glm::vec3> trailPositions; // raw positions for ring buffer
    GLuint trailVAO = 0, trailVBO = 0;
    bool trailDirty = false;

    void recordTrailPoint() {
        glm::vec3 p((float)pos.x, (float)pos.y, (float)pos.z);
        if (trailPositions.size() >= (size_t)ORBIT_TRAIL_MAX) {
            trailPositions.erase(trailPositions.begin());
        }
        trailPositions.push_back(p);
        trailDirty = true;
    }

    void uploadTrail() {
        if (!trailDirty || trailPositions.size() < 2) return;
        if (trailVAO == 0) {
            glGenVertexArrays(1, &trailVAO);
            glGenBuffers(1, &trailVBO);
            glBindVertexArray(trailVAO);
            glBindBuffer(GL_ARRAY_BUFFER, trailVBO);
            // stride = 4 floats (x, y, z, alpha)
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 4 * sizeof(float), 0);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(3 * sizeof(float)));
            glEnableVertexAttribArray(1);
            glBindVertexArray(0);
        }
        // Build interleaved data with fading alpha
        trailData.clear();
        size_t n = trailPositions.size();
        trailData.reserve(n * 4);
        for (size_t i = 0; i < n; i++) {
            float t = (float)i / (float)(n - 1); // 0 = oldest, 1 = newest
            float alpha = 0.05f + t * 0.85f; // fade from 0.05 to 0.9
            trailData.push_back(trailPositions[i].x);
            trailData.push_back(trailPositions[i].y);
            trailData.push_back(trailPositions[i].z);
            trailData.push_back(alpha);
        }
        glBindBuffer(GL_ARRAY_BUFFER, trailVBO);
        glBufferData(GL_ARRAY_BUFFER, trailData.size() * sizeof(float), trailData.data(), GL_DYNAMIC_DRAW);
        trailDirty = false;
    }

    void drawTrail() {
        if (trailPositions.size() < 2 || trailVAO == 0) return;
        glBindVertexArray(trailVAO);
        glDrawArrays(GL_LINE_STRIP, 0, (GLsizei)trailPositions.size());
        glBindVertexArray(0);
    }

    // future (predicted) trail - also interleaved (x, y, z, alpha)
    std::vector<glm::vec3> futureTrail;
    std::vector<float> futureData;
    GLuint futureVAO = 0, futureVBO = 0;
    bool futureDirty = false;

    void uploadFutureTrail() {
        if (!futureDirty || futureTrail.size() < 2) return;
        if (futureVAO == 0) {
            glGenVertexArrays(1, &futureVAO);
            glGenBuffers(1, &futureVBO);
            glBindVertexArray(futureVAO);
            glBindBuffer(GL_ARRAY_BUFFER, futureVBO);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 4 * sizeof(float), 0);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(3 * sizeof(float)));
            glEnableVertexAttribArray(1);
            glBindVertexArray(0);
        }
        // Build interleaved data with uniform alpha
        futureData.clear();
        size_t n = futureTrail.size();
        futureData.reserve(n * 4);
        for (size_t i = 0; i < n; i++) {
            futureData.push_back(futureTrail[i].x);
            futureData.push_back(futureTrail[i].y);
            futureData.push_back(futureTrail[i].z);
            futureData.push_back(0.5f); // semi-transparent
        }
        glBindBuffer(GL_ARRAY_BUFFER, futureVBO);
        glBufferData(GL_ARRAY_BUFFER, futureData.size() * sizeof(float), futureData.data(), GL_DYNAMIC_DRAW);
        futureDirty = false;
    }

    void drawFutureTrail() {
        if (futureTrail.size() < 2 || futureVAO == 0) return;
        glBindVertexArray(futureVAO);
        glDrawArrays(GL_LINE_STRIP, 0, (GLsizei)futureTrail.size());
        glBindVertexArray(0);
    }
};
std::vector<Body> bodies;
int trailFrameCounter = 0;
int futureFrameCounter = 0;

void setupSolarSystem() {
    bodies.clear();

    const double AU = 1.496e11; // 1 Astronomical Unit in meters

    // convert polar angle (degrees) and distance to x,z
    auto polar_to_cart = [](double dist, double angle_deg, double &out_x, double &out_z){
        double angle_rad = glm::radians(angle_deg);
        out_x = dist * cos(angle_rad);
        out_z = dist * sin(angle_rad);
    };

    // compute perpendicular velocity components in XZ plane given speed and angle (degrees)
    auto velocity_from_speed_angle = [](double speed, double angle_deg, double &out_vx, double &out_vz){
        double angle_rad = glm::radians(angle_deg);
        out_vx = -speed * sin(angle_rad);
        out_vz =  speed * cos(angle_rad);
    };

    auto push_body = [](const std::string &name, double mass,
                        double px, double py, double pz,
                        double vx, double vy, double vz,
                        float radius, const glm::vec3 &color){
        bodies.push_back({
            name,
            mass,
            {px, py, pz},
            {vx, vy, vz},
            radius,
            color
        });
    };

    push_body("Sun", 1.9885e30, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 6.9634e8f, {1.0f, 0.9f, 0.6f});
    bodies.back().isStar = true;

    struct PlanetDef { const char* name; double distAU; double speed; double angleDeg; double mass; float radius; glm::vec3 color; };
    std::vector<PlanetDef> planets = {
        { "Mercury", 0.387, 47.36e3,  45.0,    3.3011e23, 2.4397e6f, {0.8f, 0.7f, 0.6f} },
        { "Venus",   0.723, 35.02e3, 180.0,    4.8675e24, 6.0518e6f, {1.0f, 0.85f, 0.6f} },
        { "Earth",   1.000, 29.78e3,  10.0,    5.972e24,  6.371e6f,  {0.2f, 0.4f, 1.0f} },
        { "Mars",    1.524, 24.077e3, 270.0,   6.4171e23, 3.3895e6f, {1.0f, 0.4f, 0.2f} },
        { "Jupiter", 5.203, 13.07e3,  135.0,   1.898e27,  6.9911e7f, {1.0f, 0.85f, 0.6f} },
        { "Saturn",  9.537, 9.68e3,   240.0,   5.683e26,  5.8232e7f, {1.0f, 0.9f, 0.7f} },
        { "Uranus",  19.191, 6.80e3,  315.0,   8.681e25,  2.5362e7f, {0.6f, 0.8f, 1.0f} },
        { "Neptune", 30.07, 5.43e3,   60.0,    1.024e26,  2.4622e7f, {0.4f, 0.6f, 1.0f} },
        { "Pluto",   39.48, 4.74e3,   200.0,   1.309e22,  1.1883e6f, {0.8f, 0.8f, 0.9f} }
    };

    glm::dvec3 earthPos(0.0), earthVel(0.0);

    for (const auto &p : planets) {
        double dist_m = p.distAU * AU;
        double px, pz;
        polar_to_cart(dist_m, p.angleDeg, px, pz);

        double vx, vz;
        velocity_from_speed_angle(p.speed, p.angleDeg, vx, vz);

        if (std::string(p.name) == "Earth") {
            earthPos = glm::dvec3(px, 0.0, pz);
            earthVel = glm::dvec3(vx, 0.0, vz);
            push_body(p.name, p.mass, px, 0.0, pz, vx, 0.0, vz, p.radius, p.color);
        } else {
            push_body(p.name, p.mass, px, 0.0, pz, vx, 0.0, vz, p.radius, p.color);
        }
    }

    // Moon (relative to Earth)
    {
        double dist = 3.844e8; // Moon's distance from Earth
        double speed = 1.022e3; // Moon's speed relative to Earth
        double angleDeg = 90.0;
        double mx, mz;
        polar_to_cart(dist, angleDeg, mx, mz);

        double mvx, mvz;
        velocity_from_speed_angle(speed, angleDeg, mvx, mvz);

        // absolute position = earthPos + moon offset, absolute velocity = earthVel + moon velocity
        glm::dvec3 moonPos = earthPos + glm::dvec3(mx, 0.0, mz);
        glm::dvec3 moonVel = earthVel + glm::dvec3(mvx, 0.0, mvz);
        bodies.push_back({
            "Moon", 7.3477e22,
            moonPos, moonVel,
            1.737e6f, {0.7f, 0.7f, 0.7f}
        });
    }
}

void computeAccels(std::vector<glm::dvec3>& acc) {
    size_t n=bodies.size();
    acc.assign(n, glm::dvec3(0.0));

    // This is the correction factor: M / D^3
    const double G_SCALING_FACTOR = MASS_SCALE / (DISTANCE_SCALE * DISTANCE_SCALE * DISTANCE_SCALE);

    for(size_t i=0;i<n;i++){
        for(size_t j=0;j<n;j++){
            if(i==j) continue;
            glm::dvec3 r = bodies[j].pos - bodies[i].pos;
            double dist2 = glm::dot(r,r) + 1e-6;
            double dist = sqrt(dist2);
            double f = (G_CONST * bodies[j].mass) / dist2;

            f *= G_SCALING_FACTOR;

            acc[i] += (r/dist) * f;
        }
    }
}

void integrateVerlet(double dt) {
    size_t n=bodies.size();
    std::vector<glm::dvec3> a_old(n);
    computeAccels(a_old);
    for(size_t i=0;i<n;i++){
        bodies[i].pos += bodies[i].vel * dt + 0.5 * a_old[i] * dt * dt;
    }
    std::vector<glm::dvec3> a_new(n);
    computeAccels(a_new);
    for(size_t i=0;i<n;i++){
        bodies[i].vel += 0.5 * (a_old[i] + a_new[i]) * dt;
    }
}

// ---------- Global GL state ----------
GLFWwindow* gWindow = nullptr;
int gWidth = SCR_W_DEFAULT, gHeight = SCR_H_DEFAULT;
GLuint gProg = 0;
GLint locM, locV, locP, locColor, locLight;

GLuint gOrbitProg = 0;
GLint locOrbitM, locOrbitV, locOrbitP, locOrbitColor;

// camera control
double lastX=SCR_W_DEFAULT/2.0, lastY=SCR_H_DEFAULT/2.0;
bool leftDown=false;
bool touchMode=false;
double touchLastX=0, touchLastY=0; // separate tracking for touch input
float yaw=-90.0f, pitch=30.0f;
float distanceCam = 20.0f;
glm::vec3 camTarget = {0.0f,0.0f,0.0f};

// ---------- Input Handlers ----------
void handle_drag(double dx, double dy) {
    float sens = 0.2f;
    yaw += dx * sens; pitch -= dy * sens;
    if(pitch>89.0f) pitch=89.0f; if(pitch<-89.0f) pitch=-89.0f;
}

void handle_scroll(double yoff_scaled) {
    fov -= (float)yoff_scaled;
    if (fov < 5.0f)  fov = 5.0f;
    if (fov > 90.0f) fov = 90.0f;
}

void handle_button(int button, int action) {
    if(button==GLFW_MOUSE_BUTTON_LEFT){
        leftDown = (action==GLFW_PRESS);
    }
}

static void cursorPosCB(GLFWwindow* win, double xpos, double ypos){
    if(touchMode) return;
    if(!leftDown){ lastX=xpos; lastY=ypos; return; }
    double dx = xpos - lastX; double dy = ypos - lastY;
    lastX = xpos; lastY = ypos;
    handle_drag(dx, dy);
}

static void mouseBtnCB(GLFWwindow* w, int button, int action, int mods){
    if(touchMode) return;
    handle_button(button, action); // Call handler
    // Update lastX/Y on new click to prevent jump
    if(action == GLFW_PRESS && button == GLFW_MOUSE_BUTTON_LEFT) { 
        glfwGetCursorPos(w, &lastX, &lastY);
    }
}

static void scrollCB(GLFWwindow* w, double xoff, double yoff){
    handle_scroll(yoff * 1.5f); // Call handler with scaling
}

// ---------- compile shaders ----------
GLuint makeProgram(const char* vsSrc, const char* fsSrc) {
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(vs, 1, &vsSrc, nullptr);
    glShaderSource(fs, 1, &fsSrc, nullptr);
    glCompileShader(vs); checkCompile(vs);
    glCompileShader(fs); checkCompile(fs);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs); glAttachShader(prog, fs);
    glLinkProgram(prog); checkLink(prog);
    glDeleteShader(vs); glDeleteShader(fs);
    return prog;
}

// ---------- timing ----------
using Clock = std::chrono::high_resolution_clock;
auto tPrev = Clock::now();

// Helper: compute accelerations on a temporary body set (for future prediction)
void computeAccelsTemp(std::vector<glm::dvec3>& positions, std::vector<double>& masses,
                       std::vector<glm::dvec3>& acc) {
    size_t n = positions.size();
    acc.assign(n, glm::dvec3(0.0));
    const double G_SCALING_FACTOR = MASS_SCALE / (DISTANCE_SCALE * DISTANCE_SCALE * DISTANCE_SCALE);
    for(size_t i=0;i<n;i++){
        for(size_t j=0;j<n;j++){
            if(i==j) continue;
            glm::dvec3 r = positions[j] - positions[i];
            double dist2 = glm::dot(r,r) + 1e-6;
            double dist = sqrt(dist2);
            double f = (G_CONST * masses[j]) / dist2;
            f *= G_SCALING_FACTOR;
            acc[i] += (r/dist) * f;
        }
    }
}

void predictFutureTrails() {
    size_t n = bodies.size();
    // Clone current state
    std::vector<glm::dvec3> pos(n), vel(n);
    std::vector<double> mass(n);
    for (size_t i = 0; i < n; i++) {
        pos[i] = bodies[i].pos;
        vel[i] = bodies[i].vel;
        mass[i] = bodies[i].mass;
    }

    // Clear future trails
    for (auto &b : bodies) {
        b.futureTrail.clear();
    }

    // Add current position as first point
    for (size_t i = 0; i < n; i++) {
        bodies[i].futureTrail.push_back(glm::vec3((float)pos[i].x, (float)pos[i].y, (float)pos[i].z));
    }

    // Simulate forward
    double fdt = FUTURE_STEP_DT;
    for (int step = 0; step < FUTURE_TRAIL_POINTS; step++) {
        // Verlet integration on temp data
        std::vector<glm::dvec3> a_old;
        computeAccelsTemp(pos, mass, a_old);
        for (size_t i = 0; i < n; i++) {
            pos[i] += vel[i] * fdt + 0.5 * a_old[i] * fdt * fdt;
        }
        std::vector<glm::dvec3> a_new;
        computeAccelsTemp(pos, mass, a_new);
        for (size_t i = 0; i < n; i++) {
            vel[i] += 0.5 * (a_old[i] + a_new[i]) * fdt;
        }

        // Record
        for (size_t i = 0; i < n; i++) {
            bodies[i].futureTrail.push_back(glm::vec3((float)pos[i].x, (float)pos[i].y, (float)pos[i].z));
        }
    }

    for (auto &b : bodies) {
        b.futureDirty = true;
    }
}

void drawBodyTrail(Body &b) {
    glm::mat4 model(1.0f); // trail positions are already in world space

    // Draw past trail (fading from transparent to solid via per-vertex alpha)
    b.uploadTrail();
    glUseProgram(gOrbitProg);
    glUniformMatrix4fv(locOrbitM, 1, GL_FALSE, glm::value_ptr(model));
    glUniform3f(locOrbitColor, b.color.r * 0.6f, b.color.g * 0.6f, b.color.b * 0.6f);
    b.drawTrail();

    // Draw future trail (semi-transparent via per-vertex alpha)
    b.uploadFutureTrail();
    glUseProgram(gOrbitProg);
    glUniformMatrix4fv(locOrbitM, 1, GL_FALSE, glm::value_ptr(model));
    glUniform3f(locOrbitColor, b.color.r * 0.35f, b.color.g * 0.35f, b.color.b * 0.35f);
    b.drawFutureTrail();
}

// main loop function (called by emscripten)
void main_loop() {
    // time
    auto now = Clock::now();
    std::chrono::duration<double> elapsed = now - tPrev;
    tPrev = now;

    const double MAX_ELAPSED_SECONDS = 1.0 / 30.0; // Cap at 30fps
    double elapsed_seconds = std::min(elapsed.count(), MAX_ELAPSED_SECONDS);
    if(elapsed_seconds <= 0) elapsed_seconds = 1.0 / 60.0;

    double dt = elapsed.count() * TIME_SCALE * timeScaleMultiplier;
    if(dt <= 0) dt = 1.0/60.0 * TIME_SCALE * timeScaleMultiplier;

    // physics with substeps
    int sub = 3;
    double sdt = dt / sub;
    for(int i=0;i<sub;i++) integrateVerlet(sdt);

    // record orbit trails periodically
    trailFrameCounter++;
    if (trailFrameCounter >= ORBIT_TRAIL_RECORD_INTERVAL) {
        trailFrameCounter = 0;
        for (auto &b : bodies) {
            b.recordTrailPoint();
        }
    }

    // predict future trails periodically
    futureFrameCounter++;
    if (futureFrameCounter >= FUTURE_PREDICT_INTERVAL) {
        futureFrameCounter = 0;
        predictFutureTrails();
    }

    // --- Determine framebuffer size cross-platform ---
#ifdef __EMSCRIPTEN__
    int width = gWidth;
    int height = gHeight;
    if (width <= 0 || height <= 0) { width = SCR_W_DEFAULT; height = SCR_H_DEFAULT; }
#else
    int width, height;
    glfwGetFramebufferSize(gWindow, &width, &height);
    gWidth = width; gHeight = height;
#endif

    // render
    glViewport(0, 0, width, height);
    glClearColor(0.01f,0.01f,0.02f,1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Update time for sun shader
    gTime += (float)elapsed_seconds;

    // Enable blending for transparent orbit trails
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // camera
    float yawRad = glm::radians(yaw), pitchRad = glm::radians(pitch);
    glm::vec3 camPos;
    camPos.x = camTarget.x + distanceCam * cos(pitchRad) * cos(yawRad);
    camPos.y = camTarget.y + distanceCam * sin(pitchRad);
    camPos.z = camTarget.z + distanceCam * cos(pitchRad) * sin(yawRad);

    glm::mat4 view = glm::lookAt(camPos, camTarget, glm::vec3(0,1,0));
    glm::mat4 proj = glm::perspective(glm::radians(fov), (float)width / (float)height, 0.1f, 1000.0f);

    // Draw starfield background first
    drawStars(view, proj);

    // upload common uniforms
    glUseProgram(gOrbitProg);
    glUniformMatrix4fv(locOrbitV, 1, GL_FALSE, glm::value_ptr(view));
    glUniformMatrix4fv(locOrbitP, 1, GL_FALSE, glm::value_ptr(proj));

    glUseProgram(gProg);
    glUniformMatrix4fv(locV, 1, GL_FALSE, glm::value_ptr(view));
    glUniformMatrix4fv(locP, 1, GL_FALSE, glm::value_ptr(proj));
    glUniform3f(locLight, 0.0f, 0.0f, 0.0f);  // Light from the Sun at origin

    for(auto &b : bodies){
        drawBodyTrail(b);

        glDisable(GL_BLEND); // solid bodies

        if (b.isStar) {
            // Draw sun with special shader
            glUseProgram(gSunProg);
            glm::mat4 model(1.0f);
            model = glm::translate(model, glm::vec3((float)b.pos.x, (float)b.pos.y, (float)b.pos.z));
            float visualScale = std::max(0.05f, (float)(b.radius * 1.0f));
            model = glm::scale(model, glm::vec3(visualScale));
            glUniformMatrix4fv(locSunM, 1, GL_FALSE, glm::value_ptr(model));
            glUniformMatrix4fv(locSunV, 1, GL_FALSE, glm::value_ptr(view));
            glUniformMatrix4fv(locSunP, 1, GL_FALSE, glm::value_ptr(proj));
            glUniform1f(locSunTime, gTime);
            sunMesh.draw();

            // Draw glow billboard
            glm::vec3 sp((float)b.pos.x, (float)b.pos.y, (float)b.pos.z);
            drawSunGlow(view, proj, sp, visualScale * 5.0f);
        } else {
            glUseProgram(gProg);
            glm::mat4 model(1.0f);
            model = glm::translate(model, glm::vec3((float)b.pos.x, (float)b.pos.y, (float)b.pos.z));
            float visualScale = std::max(0.05f, (float)(b.radius * 1.0f));
            model = glm::scale(model, glm::vec3(visualScale));
            glUniformMatrix4fv(locM, 1, GL_FALSE, glm::value_ptr(model));
            glUniform3fv(locColor, 1, glm::value_ptr(b.color));
            sphereMesh.draw();
        }

        glEnable(GL_BLEND); // re-enable for next body's trail
    }

    glDisable(GL_BLEND);
    glfwSwapBuffers(gWindow);
    glfwPollEvents();
}

// ---------- resize callback ----------
static void framebuffer_cb(GLFWwindow* win, int w, int h){
    gWidth = w;
    gHeight = h;
    glViewport(0, 0, gWidth, gHeight);
}

// Exposed to JS
extern "C" {
    EMSCRIPTEN_KEEPALIVE
    void emscripten_set_canvas_size(int w, int h) {
        gWidth = w;
        gHeight = h;

        if (gWindow) {
            glViewport(0, 0, gWidth, gHeight);
        }
    }

    EMSCRIPTEN_KEEPALIVE
    void emscripten_touch_start(double x, double y) {
        touchMode = true;
        touchLastX = x;
        touchLastY = y;
    }

    EMSCRIPTEN_KEEPALIVE
    void emscripten_touch_end() {
        touchMode = false;
    }

    EMSCRIPTEN_KEEPALIVE
    void emscripten_touch_move(double x, double y) {
        double dx = x - touchLastX;
        double dy = y - touchLastY;
        touchLastX = x;
        touchLastY = y;
        handle_drag(dx, dy);
    }

    EMSCRIPTEN_KEEPALIVE
    void emscripten_touch_zoom(double delta_zoom) {
        handle_scroll(delta_zoom); 
    }

    // Add a body to the simulation
    // All values in real-world units (meters, kg, m/s) - will be scaled internally
    EMSCRIPTEN_KEEPALIVE
    void add_body(double mass, double px, double py, double pz,
                  double vx, double vy, double vz,
                  float radius, float cr, float cg, float cb, int is_star) {
        Body b;
        b.name = is_star ? "Star" : "Planet";
        b.mass = mass / MASS_SCALE;
        b.pos = glm::dvec3(px, py, pz) / DISTANCE_SCALE;
        b.vel = glm::dvec3(vx, vy, vz) / DISTANCE_SCALE;
        b.radius = (float)(radius / DISTANCE_SCALE);
        b.color = glm::vec3(cr, cg, cb);
        b.isStar = (is_star != 0);
        bodies.push_back(b);

        // Trigger future trail re-prediction
        futureFrameCounter = FUTURE_PREDICT_INTERVAL;
    }

    // Get the number of bodies currently in the simulation
    EMSCRIPTEN_KEEPALIVE
    int get_body_count() {
        return (int)bodies.size();
    }

    // --- Time scale control ---
    EMSCRIPTEN_KEEPALIVE
    void set_time_scale(double multiplier) {
        timeScaleMultiplier = multiplier;
    }

    EMSCRIPTEN_KEEPALIVE
    double get_time_scale() {
        return timeScaleMultiplier;
    }

    // --- Screen-to-world raycast (y=0 plane) ---
    // Stores result in globals; call get_world_x/z after a successful hit.
    static double s2w_x = 0.0, s2w_z = 0.0;

    EMSCRIPTEN_KEEPALIVE
    int screen_to_world(double screen_x, double screen_y) {
        // Reconstruct camera
        float yawRad = glm::radians(yaw), pitchRad = glm::radians(pitch);
        glm::vec3 camPos;
        camPos.x = camTarget.x + distanceCam * cos(pitchRad) * cos(yawRad);
        camPos.y = camTarget.y + distanceCam * sin(pitchRad);
        camPos.z = camTarget.z + distanceCam * cos(pitchRad) * sin(yawRad);

        glm::mat4 view = glm::lookAt(camPos, camTarget, glm::vec3(0,1,0));
        glm::mat4 proj = glm::perspective(glm::radians(fov), (float)gWidth / (float)gHeight, 0.1f, 1000.0f);
        glm::mat4 invVP = glm::inverse(proj * view);

        // Normalize screen coords to NDC (-1..1)
        float ndcX = (float)(2.0 * screen_x / gWidth - 1.0);
        float ndcY = (float)(1.0 - 2.0 * screen_y / gHeight); // flip Y

        // Near and far points in world space
        glm::vec4 nearH = invVP * glm::vec4(ndcX, ndcY, -1.0f, 1.0f);
        glm::vec4 farH  = invVP * glm::vec4(ndcX, ndcY,  1.0f, 1.0f);
        glm::vec3 nearW = glm::vec3(nearH) / nearH.w;
        glm::vec3 farW  = glm::vec3(farH)  / farH.w;

        // Ray direction
        glm::vec3 dir = farW - nearW;

        // Intersect with y=0 plane
        if (fabs(dir.y) < 1e-8) return 0; // parallel
        float t = -nearW.y / dir.y;
        glm::vec3 hit = nearW + t * dir;

        s2w_x = (double)hit.x;
        s2w_z = (double)hit.z;
        return 1;
    }

    EMSCRIPTEN_KEEPALIVE
    double get_world_x() { return s2w_x; }

    EMSCRIPTEN_KEEPALIVE
    double get_world_z() { return s2w_z; }
}

// ---------- init ----------
int main() {
    if(!glfwInit()){
        std::cerr << "GLFW init failed\n"; return 1;
    }
    // Ask WebGL 2 / OpenGL ES 3
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
    glfwWindowHint(GLFW_RESIZABLE, GL_TRUE);

    // create a fullscreen sized canvas (the canvas size will be controlled by HTML/CSS).
    gWindow = glfwCreateWindow(SCR_W_DEFAULT, SCR_H_DEFAULT, "SolarSim WASM", nullptr, nullptr);
    if(!gWindow) { std::cerr << "Window failed\n"; return 1; }
    glfwMakeContextCurrent(gWindow);

    // enable vsync (cap fps)
    glfwSwapInterval(1);

    // GL state
    glEnable(GL_DEPTH_TEST);

    // Mesh + program
    sphereMesh.build(SPHERE_LAT, SPHERE_LONG);
    sunMesh.build(SUN_SPHERE_LAT, SUN_SPHERE_LONG);

    gProg = makeProgram(vertexShaderSrc, fragmentShaderSrc);
    locM = glGetUniformLocation(gProg, "uModel");
    locV = glGetUniformLocation(gProg, "uView");
    locP = glGetUniformLocation(gProg, "uProj");
    locColor = glGetUniformLocation(gProg, "uColor");
    locLight = glGetUniformLocation(gProg, "uLightPos");

    gOrbitProg = makeProgram(orbitVertexShaderSrc, orbitFragmentShaderSrc);
    locOrbitM = glGetUniformLocation(gOrbitProg, "uModel");
    locOrbitV = glGetUniformLocation(gOrbitProg, "uView");
    locOrbitP = glGetUniformLocation(gOrbitProg, "uProj");
    locOrbitColor = glGetUniformLocation(gOrbitProg, "uColor");

    // Star shader
    gStarProg = makeProgram(starVertexShaderSrc, starFragmentShaderSrc);
    locStarView = glGetUniformLocation(gStarProg, "uView");
    locStarProj = glGetUniformLocation(gStarProg, "uProj");

    // Sun shader
    gSunProg = makeProgram(sunVertexShaderSrc, sunFragmentShaderSrc);
    locSunM = glGetUniformLocation(gSunProg, "uModel");
    locSunV = glGetUniformLocation(gSunProg, "uView");
    locSunP = glGetUniformLocation(gSunProg, "uProj");
    locSunTime = glGetUniformLocation(gSunProg, "uTime");

    // Glow shader
    gGlowProg = makeProgram(glowVertexShaderSrc, glowFragmentShaderSrc);
    locGlowView = glGetUniformLocation(gGlowProg, "uView");
    locGlowProj = glGetUniformLocation(gGlowProg, "uProj");
    locGlowCenter = glGetUniformLocation(gGlowProg, "uCenter");
    locGlowSize = glGetUniformLocation(gGlowProg, "uSize");
    locGlowColor = glGetUniformLocation(gGlowProg, "uColor");

    // Build starfield and glow quad
    buildStarfield();
    buildGlowQuad();

    // callbacks
    glfwSetCursorPosCallback(gWindow, cursorPosCB);
    glfwSetMouseButtonCallback(gWindow, mouseBtnCB);
    glfwSetScrollCallback(gWindow, scrollCB);
    glfwSetFramebufferSizeCallback(gWindow, framebuffer_cb);

    // Setup physics
    setupSolarSystem();
    for(auto &b : bodies){
        b.pos /= DISTANCE_SCALE;
        b.mass /= MASS_SCALE;
        b.vel /= DISTANCE_SCALE;
        b.radius = (float)(b.radius / DISTANCE_SCALE);
    }

    tPrev = Clock::now();

    // Use emscripten main loop to integrate with browser scheduling
    emscripten_set_main_loop(main_loop, 0, true);

    return 0; //emscripten_set_main_loop does not return
}
