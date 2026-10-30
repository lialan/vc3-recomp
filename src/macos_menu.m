/*
 * The Display menu:
 *
 *   Display > Internal Resolution > 1x (640x480) .. 4x (2560x1920)
 *   Display > Aspect Ratio        > 4:3 / 16:9
 *   Display > Frame Rate          > 60 fps / 120 fps, Toggle (F10)
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

- (BOOL)validateMenuItem:(NSMenuItem *)item
{
    int on = -1;

    if (item.action == @selector(pickRate:))
        on = item.tag == vc3_fps_get();
    else if (item.action == @selector(pickScale:))
        on = item.tag == vc3_scale_get();
    else if (item.action == @selector(pickAspect:))
        on = item.tag == vc3_wide_get();
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
        NSMenu *display, *res, *aspect, *rates;
        NSMenuItem *top, *it;
        NSString *f10 = [NSString stringWithFormat:@"%C", (unichar)NSF10FunctionKey];
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
        [aspect addItem:choice(@"16:9 (wider view, HUD in the middle)",
                               @selector(pickAspect:), 1)];

        rates = [[NSMenu alloc] initWithTitle:@"Frame Rate"];
        for (i = 0; i < (int)(sizeof fps / sizeof fps[0]); i++)
            [rates addItem:choice([NSString stringWithFormat:@"%d fps", fps[i]],
                                  @selector(pickRate:), fps[i])];
        [rates addItem:[NSMenuItem separatorItem]];
        /* F10 with no modifier, as in the game window. */
        it = [[NSMenuItem alloc] initWithTitle:@"Toggle 60/120 fps"
                                        action:@selector(toggleRate:)
                                 keyEquivalent:f10];
        it.keyEquivalentModifierMask = 0;
        it.target = s_target;
        [rates addItem:it];

        display = [[NSMenu alloc] initWithTitle:@"Display"];
        add_submenu(display, res);
        add_submenu(display, aspect);
        add_submenu(display, rates);

        top = [[NSMenuItem alloc] initWithTitle:@"Display" action:nil
                                  keyEquivalent:@""];
        top.submenu = display;
        /* Before SDL's Window menu, where a View menu would go. */
        {
            NSInteger at = [bar indexOfItemWithSubmenu:[NSApp windowsMenu]];
            if (at < 0)
                at = bar.numberOfItems;
            [bar insertItem:top atIndex:at];
        }
        fprintf(stderr, "[VC3] Display menu: Internal Resolution, Aspect Ratio,"
                " Frame Rate (F10 toggles 60/120)\n");
    }
}
