/**
 * @file PlatformMac.mm
 * @brief macOS implementation of the Platform interface.
 *
 * Virtual desktop
 * ---------------
 * Uses CGVirtualDisplay (private SPI, macOS 12.3+) to create an isolated
 * virtual display. Applications launched on this display are invisible to
 * the user's current session.
 *
 * Screen capture
 * --------------
 * Uses ScreenCaptureKit (macOS 12.3+) SCScreenshotManager for high-quality
 * synchronous frame capture. Falls back to CGDisplayCreateImageForRect on
 * older systems.
 *
 * Input
 * -----
 * Mouse and keyboard events are posted via CGEvent. Events are targeted at
 * the last-launched process (m_last_pid) when available.
 *
 * Permissions
 * -----------
 * Screen Recording permission is required for capture.
 * Accessibility permission is required for keyboard/mouse injection.
 *
 * @copyright AgentDesktop Project
 */

#import <Cocoa/Cocoa.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ApplicationServices/ApplicationServices.h>

#include "Platform.h"

#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

// CGVirtualDisplay SPI (available macOS 12.4+)
@interface CGVirtualDisplayMode : NSObject
- (instancetype)initWithWidth:(NSUInteger)width
                       height:(NSUInteger)height
                  refreshRate:(double)refreshRate;
@property (nonatomic, readonly) NSUInteger width;
@property (nonatomic, readonly) NSUInteger height;
@end

@interface CGVirtualDisplaySettings : NSObject
@property (nonatomic, copy)   NSArray    *modes;
@property (nonatomic, assign) NSUInteger  hiDPI;
@end

@interface CGVirtualDisplayDescriptor : NSObject
@property (nonatomic, copy)             NSString         *name;
@property (nonatomic, assign)           NSUInteger        maxPixelsWide;
@property (nonatomic, assign)           NSUInteger        maxPixelsHigh;
@property (nonatomic, assign)           NSUInteger        vendorID;
@property (nonatomic, assign)           NSUInteger        productID;
@property (nonatomic, assign)           NSUInteger        serialNumber;
@property (nullable, nonatomic, strong) dispatch_queue_t  queue;
@property (nullable, nonatomic, copy)   dispatch_block_t  terminationHandler;
@end

@interface CGVirtualDisplay : NSObject
- (nullable instancetype)initWithDescriptor:(CGVirtualDisplayDescriptor *)descriptor;
- (void)applySettings:(CGVirtualDisplaySettings *)settings;
@property (nonatomic, readonly) CGDirectDisplayID displayID;
@end

namespace {

class MacPlatform : public agentdesktop::Platform {
public:
    MacPlatform()  = default;
    ~MacPlatform() override {
        if (m_vd) { m_vd = nil; }
    }

    bool init(std::string& error) override;
    bool prepare_capture_thread(std::string& /*error*/) override { return true; }
    agentdesktop::Frame             capture() override;
    agentdesktop::LaunchResult      launch_app(const std::string& path,
                                                const std::string& args) override;
    agentdesktop::ActionResult      maximize_window(int pid) override;
    agentdesktop::ActionResult      click(int x, int y) override;
    agentdesktop::ActionResult      double_click(int x, int y) override;
    agentdesktop::ActionResult      right_click(int x, int y) override;
    agentdesktop::ActionResult      scroll(int x, int y, int delta) override;
    agentdesktop::ActionResult      type_text(const std::string& text) override;
    agentdesktop::ActionResult      key_press(const std::string& key) override;
    agentdesktop::ScreenshotResult  screenshot(int rx = 0, int ry = 0,
                                                int rw = 0, int rh = 0) override;
    int phys_width()  const override { return (int)m_w; }
    int phys_height() const override { return (int)m_h; }

private:
    CGVirtualDisplay     *m_vd         = nil;
    CGDirectDisplayID     m_did        = 0;
    NSUInteger            m_w          = 1920, m_h = 1080;
    CGPoint               m_origin     = {0, 0};
    pid_t                 m_last_pid   = 0;
    NSRunningApplication *m_target_app = nil;

