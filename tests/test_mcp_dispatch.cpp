/**
 * @file test_mcp_dispatch.cpp
 * @brief Unit tests for MCP tool dispatch and result formatting.
 *
 * Uses a MockPlatform that returns controlled results so tests run
 * without any real virtual desktop, GLFW, or OpenGL.
 *
 * @copyright AgentDesktop Project
 */

#include <gtest/gtest.h>
#include "mcp/McpTools.h"
#include "mcp/ImageEncoder.h"

#include <cstring>
#include <vector>

using namespace agentdesktop::mcp;
using json = nlohmann::json;

// ---------------------------------------------------------------------------
// MockPlatform — returns deterministic results for every method
// ---------------------------------------------------------------------------

class MockPlatform : public agentdesktop::Platform {
public:
    // Configuration
    bool      launch_ok   = true;
    int       launch_pid  = 42;
    bool      action_ok   = true;
    bool      screenshot_ok = true;
    int       width  = 1920;
    int       height = 1080;

    // Call counters
    int click_calls        = 0;
    int double_click_calls = 0;
    int right_click_calls  = 0;
    int scroll_calls       = 0;
    int type_calls         = 0;
    int key_calls          = 0;
    int maximize_calls     = 0;
    int screenshot_calls   = 0;

    // Last args
    int last_click_x = 0, last_click_y = 0;
    int last_scroll_delta = 0;
    std::string last_text;
    std::string last_key;
    int last_pid = 0;

    bool init(std::string&) override { return true; }
    agentdesktop::Frame capture() override { return {}; }

    agentdesktop::LaunchResult launch_app(const std::string&, const std::string&) override {
        return {launch_ok, launch_pid, launch_ok ? "" : "mock launch error"};
    }

    agentdesktop::ActionResult maximize_window(int pid) override {
        ++maximize_calls; last_pid = pid;
        return {action_ok, action_ok ? "" : "mock maximize error"};
    }

    agentdesktop::ActionResult click(int x, int y) override {
        ++click_calls; last_click_x = x; last_click_y = y;
        return {action_ok, action_ok ? "" : "mock click error"};
    }

    agentdesktop::ActionResult double_click(int x, int y) override {
        ++double_click_calls; last_click_x = x; last_click_y = y;
        return {action_ok};
    }

    agentdesktop::ActionResult right_click(int x, int y) override {
        ++right_click_calls; last_click_x = x; last_click_y = y;
        return {action_ok};
    }

    agentdesktop::ActionResult scroll(int x, int y, int delta) override {
        ++scroll_calls; last_click_x = x; last_click_y = y;
        last_scroll_delta = delta;
        return {action_ok};
    }

    agentdesktop::ActionResult type_text(const std::string& text) override {
        ++type_calls; last_text = text;
        return {action_ok};
    }

    agentdesktop::ActionResult key_press(const std::string& key) override {
        ++key_calls; last_key = key;
        return {action_ok};
    }

    agentdesktop::ScreenshotResult screenshot(int, int, int, int) override {
        ++screenshot_calls;
        agentdesktop::ScreenshotResult r;
        r.ok     = screenshot_ok;
        r.error  = screenshot_ok ? "" : "mock screenshot error";
        r.width  = width;
        r.height = height;
        r.pixels.resize(static_cast<size_t>(width) * height * 4, 0x7F);
        return r;
    }

    int phys_width()  const override { return width; }
    int phys_height() const override { return height; }
};

// ---------------------------------------------------------------------------
// Helper: extract text from a tool result
// ---------------------------------------------------------------------------

static std::string get_text(const json& result) {
    return result["content"][0]["text"].get<std::string>();
}

static bool is_error(const json& result) {
    if (!result.contains("content") || result["content"].empty()) return false;
    const auto& c0 = result["content"][0];
    return c0.contains("isError") && c0["isError"].get<bool>();
}

// ---------------------------------------------------------------------------
// make_text_result
// ---------------------------------------------------------------------------

TEST(McpResults, TextResultShape) {
    const json r = make_text_result("hello", false);
    ASSERT_TRUE(r.contains("content"));
    ASSERT_EQ(r["content"].size(), 1u);
    EXPECT_EQ(r["content"][0]["type"].get<std::string>(), "text");
    EXPECT_EQ(r["content"][0]["text"].get<std::string>(), "hello");
    EXPECT_FALSE(r["content"][0]["isError"].get<bool>());
}

