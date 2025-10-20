// main_web.cpp
// Emscripten / WebAssembly version of SolarSim using GLFW + WebGL2 (GLSL ES 3.00)
// Compile with emcc (instructions below)

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

const int SPHERE_LAT = 16;
const int SPHERE_LONG = 16;

float fov = 80.0f;

// ---------- Embedded shaders (GLSL ES 3.00) ----------
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

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProj;

void main() {
    gl_Position = uProj * uView * uModel * vec4(aPos, 1.0);
}
)glsl";

const char *orbitFragmentShaderSrc = R"glsl(#version 300 es
precision highp float;
out vec4 FragColor;

uniform vec3 uColor;

void main() {
    FragColor = vec4(uColor, 1.0);
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
} sphereMesh;

// ---------- Physics bodies ----------
struct Body {
    std::string name;
    double mass;
    glm::dvec3 pos;
    glm::dvec3 vel;
    float radius;
    glm::vec3 color;
};
std::vector<Body> bodies;

void setupSolarSystem() {
    bodies.clear();

    // Sun
    bodies.push_back({
        "Sun", 1.9885e30,
        {0, 0, 0}, {0, 0, 0},
        6.9634e8f, {1.0f, 0.9f, 0.6f}
    });

    // Mercury
    bodies.push_back({
        "Mercury", 3.3011e23,
        {0.387 * 1.496e11, 0, 0},
        {0, 47.36e3, 0},
        2.4397e6f, {0.8f, 0.7f, 0.6f}
    });

    // Venus
    bodies.push_back({
        "Venus", 4.8675e24,
        {0.723 * 1.496e11, 0, 0},
        {0, 35.02e3, 0},
        6.0518e6f, {1.0f, 0.85f, 0.6f}
    });

    // Earth
    bodies.push_back({
        "Earth", 5.972e24,
        {1.0 * 1.496e11, 0, 0},
        {0, 29.78e3, 0},
        6.371e6f, {0.2f, 0.4f, 1.0f}
    });

    // Moon
    bodies.push_back({
        "Moon", 7.3477e22,
        {1.0 * 1.496e11 + 3.844e8, 0, 0},
        {0, 29.78e3 + 1.022e3, 0},
        1.737e6f, {0.7f, 0.7f, 0.7f}
    });

    // Mars
    bodies.push_back({
        "Mars", 6.4171e23,
        {1.524 * 1.496e11, 0, 0},
        {0, 24.077e3, 0},
        3.3895e6f, {1.0f, 0.4f, 0.2f}
    });

    // Jupiter
    bodies.push_back({
        "Jupiter", 1.898e27,
        {5.203 * 1.496e11, 0, 0},
        {0, 13.07e3, 0},
        6.9911e7f, {1.0f, 0.85f, 0.6f}
    });

    // Saturn
    bodies.push_back({
        "Saturn", 5.683e26,
        {9.537 * 1.496e11, 0, 0},
        {0, 9.68e3, 0},
        5.8232e7f, {1.0f, 0.9f, 0.7f}
    });

    // Uranus
    bodies.push_back({
        "Uranus", 8.681e25,
        {19.191 * 1.496e11, 0, 0},
        {0, 6.80e3, 0},
        2.5362e7f, {0.6f, 0.8f, 1.0f}
    });

    // Neptune
    bodies.push_back({
        "Neptune", 1.024e26,
        {30.07 * 1.496e11, 0, 0},
        {0, 5.43e3, 0},
        2.4622e7f, {0.4f, 0.6f, 1.0f}
    });

    // Pluto (why not!)
    bodies.push_back({
        "Pluto", 1.309e22,
        {39.48 * 1.496e11, 0, 0},
        {0, 4.74e3, 0},
        1.1883e6f, {0.8f, 0.8f, 0.9f}
    });
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
float yaw=-90.0f, pitch=0.0f;
float distanceCam = 20.0f;
glm::vec3 camTarget = {0.0f,0.0f,0.0f};

static void cursorPosCB(GLFWwindow* win, double xpos, double ypos){
    if(!leftDown){ lastX=xpos; lastY=ypos; return; }
    double dx = xpos - lastX; double dy = ypos - lastY;
    lastX = xpos; lastY = ypos;
    float sens = 0.2f;
    yaw += dx * sens; pitch -= dy * sens;
    if(pitch>89.0f) pitch=89.0f; if(pitch<-89.0f) pitch=-89.0f;
}
static void mouseBtnCB(GLFWwindow* w, int button, int action, int mods){
    if(button==GLFW_MOUSE_BUTTON_LEFT){
        leftDown = (action==GLFW_PRESS);
    }
}

static void scrollCB(GLFWwindow* w, double xoff, double yoff){
    fov -= (float)yoff * 1.5f;
    if (fov < 5.0f)  fov = 5.0f;
    if (fov > 90.0f) fov = 90.0f;
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

GLuint orbitVAO = 0, orbitVBO = 0;
const int ORBIT_SEGMENTS = 64;

void buildOrbitMesh() {
    std::vector<float> verts;
    verts.reserve(ORBIT_SEGMENTS * 3);
    for (int i = 0; i < ORBIT_SEGMENTS; i++) {
        float theta = (float)i / ORBIT_SEGMENTS * glm::two_pi<float>();
        verts.push_back(cos(theta));
        verts.push_back(0.0f);
        verts.push_back(sin(theta));
    }
    glGenVertexArrays(1, &orbitVAO);
    glGenBuffers(1, &orbitVBO);
    glBindVertexArray(orbitVAO);
    glBindBuffer(GL_ARRAY_BUFFER, orbitVBO);
    glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), 0);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);
}

