/**
 * @file main.cpp
 * @brief Application entry point for AgentDesktop.
 *
 * Two execution modes
 * -------------------
 *   GUI mode (default)
 *     AgentDesktop.exe
 *     Initialises GLFW, creates an OpenGL 3.3 Core context, bootstraps
 *     Dear ImGui, and runs the render loop.
 *
 *   MCP mode
 *     AgentDesktop.exe --mcp
 *     Skips all GUI initialisation.  Initialises the Platform directly,
 *     then runs a blocking stdio JSON-RPC 2.0 loop that exposes all
 *     virtual-desktop operations as MCP tools.
 *
 * GLFW / OpenGL version requirements
 * ------------------------------------
 *   Windows : OpenGL 3.3 via the display driver (all modern GPUs).
 *   macOS   : OpenGL 3.3 Core, requires GLFW_OPENGL_FORWARD_COMPAT on 10.9+.
 *   Linux   : OpenGL 3.3 via Mesa (software fallback acceptable for CI).
 *
 * @copyright AgentDesktop Project
 */

#include "app/App.h"
#include "mcp/McpServer.h"

#include <GLFW/glfw3.h>
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <cstdio>
#include <cstring>
#include <memory>

// ---------------------------------------------------------------------------
// GLFW error callback
// ---------------------------------------------------------------------------

static void on_glfw_error(int code, const char* description) {
    fprintf(stderr, "[GLFW] Error %d: %s\n", code, description);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    // ── Mode selection ───────────────────────────────────────────────────────
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--mcp") == 0) {
            return run_mcp_mode();
        }
    }

    // ── GLFW initialisation ──────────────────────────────────────────────────
    glfwSetErrorCallback(on_glfw_error);
    if (!glfwInit()) {
        fprintf(stderr, "Failed to initialise GLFW\n");
        return 1;
    }

    // Request OpenGL 3.3 Core — supported on all target platforms.
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif
    glfwWindowHint(GLFW_SAMPLES, 0);

    GLFWwindow* window = glfwCreateWindow(
        1440, 900, "AgentDesktop", nullptr, nullptr);
    if (!window) {
        fprintf(stderr, "Failed to create GLFW window\n");
        glfwTerminate();
        return 1;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);  // VSync on

    // ── Dear ImGui initialisation ────────────────────────────────────────────
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = "agentdesktop.ini"; // persist window layout
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui_ImplGlfw_InitForOpenGL(window, /*install_callbacks=*/false);
    ImGui_ImplOpenGL3_Init("#version 330 core");

    // ── Application instance ─────────────────────────────────────────────────
    auto app = std::make_unique<agentdesktop::app::AgentDesktopApp>(window);

    // ── Input callbacks (forwarded to app) ───────────────────────────────────
    glfwSetWindowUserPointer(window, app.get());

    // Forward all input to ImGui first, then to the app if ImGui doesn't
    // want it.  This is required because install_callbacks=false above.
    glfwSetKeyCallback(window, [](GLFWwindow* w, int key, int scan,
                                   int action, int mods) {
        ImGui_ImplGlfw_KeyCallback(w, key, scan, action, mods);
        if (ImGui::GetIO().WantCaptureKeyboard) return;
        auto* a = static_cast<agentdesktop::app::AgentDesktopApp*>(
                      glfwGetWindowUserPointer(w));
        a->on_key(key, action, mods);
    });

    glfwSetCharCallback(window, [](GLFWwindow* w, unsigned int cp) {
        ImGui_ImplGlfw_CharCallback(w, cp);
        if (ImGui::GetIO().WantCaptureKeyboard) return;
        auto* a = static_cast<agentdesktop::app::AgentDesktopApp*>(
                      glfwGetWindowUserPointer(w));
        a->on_char(cp);
    });

    glfwSetScrollCallback(window, [](GLFWwindow* w, double xoff, double yoff) {
        ImGui_ImplGlfw_ScrollCallback(w, xoff, yoff);
    });

    glfwSetMouseButtonCallback(window, [](GLFWwindow* w, int btn, int action, int mods) {
        ImGui_ImplGlfw_MouseButtonCallback(w, btn, action, mods);
    });

    glfwSetCursorPosCallback(window, [](GLFWwindow* w, double x, double y) {
        ImGui_ImplGlfw_CursorPosCallback(w, x, y);
    });

    // ── Render loop ──────────────────────────────────────────────────────────
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        app->render();

        ImGui::Render();

        int fb_w = 0, fb_h = 0;
        glfwGetFramebufferSize(window, &fb_w, &fb_h);
        glViewport(0, 0, fb_w, fb_h);
        glClearColor(0.05f, 0.055f, 0.078f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    // ── Teardown ─────────────────────────────────────────────────────────────
    app.reset(); // disconnect virtual desktop before destroying GL context

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