TEST(McpResults, ErrorResultShape) {
    const json r = make_text_result("bad", true);
    EXPECT_TRUE(r["content"][0]["isError"].get<bool>());
}

TEST(McpResults, ImageResultShape) {
    const json r = make_image_result("abc123");
    ASSERT_EQ(r["content"][0]["type"].get<std::string>(), "image");
    EXPECT_EQ(r["content"][0]["data"].get<std::string>(), "abc123");
    EXPECT_EQ(r["content"][0]["mimeType"].get<std::string>(), "image/png");
}

// ---------------------------------------------------------------------------
// get_desktop_info
// ---------------------------------------------------------------------------

TEST(ToolGetDesktopInfo, NullPlatformReturnsNotActive) {
    const json r = tool_get_desktop_info({}, nullptr);
    EXPECT_FALSE(is_error(r));
    EXPECT_NE(get_text(r).find("active=false"), std::string::npos);
}

TEST(ToolGetDesktopInfo, ReturnsWidthAndHeight) {
    MockPlatform plat;
    const json r = tool_get_desktop_info({}, &plat);
    EXPECT_FALSE(is_error(r));
    EXPECT_NE(get_text(r).find("1920"), std::string::npos);
    EXPECT_NE(get_text(r).find("1080"), std::string::npos);
    EXPECT_NE(get_text(r).find("active=true"), std::string::npos);
}

// ---------------------------------------------------------------------------
// screenshot
// ---------------------------------------------------------------------------

TEST(ToolScreenshot, NullPlatformIsError) {
    const json r = tool_screenshot({}, nullptr);
    EXPECT_TRUE(is_error(r));
}

TEST(ToolScreenshot, SuccessReturnsImageContent) {
    MockPlatform plat;
    plat.width = 640; plat.height = 480;
    plat.screenshot_ok = true;
    const json r = tool_screenshot({}, &plat);
    ASSERT_TRUE(r.contains("content"));
    ASSERT_FALSE(r["content"].empty());
    const auto& c0 = r["content"][0];
    EXPECT_EQ(c0["type"].get<std::string>(), "image");
    EXPECT_EQ(c0["mimeType"].get<std::string>(), "image/png");
    EXPECT_FALSE(c0["data"].get<std::string>().empty());
    EXPECT_EQ(plat.screenshot_calls, 1);
}

TEST(ToolScreenshot, PlatformFailureIsError) {
    MockPlatform plat;
    plat.screenshot_ok = false;
    const json r = tool_screenshot({}, &plat);
    ASSERT_TRUE(r.contains("content"));
    const auto& c0 = r["content"][0];
    EXPECT_EQ(c0["type"].get<std::string>(), "text");
    EXPECT_TRUE(c0.value("isError", false));
    EXPECT_NE(c0["text"].get<std::string>().find("failed"), std::string::npos);
}

TEST(ToolScreenshot, RegionArgsPassedThrough) {
    MockPlatform plat;
    plat.width = 1920; plat.height = 1080;
    const json args = {{"x", 10}, {"y", 20}, {"w", 100}, {"h", 50}};
    tool_screenshot(args, &plat);
    EXPECT_EQ(plat.screenshot_calls, 1);
}

// ---------------------------------------------------------------------------
// launch_app
// ---------------------------------------------------------------------------

TEST(ToolLaunchApp, MissingPathIsError) {
    MockPlatform plat;
    const json r = tool_launch_app({}, &plat);
    ASSERT_TRUE(r.contains("content"));
    const auto& c0 = r["content"][0];
    EXPECT_TRUE(c0.value("isError", false));
    EXPECT_NE(c0["text"].get<std::string>().find("path"), std::string::npos);
}

TEST(ToolLaunchApp, SuccessReturnsPid) {
    MockPlatform plat;
    plat.launch_pid = 1234;
    const json args = {{"path", "notepad"}};
    const json r = tool_launch_app(args, &plat);
    EXPECT_FALSE(is_error(r));
    EXPECT_NE(get_text(r).find("1234"), std::string::npos);
}

TEST(ToolLaunchApp, FailureIsError) {
    MockPlatform plat;
    plat.launch_ok = false;
    const json args = {{"path", "badapp"}};
    const json r = tool_launch_app(args, &plat);
    EXPECT_TRUE(is_error(r));
}

// ---------------------------------------------------------------------------
// click / double_click / right_click
// ---------------------------------------------------------------------------