void drawOrbit(float radius) {
    glUseProgram(gOrbitProg); // Use the simple orbit shader
    glm::mat4 model = glm::scale(glm::mat4(1.0f), glm::vec3(radius));
    glUniformMatrix4fv(locOrbitM, 1, GL_FALSE, glm::value_ptr(model));
    glUniform3f(locOrbitColor, 0.3f, 0.3f, 0.3f);
    glBindVertexArray(orbitVAO);
    glDrawArrays(GL_LINE_LOOP, 0, ORBIT_SEGMENTS);
    glBindVertexArray(0);
}

// main loop function (called by emscripten)
void main_loop() {
    // time
    auto now = Clock::now();
    std::chrono::duration<double> elapsed = now - tPrev;
    tPrev = now;
    double dt = elapsed.count() * TIME_SCALE;
    if(dt <= 0) dt = 1.0/60.0 * TIME_SCALE;

    // physics with substeps
    int sub = 3;
    double sdt = dt / sub;
    for(int i=0;i<sub;i++) integrateVerlet(sdt);

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
    glClearColor(0.02f,0.02f,0.04f,1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // camera
    float yawRad = glm::radians(yaw), pitchRad = glm::radians(pitch);
    glm::vec3 camPos;
    camPos.x = camTarget.x + distanceCam * cos(pitchRad) * cos(yawRad);
    camPos.y = camTarget.y + distanceCam * sin(pitchRad);
    camPos.z = camTarget.z + distanceCam * cos(pitchRad) * sin(yawRad);

    glm::mat4 view = glm::lookAt(camPos, camTarget, glm::vec3(0,1,0));
    glm::mat4 proj = glm::perspective(glm::radians(fov), (float)width / (float)height, 0.1f, 1000.0f);

    // upload common uniforms
    glUseProgram(gOrbitProg);
    glUniformMatrix4fv(locOrbitV, 1, GL_FALSE, glm::value_ptr(view));
    glUniformMatrix4fv(locOrbitP, 1, GL_FALSE, glm::value_ptr(proj));

    glUseProgram(gProg);
    glUniformMatrix4fv(locV, 1, GL_FALSE, glm::value_ptr(view));
    glUniformMatrix4fv(locP, 1, GL_FALSE, glm::value_ptr(proj));
    glUniform3f(locLight, camPos.x, camPos.y, camPos.z);

    for(const auto &b : bodies){
        if (b.name != "Sun") {
            float r = glm::length(b.pos);
            drawOrbit(r); // draw circle with current orbital radius
        }

        glUseProgram(gProg);

        glm::mat4 model(1.0f);
        model = glm::translate(model, glm::vec3((float)b.pos.x, (float)b.pos.y, (float)b.pos.z));
        float visualScale = std::max(0.05f, (float)(b.radius * 1.0f));
        model = glm::scale(model, glm::vec3(visualScale));
        glUniformMatrix4fv(locM, 1, GL_FALSE, glm::value_ptr(model));
        glUniform3fv(locColor, 1, glm::value_ptr(b.color));
        sphereMesh.draw();
    }

    glfwSwapBuffers(gWindow);
    glfwPollEvents();
}

// ---------- resize callback ----------
static void framebuffer_cb(GLFWwindow* win, int w, int h){
    // Update stored sizes and viewport immediately
    gWidth = w;
    gHeight = h;
    glViewport(0, 0, gWidth, gHeight);
}

// Expose a function to JS so the index.html can set canvas size in the Module
extern "C" {
    EMSCRIPTEN_KEEPALIVE
    void emscripten_set_canvas_size(int w, int h) {
        gWidth = w;
        gHeight = h;
        // Also update immediate GL viewport if context exists
        if (gWindow) {
            glViewport(0, 0, gWidth, gHeight);
        }
    }
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
    buildOrbitMesh();

    // Use emscripten main loop to integrate with browser scheduling
    emscripten_set_main_loop(main_loop, 0, true);

    return 0; //emscripten_set_main_loop does not return
}
