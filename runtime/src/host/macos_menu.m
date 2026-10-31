/*
 * The Display menu:
 *
 *   Display > Internal Resolution > 1x (640x480) .. 4x (2560x1920)
 *   Display > Aspect Ratio        > 4:3 / 16:9
 *   Display > Frame Rate          > 60 fps / 120 fps, Toggle (F10)
 *   Controls > one item per cabinet action (coin, start, fire, reload, test,
 *              service, the frame-rate toggle) showing its key; choosing
 *              one takes the next key pressed in the game window (Escape:
 *              no key). Reset Keys to Defaults.
 *
 * with a check on each current choice.
 *
 * SDL owns the Cocoa application and builds the menu bar when its video
 * starts; the toolkit's window calls vc3_menu_install on the main thread
 * once the window is up (xbox_HostWindowOnOpen), and this adds one more menu
 * to it. The items' actions run on the main thread and only set a value
 * (display_settings.c), an atomic the executor reads at its next flip; the
 * setting is saved there too. The checks are set when the menu opens
 * (validateMenuItem:), so F10 pressed in the game shows up there as well.
 */
#import <Cocoa/Cocoa.h>
#include <stdio.h>

#include "display_settings.h"
#include "host_input.h"

@interface VC3MenuTarget : NSObject <NSMenuItemValidation>
@end

@implementation VC3MenuTarget
- (void)pickRate:(NSMenuItem *)item
{
    vc3_fps_set((int)item.tag);
}

- (void)toggleRate:(NSMenuItem *)item
{
    (void)item;
    vc3_fps_toggle();
}

- (void)pickScale:(NSMenuItem *)item
{
    vc3_scale_set((int)item.tag);
}

- (void)pickAspect:(NSMenuItem *)item
{
    vc3_wide_set((int)item.tag);
}

- (void)learnKey:(NSMenuItem *)item
{
    vc3_key_learn((int)item.tag);
}

- (void)keyDefaults:(NSMenuItem *)item
{
    (void)item;
    vc3_key_defaults();
}

- (BOOL)validateMenuItem:(NSMenuItem *)item
{
    int on = -1;

    if (item.action == @selector(pickRate:))
        on = item.tag == vc3_fps_get();
    else if (item.action == @selector(pickScale:))
        on = item.tag == vc3_scale_get();
    else if (item.action == @selector(pickAspect:))
        on = item.tag == vc3_wide_get();
    else if (item.action == @selector(learnKey:)) {
        int a = (int)item.tag;
        const char *k = vc3_key_name(a);
        item.title = [NSString stringWithFormat:@"%s:  %s", vc3_key_action_name(a),
                      vc3_key_learning() == a ? "press a key in the game window..."
                      : *k ? k : "(none)"];
    } else if (item.action == @selector(toggleRate:)) {
        const char *k = vc3_key_name(VC3_KEY_FPS);
        item.title = *k ? [NSString stringWithFormat:@"Toggle 60/120 fps (%s)", k]
                        : @"Toggle 60/120 fps";
    }
    if (on >= 0)
        item.state = on ? NSControlStateValueOn : NSControlStateValueOff;
    return YES;
}
@end

static VC3MenuTarget *s_target;

static NSMenuItem *choice(NSString *title, SEL action, NSInteger tag)
{
    NSMenuItem *it = [[NSMenuItem alloc] initWithTitle:title action:action
                                         keyEquivalent:@""];
    it.tag = tag;
    it.target = s_target;
    return it;
}

static void add_submenu(NSMenu *parent, NSMenu *sub)
{
    NSMenuItem *it = [[NSMenuItem alloc] initWithTitle:sub.title action:nil
                                         keyEquivalent:@""];
    it.submenu = sub;
    [parent addItem:it];
}

void vc3_menu_install(void)
{
    @autoreleasepool {
        NSMenu *bar = [NSApp mainMenu];
        NSMenu *display, *res, *aspect, *rates, *controls;
        NSMenuItem *top, *it;
        static const int fps[] = { 60, 120 };
        int i;

        if (!bar || s_target)
            return;
        s_target = [VC3MenuTarget new];

        res = [[NSMenu alloc] initWithTitle:@"Internal Resolution"];
        for (i = 1; i <= VC3_SCALE_MAX; i++)
            [res addItem:choice([NSString stringWithFormat:@"%dx (%dx%d)", i,
                                 640 * i, 480 * i], @selector(pickScale:), i)];

        aspect = [[NSMenu alloc] initWithTitle:@"Aspect Ratio"];
        [aspect addItem:choice(@"4:3", @selector(pickAspect:), 0)];
        [aspect addItem:choice(@"16:9 (wider view)",
                               @selector(pickAspect:), 1)];

        rates = [[NSMenu alloc] initWithTitle:@"Frame Rate"];
        for (i = 0; i < (int)(sizeof fps / sizeof fps[0]); i++)
            [rates addItem:choice([NSString stringWithFormat:@"%d fps", fps[i]],
                                  @selector(pickRate:), fps[i])];
        [rates addItem:[NSMenuItem separatorItem]];
        /* The key is the game window's (Controls; F10 by default), shown in
         * the title rather than held by the menu, so a remapped key and the
         * menu never both toggle. */
        it = [[NSMenuItem alloc] initWithTitle:@"Toggle 60/120 fps"
                                        action:@selector(toggleRate:)
                                 keyEquivalent:@""];
        it.target = s_target;
        [rates addItem:it];

        display = [[NSMenu alloc] initWithTitle:@"Display"];
        add_submenu(display, res);
        add_submenu(display, aspect);
        add_submenu(display, rates);

        controls = [[NSMenu alloc] initWithTitle:@"Controls"];
        it = [[NSMenuItem alloc] initWithTitle:@"Mouse: aim; left button fires, right reloads"
                                        action:nil keyEquivalent:@""];
        it.enabled = NO;
        [controls addItem:it];
        [controls addItem:[NSMenuItem separatorItem]];
        for (i = 0; i < VC3_KEY_COUNT; i++)
            [controls addItem:choice([NSString stringWithUTF8String:vc3_key_action_name(i)],
                                     @selector(learnKey:), i)];
        [controls addItem:[NSMenuItem separatorItem]];
        [controls addItem:choice(@"Reset Keys to Defaults", @selector(keyDefaults:), 0)];

        /* Before SDL's Window menu, where a View menu would go. */
        {
            NSInteger at = [bar indexOfItemWithSubmenu:[NSApp windowsMenu]];
            if (at < 0)
                at = bar.numberOfItems;
            top = [[NSMenuItem alloc] initWithTitle:@"Display" action:nil
                                      keyEquivalent:@""];
            top.submenu = display;
            [bar insertItem:top atIndex:at];
            top = [[NSMenuItem alloc] initWithTitle:@"Controls" action:nil
                                      keyEquivalent:@""];
            top.submenu = controls;
            [bar insertItem:top atIndex:at + 1];
        }
        fprintf(stderr, "[VC3] Display menu: Internal Resolution, Aspect Ratio,"
                " Frame Rate; Controls menu: key bindings\n");
    }
}