TEST(ToolClick, CallsPlatformClick) {
    MockPlatform plat;
    const json args = {{"x", 100}, {"y", 200}};
    const json r = tool_click(args, &plat);
    EXPECT_FALSE(is_error(r));
    EXPECT_EQ(plat.click_calls, 1);
    EXPECT_EQ(plat.last_click_x, 100);
    EXPECT_EQ(plat.last_click_y, 200);
}

TEST(ToolDoubleClick, CallsPlatformDoubleClick) {
    MockPlatform plat;
    const json args = {{"x", 50}, {"y", 75}};
    const json r = tool_double_click(args, &plat);
    (void)r;
    EXPECT_EQ(plat.double_click_calls, 1);
}

TEST(ToolRightClick, CallsPlatformRightClick) {
    MockPlatform plat;
    const json args = {{"x", 10}, {"y", 10}};
    const json r = tool_right_click(args, &plat);
    (void)r;
    EXPECT_EQ(plat.right_click_calls, 1);
}

TEST(ToolClick, NullPlatformIsError) {
    const json r = tool_click({{"x",0},{"y",0}}, nullptr);
    EXPECT_TRUE(is_error(r));
}

// ---------------------------------------------------------------------------
// scroll
// ---------------------------------------------------------------------------

TEST(ToolScroll, PositiveDeltaScrollsUp) {
    MockPlatform plat;
    const json args = {{"x", 400}, {"y", 300}, {"delta", 3}};
    const json r = tool_scroll(args, &plat);
    EXPECT_FALSE(is_error(r));
    EXPECT_EQ(plat.scroll_calls, 1);
    EXPECT_EQ(plat.last_scroll_delta, 3);
}

TEST(ToolScroll, NegativeDeltaScrollsDown) {
    MockPlatform plat;
    const json args = {{"x", 0}, {"y", 0}, {"delta", -5}};
    const json r = tool_scroll(args, &plat);
    (void)r;
    EXPECT_EQ(plat.last_scroll_delta, -5);
}

// ---------------------------------------------------------------------------
// type_text
// ---------------------------------------------------------------------------

TEST(ToolTypeText, SendsTextToPlatform) {
    MockPlatform plat;
    const json args = {{"text", "hello world"}};
    const json r = tool_type_text(args, &plat);
    EXPECT_FALSE(is_error(r));
    EXPECT_EQ(plat.last_text, "hello world");
    EXPECT_EQ(plat.type_calls, 1);
}

TEST(ToolTypeText, EmptyTextIsError) {
    MockPlatform plat;
    const json args = {{"text", ""}};
    const json r = tool_type_text(args, &plat);
    EXPECT_TRUE(is_error(r));
    EXPECT_EQ(plat.type_calls, 0);
}

TEST(ToolTypeText, MissingTextIsError) {
    MockPlatform plat;
    const json r = tool_type_text({}, &plat);
    ASSERT_TRUE(r.contains("content"));
    EXPECT_TRUE(r["content"][0].value("isError", false));
    EXPECT_EQ(plat.type_calls, 0);
}

// ---------------------------------------------------------------------------
// key_press
// ---------------------------------------------------------------------------

TEST(ToolKeyPress, SendsKeyToPlatform) {
    MockPlatform plat;
    const json args = {{"key", "Enter"}};
    const json r = tool_key_press(args, &plat);
    EXPECT_FALSE(is_error(r));
    EXPECT_EQ(plat.last_key, "Enter");
    EXPECT_EQ(plat.key_calls, 1);
}

TEST(ToolKeyPress, EmptyKeyIsError) {
    MockPlatform plat;
    const json r = tool_key_press({{"key",""}}, &plat);
    EXPECT_TRUE(is_error(r));
    EXPECT_EQ(plat.key_calls, 0);
}

// ---------------------------------------------------------------------------
// maximize_window
// ---------------------------------------------------------------------------

TEST(ToolMaximize, RequiresPidGreaterThanZero) {
    MockPlatform plat;
    const json r = tool_maximize_window({{"pid", 0}}, &plat);
    EXPECT_TRUE(is_error(r));
    EXPECT_EQ(plat.maximize_calls, 0);
}

TEST(ToolMaximize, ValidPidCallsPlatform) {
    MockPlatform plat;
    const json r = tool_maximize_window({{"pid", 999}}, &plat);
    EXPECT_FALSE(is_error(r));
    EXPECT_EQ(plat.maximize_calls, 1);
    EXPECT_EQ(plat.last_pid, 999);
}