    // Hide cursor → warp to pt → post HID mouse events → warp back → show cursor.
    // Completes in <0.5 ms so the cursor never visibly moves.
    static void post_mouse_event(CGPoint pt, CGMouseButton btn,
                                 CGEventType down_t, CGEventType up_t,
                                 int click_count = 1);

    // Activate the target app so clicks land on the right element.
    void ensure_active() const;

    // Map key name to CGKeyCode
    static CGKeyCode key_name_to_code(const std::string& name, CGEventFlags& mods_out);
};

// ---------------------------------------------------------------------------
// init
// ---------------------------------------------------------------------------

bool MacPlatform::init(std::string& error) {
    // CGVirtualDisplay and other AppKit APIs must run on the main thread.
    // do_connect() calls init() from a background std::thread, so we
    // dispatch_sync to the main queue.  If we're already on the main thread
    // (e.g. MCP mode) we call the block directly to avoid deadlock.
    __block bool         ok  = false;
    __block std::string  blk_err;
    __block CGDirectDisplayID blk_did = 0;

    // CGVirtualDisplay setup requires the main run loop. dispatch_sync to the
    // main queue so the background worker thread waits while GLFW drives the loop.
    dispatch_sync(dispatch_get_main_queue(), ^{
        @autoreleasepool {
            [NSApplication sharedApplication];

            CGVirtualDisplayMode *mode = [[CGVirtualDisplayMode alloc]
                initWithWidth:m_w height:m_h refreshRate:30.0];

            CGVirtualDisplaySettings *settings = [[CGVirtualDisplaySettings alloc] init];
            settings.hiDPI = 0;
            settings.modes = @[mode];

            CGVirtualDisplayDescriptor *desc = [[CGVirtualDisplayDescriptor alloc] init];
            desc.name          = @"AgentDesktop VD";
            desc.maxPixelsWide = m_w;
            desc.maxPixelsHigh = m_h;
            desc.vendorID      = 0x1234;
            desc.productID     = 0x0001;
            desc.serialNumber  = 1;
            desc.queue         = dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0);

            m_vd = [[CGVirtualDisplay alloc] initWithDescriptor:desc];
            if (!m_vd) {
                blk_err = "CGVirtualDisplay initWithDescriptor returned nil (requires macOS 12.4+)";
                return;
            }
            blk_did = m_vd.displayID;

            // Give macOS time to register the new virtual display
            [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:1.0]];
            [m_vd applySettings:settings];

            // Poll until display reports full dimensions (up to 5 s)
            CGRect bounds = CGRectZero;
            for (int i = 0; i < 50; ++i) {
                [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
                bounds = CGDisplayBounds(blk_did);
                if (bounds.size.width >= (CGFloat)m_w &&
                    bounds.size.height >= (CGFloat)m_h)
                    break;
            }
            m_origin = bounds.origin;
            ok = true;
        }
    });

    if (!ok) { error = blk_err; return false; }
    m_did = blk_did;

    return true;
}

// ---------------------------------------------------------------------------
// capture
// ---------------------------------------------------------------------------

