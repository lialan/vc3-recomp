/*
 * The Display menu: Display > Frame Rate > 60 fps / 120 fps, with a check
 * on the current rate.
 *
 * SDL owns the Cocoa application and builds the menu bar when its video
 * starts; the toolkit's window calls vc3_menu_install on the main thread
 * once the window is up (xbox_HostWindowOnOpen), and this adds one more menu
 * to it. The items' actions run on the main thread and only set the rate
 * (display_rate.c), an atomic the executor reads at its next flip. The
 * checks are set when the menu opens (validateMenuItem:), so F10 pressed in
 * the game shows up there too.
 */
#import <Cocoa/Cocoa.h>
#include <stdio.h>

#include "display_rate.h"

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

- (BOOL)validateMenuItem:(NSMenuItem *)item
{
    if (item.action == @selector(pickRate:))
        item.state = item.tag == vc3_fps_get() ? NSControlStateValueOn
                                               : NSControlStateValueOff;
    return YES;
}
@end

static VC3MenuTarget *s_target;

void vc3_menu_install(void)
{
    @autoreleasepool {
        NSMenu *bar = [NSApp mainMenu];
        NSMenu *display, *rates;
        NSMenuItem *top, *sub, *it;
        NSString *f10 = [NSString stringWithFormat:@"%C", (unichar)NSF10FunctionKey];
        static const int fps[] = { 60, 120 };
        unsigned i;

        if (!bar || s_target)
            return;
        s_target = [VC3MenuTarget new];

        rates = [[NSMenu alloc] initWithTitle:@"Frame Rate"];
        for (i = 0; i < sizeof fps / sizeof fps[0]; i++) {
            it = [[NSMenuItem alloc]
                    initWithTitle:[NSString stringWithFormat:@"%d fps", fps[i]]
                           action:@selector(pickRate:) keyEquivalent:@""];
            it.tag = fps[i];
            it.target = s_target;
            [rates addItem:it];
        }
        [rates addItem:[NSMenuItem separatorItem]];
        /* F10 with no modifier, as in the game window. */
        it = [[NSMenuItem alloc] initWithTitle:@"Toggle 60/120 fps"
                                        action:@selector(toggleRate:)
                                 keyEquivalent:f10];
        it.keyEquivalentModifierMask = 0;
        it.target = s_target;
        [rates addItem:it];

        display = [[NSMenu alloc] initWithTitle:@"Display"];
        sub = [[NSMenuItem alloc] initWithTitle:@"Frame Rate" action:nil
                                  keyEquivalent:@""];
        sub.submenu = rates;
        [display addItem:sub];

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
        fprintf(stderr, "[VC3] Display menu: Frame Rate 60/120 (F10 toggles)\n");
    }
}
