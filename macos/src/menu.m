/* macOS menu bar: a "Video" menu that edits scudrace.ini (resolution, fullscreen, smoothing, aspect, scanlines) and relaunches to apply.
   The game reads its settings only at startup, so a change means a restart. */
#import <Cocoa/Cocoa.h>
#include <pthread.h>
#include <unistd.h>
#include <mach-o/dyld.h>
extern char g_game_dir[];
extern void host_get_args(int *argc, char ***argv);

static NSString *ini_path(void) { return [[NSString stringWithUTF8String:g_game_dir] stringByAppendingPathComponent:@"scudrace.ini"]; }
static NSString *ini_get(NSString *key, NSString *def) {
    NSString *t = [NSString stringWithContentsOfFile:ini_path() encoding:NSUTF8StringEncoding error:nil];
    for (NSString *l in [t componentsSeparatedByString:@"\n"])
        if ([l hasPrefix:[key stringByAppendingString:@"="]]) return [l substringFromIndex:key.length + 1];
    return def;
}
static void ini_set(NSString *key, NSString *val) {
    NSString *t = [NSString stringWithContentsOfFile:ini_path() encoding:NSUTF8StringEncoding error:nil] ?: @"";
    NSMutableArray *out = [NSMutableArray array]; BOOL done = NO;
    for (NSString *l in [t componentsSeparatedByString:@"\n"]) {
        if (!l.length) continue;
        if ([l hasPrefix:[key stringByAppendingString:@"="]]) { [out addObject:[NSString stringWithFormat:@"%@=%@", key, val]]; done = YES; } else [out addObject:l];
    }
    if (!done) [out addObject:[NSString stringWithFormat:@"%@=%@", key, val]];
    [[[out componentsJoinedByString:@"\n"] stringByAppendingString:@"\n"] writeToFile:ini_path() atomically:YES encoding:NSUTF8StringEncoding error:nil];
}

@interface SRMenu : NSObject
- (void)pick:(NSMenuItem *)it;
@end
@implementation SRMenu
- (void)pick:(NSMenuItem *)it {
    NSArray *kv = it.representedObject;                   /* @[key, value] */
    NSAlert *a = [[NSAlert alloc] init];
    a.messageText = @"Restart Scud Race to apply?";
    a.informativeText = @"Video settings take effect when the game starts. Any race in progress will be lost.";
    [a addButtonWithTitle:@"Restart"]; [a addButtonWithTitle:@"Cancel"];
    if ([a runModal] != NSAlertFirstButtonReturn) return;
    ini_set(kv[0], kv[1]);
    char exe[4096]; uint32_t n = sizeof exe; int argc; char **argv; host_get_args(&argc, &argv);
    if (_NSGetExecutablePath(exe, &n)) return;
    execv(exe, argv);                                    /* same process image replaced: the new run reads the new ini */
}
- (BOOL)validateMenuItem:(NSMenuItem *)it {
    NSArray *kv = it.representedObject;
    if (kv) it.state = [ini_get(kv[0], @"") isEqualToString:kv[1]] ? NSControlStateValueOn : NSControlStateValueOff;
    return YES;
}
@end

static void add(NSMenu *m, SRMenu *t, NSString *title, NSString *key, NSString *val) {
    NSMenuItem *i = [m addItemWithTitle:title action:@selector(pick:) keyEquivalent:@""];
    i.target = t; i.representedObject = @[key, val];
}
void host_menu_install(void) {
    static BOOL done; if (done || !pthread_main_np()) return; done = YES;
    @autoreleasepool {
        static SRMenu *t; t = [[SRMenu alloc] init];
        NSMenu *bar = [NSApp mainMenu]; if (!bar) { bar = [[NSMenu alloc] init]; [NSApp setMainMenu:bar]; }
        NSMenu *v = [[NSMenu alloc] initWithTitle:@"Video"];
        NSMenu *res = [[NSMenu alloc] initWithTitle:@"Resolution"];
        for (NSNumber *s in @[@1, @2, @3, @4, @6, @8]) {
            NSString *lab = [NSString stringWithFormat:@"%dx  (%d×%d)", s.intValue, 384 * s.intValue, 224 * s.intValue];
            add(res, t, lab, @"scale", s.stringValue);
        }
        [[v addItemWithTitle:@"Resolution" action:nil keyEquivalent:@""] setSubmenu:res];
        [v addItem:[NSMenuItem separatorItem]];
        add(v, t, @"Fullscreen", @"fullscreen", @"1"); add(v, t, @"Windowed", @"fullscreen", @"0");
        [v addItem:[NSMenuItem separatorItem]];
        add(v, t, @"Smooth Scaling", @"smooth", @"1"); add(v, t, @"Sharp Pixels", @"smooth", @"0");
        [v addItem:[NSMenuItem separatorItem]];
        add(v, t, @"4:3 Aspect", @"aspect43", @"1"); add(v, t, @"Wide Aspect", @"aspect43", @"0");
        [v addItem:[NSMenuItem separatorItem]];
        add(v, t, @"Scanlines On", @"scanlines", @"1"); add(v, t, @"Scanlines Off", @"scanlines", @"0");
        NSMenuItem *top = [[NSMenuItem alloc] initWithTitle:@"Video" action:nil keyEquivalent:@""]; [top setSubmenu:v];
        [bar insertItem:top atIndex:MIN(1, (NSInteger)bar.numberOfItems)];
    }
}