agentdesktop::Frame MacPlatform::capture() {
    agentdesktop::Frame frame;
    if (m_w == 0 || m_h == 0) return frame;  // display not ready yet
    @autoreleasepool {
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        __block std::vector<uint8_t> pixels;
        __block int fw = 0, fh = 0;

        [SCShareableContent getShareableContentWithCompletionHandler:
          ^(SCShareableContent *content, NSError *err) {
            if (err) { dispatch_semaphore_signal(sem); return; }
            SCDisplay *tgt = nil;
            for (SCDisplay *d in content.displays)
                if (d.displayID == m_did) { tgt = d; break; }
            if (!tgt) { dispatch_semaphore_signal(sem); return; }

            SCContentFilter *f  = [[SCContentFilter alloc]
                initWithDisplay:tgt excludingWindows:@[]];
            SCStreamConfiguration *sc = [[SCStreamConfiguration alloc] init];
            sc.width       = CAPTURE_W;
            sc.height      = (int)(m_h * CAPTURE_W / m_w);
            sc.scalesToFit = YES;
            sc.showsCursor = NO;

            [SCScreenshotManager captureImageWithFilter:f configuration:sc
              completionHandler:^(CGImageRef img, NSError *ce) {
                if (ce || !img) { dispatch_semaphore_signal(sem); return; }
                fw = (int)CGImageGetWidth(img);
                fh = (int)CGImageGetHeight(img);
                CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
                pixels.resize((size_t)fw * fh * 4);
                CGContextRef ctx = CGBitmapContextCreate(
                    pixels.data(), fw, fh, 8, fw*4, cs,
                    kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
                CGContextDrawImage(ctx, CGRectMake(0,0,fw,fh), img);
                CGContextRelease(ctx); CGColorSpaceRelease(cs);
                dispatch_semaphore_signal(sem);
            }];
        }];

        dispatch_semaphore_wait(sem,
            dispatch_time(DISPATCH_TIME_NOW, 5LL * NSEC_PER_SEC));

        frame.pixels      = std::move(pixels);
        frame.width       = fw;
        frame.height      = fh;
        frame.phys_width  = (int)m_w;
        frame.phys_height = (int)m_h;
    }
    return frame;
}

// ---------------------------------------------------------------------------
// screenshot
// ---------------------------------------------------------------------------

agentdesktop::ScreenshotResult MacPlatform::screenshot(int rx, int ry, int rw, int rh) {
    agentdesktop::ScreenshotResult result;
    @autoreleasepool {
        if (rw <= 0 || rh <= 0) { rx=0; ry=0; rw=(int)m_w; rh=(int)m_h; }
        if (rx < 0) rx=0; if (ry < 0) ry=0;
        if (rx+rw > (int)m_w) rw=(int)m_w-rx;
        if (ry+rh > (int)m_h) rh=(int)m_h-ry;
        if (rw<=0||rh<=0) { result.error="Invalid region"; return result; }

        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        __block agentdesktop::ScreenshotResult blk;

        [SCShareableContent getShareableContentWithCompletionHandler:
          ^(SCShareableContent *content, NSError *err) {
            if (err) { dispatch_semaphore_signal(sem); return; }
            SCDisplay *tgt = nil;
            for (SCDisplay *d in content.displays)
                if (d.displayID == m_did) { tgt = d; break; }
            if (!tgt) { dispatch_semaphore_signal(sem); return; }

            SCContentFilter *f  = [[SCContentFilter alloc]
                initWithDisplay:tgt excludingWindows:@[]];
            SCStreamConfiguration *sc = [[SCStreamConfiguration alloc] init];
            sc.width = (int)m_w; sc.height = (int)m_h;
            sc.scalesToFit = NO; sc.showsCursor = NO;

            [SCScreenshotManager captureImageWithFilter:f configuration:sc
              completionHandler:^(CGImageRef img, NSError *ce) {
                if (ce||!img) { dispatch_semaphore_signal(sem); return; }
                size_t fw2=(size_t)CGImageGetWidth(img),
                       fh2=(size_t)CGImageGetHeight(img);
                CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
                std::vector<uint8_t> full(fw2*fh2*4);
                CGContextRef ctx = CGBitmapContextCreate(
                    full.data(),fw2,fh2,8,fw2*4,cs,
                    kCGImageAlphaNoneSkipFirst|kCGBitmapByteOrder32Little);
                CGContextDrawImage(ctx,CGRectMake(0,0,fw2,fh2),img);
                CGContextRelease(ctx); CGColorSpaceRelease(cs);
                // Crop
                blk.pixels.resize((size_t)rw*rh*4);
                for(int y=0;y<rh;++y)
                    memcpy(blk.pixels.data()+(size_t)y*rw*4,
                           full.data()+((size_t)(ry+y)*fw2+rx)*4,
                           (size_t)rw*4);
                blk.width=rw; blk.height=rh; blk.ok=true;
                dispatch_semaphore_signal(sem);
            }];
        }];
        dispatch_semaphore_wait(sem,
            dispatch_time(DISPATCH_TIME_NOW, 15LL*NSEC_PER_SEC));
        result = std::move(blk);
    }
    return result;
}

// ---------------------------------------------------------------------------
// launch_app
// ---------------------------------------------------------------------------

agentdesktop::LaunchResult MacPlatform::launch_app(
        const std::string& path, const std::string& /*args*/) {
    agentdesktop::LaunchResult result;
    @autoreleasepool {
        NSString *appName = [NSString stringWithUTF8String:path.c_str()];

        // Step 1: Warp cursor to VD centre before launch so the new window
        // appears on the virtual display instead of the user's main screen.
        CGEventRef q        = CGEventCreate(NULL);
        CGPoint    saved    = CGEventGetLocation(q);
        CFRelease(q);
        CGPoint vd_center   = CGPointMake(m_origin.x + m_w / 2.0,
                                          m_origin.y + m_h / 2.0);
        CGWarpMouseCursorPosition(vd_center);
        CGAssociateMouseAndMouseCursorPosition(false); // freeze visible cursor

        // Step 2: Launch in background (open -ga resolves any app name).
        std::string cmd = "open -ga '" + path + "'";
        system(cmd.c_str());

        CGAssociateMouseAndMouseCursorPosition(true);
        CGWarpMouseCursorPosition(saved);

        // Step 3: Wait up to 10 s for the process to appear.
        NSRunningApplication *launched = nil;
        for (int i = 0; i < 100 && !launched; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            for (NSRunningApplication *app in
                     [[NSWorkspace sharedWorkspace] runningApplications]) {
                if (!app.terminated &&
                    [app.localizedName caseInsensitiveCompare:appName]
                        == NSOrderedSame) {
                    launched = app;
                    break;
                }
            }
        }
        if (!launched) {
            result.error = "Process not found after launch: " + path;
            return result;
        }
        result.pid = (int)launched.processIdentifier;
        m_last_pid = result.pid;

        // Step 4: Wait for finish launching (up to 5 s).
        for (int i = 0; i < 100 && !launched.isFinishedLaunching; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        // Step 5: AX API — force-position window onto the virtual display.
        AXUIElementRef ax_app = AXUIElementCreateApplication(result.pid);
        CFArrayRef windows = NULL;
        for (int i = 0; i < 30; ++i) {
            if (windows) { CFRelease(windows); windows = NULL; }
            AXUIElementCopyAttributeValue(ax_app, kAXWindowsAttribute,
                                          (CFTypeRef *)&windows);
            if (windows && CFArrayGetCount(windows) > 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (windows && CFArrayGetCount(windows) > 0) {
            AXUIElementRef win = (AXUIElementRef)CFArrayGetValueAtIndex(windows, 0);
            CGPoint pos = { m_origin.x, m_origin.y };
            CGSize  sz  = { (CGFloat)m_w, (CGFloat)m_h };
            AXValueRef pv = AXValueCreate((AXValueType)kAXValueCGPointType, &pos);
            AXValueRef sv = AXValueCreate((AXValueType)kAXValueCGSizeType,  &sz);
            AXUIElementSetAttributeValue(win, kAXPositionAttribute, pv);
            AXUIElementSetAttributeValue(win, kAXSizeAttribute,     sv);
            CFRelease(pv); CFRelease(sv);
        }
        if (windows) CFRelease(windows);
        CFRelease(ax_app);

        m_target_app = launched;
        result.ok = true;
    }
    return result;
}

// ---------------------------------------------------------------------------
// maximize_window
// ---------------------------------------------------------------------------

agentdesktop::ActionResult MacPlatform::maximize_window(int /*pid*/) {
    // Window is already positioned on the virtual display by launch_app via AX API.
    return {true};
}

// ---------------------------------------------------------------------------
// Mouse helpers
// ---------------------------------------------------------------------------


// Hide cursor → warp to pt → post DOWN+UP → warp back to saved pos → show cursor.
// Completes in <0.5 ms (sub-frame) so the cursor never visibly moves on the
// user's physical display. CGEventPost(kCGHIDEventTap) reaches every app layer.
void MacPlatform::post_mouse_event(CGPoint pt, CGMouseButton btn,
                                    CGEventType down_t, CGEventType up_t,
                                    int click_count) {
    CGEventRef query = CGEventCreate(NULL);
    CGPoint    saved = CGEventGetLocation(query);
    CFRelease(query);

    CGDisplayHideCursor(kCGDirectMainDisplay);
    CGWarpMouseCursorPosition(pt);

    CGEventRef d = CGEventCreateMouseEvent(NULL, down_t, pt, btn);
    CGEventRef u = CGEventCreateMouseEvent(NULL, up_t,   pt, btn);
    CGEventSetIntegerValueField(d, kCGMouseEventClickState, click_count);
    CGEventSetIntegerValueField(u, kCGMouseEventClickState, click_count);
    CGEventPost(kCGHIDEventTap, d);
    CGEventPost(kCGHIDEventTap, u);
    CFRelease(d);
    CFRelease(u);

    CGWarpMouseCursorPosition(saved);
    CGDisplayShowCursor(kCGDirectMainDisplay);
}

// Activate the target app before clicking so the click lands on the right
// element instead of being consumed as a window-activation event.
void MacPlatform::ensure_active() const {
    if (!m_target_app || m_target_app.terminated || m_target_app.isActive) return;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    [m_target_app activateWithOptions:NSApplicationActivateIgnoringOtherApps];
#pragma clang diagnostic pop
    for (int i = 0; i < 8 && !m_target_app.isActive; ++i)
        usleep(10000); // 10 ms per poll, up to 80 ms total
}

agentdesktop::ActionResult MacPlatform::click(int x, int y) {
    if (!AXIsProcessTrusted())
        return {false, "Accessibility permission required — enable in System Settings"};
    ensure_active();
    const CGPoint pt{m_origin.x + x, m_origin.y + y};
    post_mouse_event(pt, kCGMouseButtonLeft,
                     kCGEventLeftMouseDown, kCGEventLeftMouseUp);
    return {true};
}

agentdesktop::ActionResult MacPlatform::double_click(int x, int y) {
    if (!AXIsProcessTrusted())
        return {false, "Accessibility permission required"};
    ensure_active();
    const CGPoint pt{m_origin.x + x, m_origin.y + y};
    post_mouse_event(pt, kCGMouseButtonLeft,
                     kCGEventLeftMouseDown, kCGEventLeftMouseUp, 1);
    post_mouse_event(pt, kCGMouseButtonLeft,
                     kCGEventLeftMouseDown, kCGEventLeftMouseUp, 2);
    return {true};
}

agentdesktop::ActionResult MacPlatform::right_click(int x, int y) {
    if (!AXIsProcessTrusted())
        return {false, "Accessibility permission required"};
    ensure_active();
    const CGPoint pt{m_origin.x + x, m_origin.y + y};
    post_mouse_event(pt, kCGMouseButtonRight,
                     kCGEventRightMouseDown, kCGEventRightMouseUp);
    return {true};
}

agentdesktop::ActionResult MacPlatform::scroll(int x, int y, int delta) {
    const CGPoint pt{m_origin.x + x, m_origin.y + y};
    CGEventRef query = CGEventCreate(NULL);
    CGPoint    saved = CGEventGetLocation(query);
    CFRelease(query);

    CGDisplayHideCursor(kCGDirectMainDisplay);
    CGWarpMouseCursorPosition(pt);

    CGEventRef ev = CGEventCreateScrollWheelEvent(NULL,
        kCGScrollEventUnitLine, 1, delta);
    CGEventSetLocation(ev, pt);
    CGEventPost(kCGHIDEventTap, ev);
    CFRelease(ev);

    CGWarpMouseCursorPosition(saved);
    CGDisplayShowCursor(kCGDirectMainDisplay);
    return {true};
}

// ---------------------------------------------------------------------------
// type_text / key_press
// ---------------------------------------------------------------------------

agentdesktop::ActionResult MacPlatform::type_text(const std::string& text) {
    @autoreleasepool {
        NSString *ns = [NSString stringWithUTF8String:text.c_str()];
        for (NSUInteger i = 0; i < ns.length; ++i) {
            unichar ch = [ns characterAtIndex:i];
            CGEventRef d = CGEventCreateKeyboardEvent(NULL, 0, true);
            CGEventRef u = CGEventCreateKeyboardEvent(NULL, 0, false);
            CGEventKeyboardSetUnicodeString(d, 1, &ch);
            CGEventKeyboardSetUnicodeString(u, 1, &ch);
            if (m_last_pid > 0) {
                CGEventPostToPid(m_last_pid, d);
                CGEventPostToPid(m_last_pid, u);
            } else {
                CGEventPost(kCGHIDEventTap, d);
                CGEventPost(kCGHIDEventTap, u);
            }
            CFRelease(d); CFRelease(u);
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
    }
    return {true};
}

CGKeyCode MacPlatform::key_name_to_code(const std::string& name, CGEventFlags& mods) {
    mods = 0;
    std::string k = name;
    auto eat = [&](const char* prefix, CGEventFlags flag) {
        if (k.size() > strlen(prefix) &&
            k.compare(0,strlen(prefix),prefix)==0) {
            mods |= flag; k = k.substr(strlen(prefix)); }
    };
    eat("ctrl+",  kCGEventFlagMaskControl);
    eat("alt+",   kCGEventFlagMaskAlternate);
    eat("shift+", kCGEventFlagMaskShift);
    eat("cmd+",   kCGEventFlagMaskCommand);

    static const std::pair<const char*, CGKeyCode> kMap[] = {
        {"Return",    36}, {"Enter",     36}, {"Escape",   53},
        {"Tab",       48}, {"Backspace", 51}, {"Delete",  117},
        {"Space",     49}, {"Home",      115},{"End",     119},
        {"PageUp",   116}, {"PageDown", 121}, {"Insert",   99},
        {"ArrowLeft", 123},{"ArrowRight",124},{"ArrowDown",125},{"ArrowUp",126},
        {"F1",122},{"F2",120},{"F3",99},{"F4",118},{"F5",96},
        {"F6",97},{"F7",98},{"F8",100},{"F9",101},{"F10",109},
        {"F11",103},{"F12",111},
    };
    for (auto& [nm, code] : kMap)
        if (k == nm) return code;
    return 0xFFFF;
}

agentdesktop::ActionResult MacPlatform::key_press(const std::string& key) {
    CGEventFlags mods = 0;
    const CGKeyCode code = key_name_to_code(key, mods);
    if (code == 0xFFFF) return {false, "Unknown key: " + key};

    CGEventRef d = CGEventCreateKeyboardEvent(NULL, code, true);
    CGEventRef u = CGEventCreateKeyboardEvent(NULL, code, false);
    if (mods) {
        CGEventSetFlags(d, mods | CGEventGetFlags(d));
        CGEventSetFlags(u, mods | CGEventGetFlags(u));
    }
    if (m_last_pid > 0) {
        CGEventPostToPid(m_last_pid, d);
        CGEventPostToPid(m_last_pid, u);
    } else {
        CGEventPost(kCGHIDEventTap, d);
        CGEventPost(kCGHIDEventTap, u);
    }
    CFRelease(d); CFRelease(u);
    return {true};
}

} // anonymous namespace

agentdesktop::Platform* create_platform() {
    return new MacPlatform();
}
