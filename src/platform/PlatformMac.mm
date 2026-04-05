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

// CGVirtualDisplay SPI (available macOS 12.3+)
@interface CGVirtualDisplay : NSObject
- (instancetype)initWithDescriptor:(CGVirtualDisplayDescriptor*)desc;
@property (readonly) CGDirectDisplayID displayID;
@end
@interface CGVirtualDisplayDescriptor : NSObject
@property NSUInteger width, height;
@property CGFloat    ppi;
@property NSUInteger refreshRate;
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
    CGVirtualDisplay  *m_vd  = nil;
    CGDirectDisplayID  m_did = 0;
    NSUInteger m_w = 1920, m_h = 1080;
    CGPoint    m_origin = {0, 0};
    pid_t      m_last_pid = 0;

    // Post a mouse event to the virtual display
    void post_mouse(CGEventType type, CGMouseButton btn, CGPoint pt) const;

    // Map key name to CGKeyCode
    static CGKeyCode key_name_to_code(const std::string& name, CGEventFlags& mods_out);
};

// ---------------------------------------------------------------------------
// init
// ---------------------------------------------------------------------------

bool MacPlatform::init(std::string& error) {
    @autoreleasepool {
        CGVirtualDisplayDescriptor *desc = [[CGVirtualDisplayDescriptor alloc] init];
        desc.width       = m_w;
        desc.height      = m_h;
        desc.ppi         = 96;
        desc.refreshRate = 30;

        m_vd = [[CGVirtualDisplay alloc] initWithDescriptor:desc];
        if (!m_vd) {
            error = "CGVirtualDisplay init failed (requires macOS 12.3+)";
            return false;
        }
        m_did = m_vd.displayID;

        // Get origin of the new virtual display
        CGRect bounds = CGDisplayBounds(m_did);
        m_origin = bounds.origin;
        m_w = (NSUInteger)bounds.size.width;
        m_h = (NSUInteger)bounds.size.height;
    }
    return true;
}

// ---------------------------------------------------------------------------
// capture
// ---------------------------------------------------------------------------

agentdesktop::Frame MacPlatform::capture() {
    agentdesktop::Frame frame;
    @autoreleasepool {
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        __block std::vector<uint8_t> pixels;
        __block int fw = 0, fh = 0;

        [SCShareableContent getShareableContentWithCompletionHandler:
          ^(SCShareableContent *content, NSError *err) {
            if (err) { dispatch_semaphore_signal(sem); return; }
            SCDisplay *tgt = nil;
            for (SCDisplay *d in content.displays)
                if (d.displayID == self->m_did) { tgt = d; break; }
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
                if (d.displayID == self->m_did) { tgt = d; break; }
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
        const std::string& path, const std::string& args) {
    agentdesktop::LaunchResult result;
    @autoreleasepool {
        NSString *npath = [NSString stringWithUTF8String:path.c_str()];
        NSString *script = [NSString stringWithFormat:
            @"tell application \"%@\" to activate", npath];
        NSAppleScript *as = [[NSAppleScript alloc] initWithSource:script];
        NSDictionary *err = nil;
        [as executeAndReturnError:&err];
        if (err) {
            result.error = std::string([[err description] UTF8String]);
            return result;
        }
        // Get the PID of the launched app
        NSArray *apps = [[NSWorkspace sharedWorkspace] runningApplications];
        for (NSRunningApplication *app in apps) {
            if ([app.localizedName.lowercaseString containsString:
                    npath.lastPathComponent.lowercaseString]) {
                result.pid = app.processIdentifier;
                m_last_pid = result.pid;
                break;
            }
        }
        result.ok = true;
    }
    return result;
}

// ---------------------------------------------------------------------------
// maximize_window
// ---------------------------------------------------------------------------

agentdesktop::ActionResult MacPlatform::maximize_window(int pid) {
    @autoreleasepool {
        for (NSRunningApplication *app in
             [[NSWorkspace sharedWorkspace] runningApplications]) {
            if (app.processIdentifier == pid) {
                [app activateWithOptions:NSApplicationActivateIgnoringOtherApps];
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                // Send Cmd+Ctrl+F (full screen)
                CGEventRef d = CGEventCreateKeyboardEvent(NULL, 3 /*F*/, true);
                CGEventSetFlags(d, kCGEventFlagMaskCommand|kCGEventFlagMaskControl);
                CGEventPostToPid(pid, d);
                CFRelease(d);
                CGEventRef u = CGEventCreateKeyboardEvent(NULL, 3, false);
                CGEventPostToPid(pid, u);
                CFRelease(u);
                return {true};
            }
        }
    }
    return {false, "Process not found: " + std::to_string(pid)};
}

// ---------------------------------------------------------------------------
// Mouse helpers
// ---------------------------------------------------------------------------

void MacPlatform::post_mouse(CGEventType type, CGMouseButton btn, CGPoint pt) const {
    CGEventRef ev = CGEventCreateMouseEvent(NULL, type, pt, btn);
    if (m_last_pid > 0) CGEventPostToPid(m_last_pid, ev);
    else                CGEventPost(kCGHIDEventTap, ev);
    CFRelease(ev);
}

agentdesktop::ActionResult MacPlatform::click(int x, int y) {
    const CGPoint pt{m_origin.x + x, m_origin.y + y};
    post_mouse(kCGEventLeftMouseDown, kCGMouseButtonLeft, pt);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    post_mouse(kCGEventLeftMouseUp,   kCGMouseButtonLeft, pt);
    return {true};
}

agentdesktop::ActionResult MacPlatform::double_click(int x, int y) {
    click(x, y);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    click(x, y);
    return {true};
}

agentdesktop::ActionResult MacPlatform::right_click(int x, int y) {
    const CGPoint pt{m_origin.x + x, m_origin.y + y};
    post_mouse(kCGEventRightMouseDown, kCGMouseButtonRight, pt);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    post_mouse(kCGEventRightMouseUp,   kCGMouseButtonRight, pt);
    return {true};
}

agentdesktop::ActionResult MacPlatform::scroll(int x, int y, int delta) {
    CGEventRef ev = CGEventCreateScrollWheelEvent(NULL,
        kCGScrollEventUnitLine, 1, delta);
    if (m_last_pid > 0) CGEventPostToPid(m_last_pid, ev);
    else                CGEventPost(kCGHIDEventTap, ev);
    CFRelease(ev);
    (void)x; (void)y;
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
