/*
 * tools/mx250_menubar.m
 *
 * Featherweight macOS Menu Bar Monitor for NVIDIA GeForce MX250 (< 0.02% CPU).
 *   - Uses NSApplicationActivationPolicyAccessory (no Dock icon clutter)
 *   - Polls gp108_poll_snapshot(&snap, 0) every 2.0s with 0.5s timer coalescing
 *   - Strict D3off sleep guard (never wakes sleeping GPU)
 *   - Live GPU-Util %, Die Temp °C, GDDR5 VRAM MiB, Active Processes,
 *     1-click 3D Demo launcher, and 1-click D0 / D3off Power Toggle.
 */

#import <Cocoa/Cocoa.h>
#include <dlfcn.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern char **environ;

struct GP108ProcSnapshot {
    int32_t  pid;
    uint32_t engine_flags;
    uint32_t vram_mib;
    uint32_t gart_mib;
    char     comm[64];
};

struct GP108LiveSnapshot {
    uint32_t power_state;
    int32_t  temp_celsius;
    uint32_t gpu_util_pct;
    uint32_t vram_used_mib;
    uint32_t vram_total_mib;
    uint32_t pcie_gen;
    uint32_t pcie_width;
    uint32_t pcie_max_gen;
    uint32_t pcie_max_width;
    uint64_t irq_count;
    uint32_t pmc_boot_0;
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t subvendor_id;
    uint16_t subdevice_id;
    uint8_t  bus, device, function;
    uint32_t proc_count;
    struct GP108ProcSnapshot procs[16];
};

typedef int  (*pfn_poll_snapshot_t)(struct GP108LiveSnapshot *, uint32_t);
typedef int  (*pfn_set_power_t)(int);
typedef void (*pfn_get_clocks_t)(uint32_t *, uint32_t *, uint32_t *);

@interface MX250MenuController : NSObject <NSMenuDelegate>
@property(nonatomic, strong) NSStatusItem *statusItem;
@property(nonatomic, strong) NSMenu *menu;
@property(nonatomic, strong) NSMenuItem *stateItem;
@property(nonatomic, strong) NSMenuItem *utilTempItem;
@property(nonatomic, strong) NSMenuItem *vramItem;
@property(nonatomic, strong) NSMenuItem *irqItem;
@property(nonatomic, strong) NSMenuItem *procHeaderItem;
@property(nonatomic, strong) NSMutableArray<NSMenuItem *> *procItems;
@property(nonatomic, strong) NSMenuItem *powerToggleItem;
@property(nonatomic, strong) NSTimer *pollTimer;
@property(nonatomic, assign) pfn_poll_snapshot_t fnPoll;
@property(nonatomic, assign) pfn_set_power_t fnSetPower;
@property(nonatomic, assign) pfn_get_clocks_t fnGetClocks;
@property(nonatomic, assign) struct GP108LiveSnapshot lastSnap;
@end

@implementation MX250MenuController

- (instancetype)initWithSmiDylib:(const char *)smiPath {
    self = [super init];
    if (self) {
        void *dl = dlopen(smiPath, RTLD_NOW | RTLD_LOCAL);
        if (!dl) {
            dl = dlopen("/Library/Application Support/NVIDIA-GP108/lib/libnvidia_smi_macos.dylib",
                        RTLD_NOW | RTLD_LOCAL);
        }
        if (dl) {
            _fnPoll      = (pfn_poll_snapshot_t)dlsym(dl, "gp108_poll_snapshot");
            _fnSetPower  = (pfn_set_power_t)dlsym(dl, "gp108_set_power_state");
            _fnGetClocks = (pfn_get_clocks_t)dlsym(dl, "gp108_get_live_clocks");
        }
        _procItems = [[NSMutableArray alloc] init];
        [self setupStatusBar];
        [self refreshTelemetry];

        /* On Mode B login / wake, if MX250 was left in raw BIOS D0 with 0 active clients,
         * automatically cut power to D3cold (0W) so battery life stays maxed out!
         */
        uint32_t gpc = 0, mem = 0, rm_act = 0;
        if (_fnGetClocks) _fnGetClocks(&gpc, &mem, &rm_act);
        if (_lastSnap.power_state && !rm_act && _lastSnap.proc_count == 0 && _fnSetPower) {
            _fnSetPower(0);
            [self refreshTelemetry];
        }

        [[[NSWorkspace sharedWorkspace] notificationCenter]
            addObserver:self
               selector:@selector(onSystemWake:)
                   name:NSWorkspaceDidWakeNotification
                 object:nil];

        _pollTimer = [NSTimer scheduledTimerWithTimeInterval:2.0
                                                      target:self
                                                    selector:@selector(refreshTelemetry)
                                                    userInfo:nil
                                                     repeats:YES];
        _pollTimer.tolerance = 0.5;
        [[NSRunLoop mainRunLoop] addTimer:_pollTimer forMode:NSRunLoopCommonModes];
    }
    return self;
}

- (void)onSystemWake:(NSNotification *)note {
    (void)note;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(2.5 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        [self refreshTelemetry];
        uint32_t gpc = 0, mem = 0, rm_act = 0;
        if (_fnGetClocks) _fnGetClocks(&gpc, &mem, &rm_act);
        if (_lastSnap.power_state && !rm_act && _lastSnap.proc_count == 0 && _fnSetPower) {
            _fnSetPower(0);
            [self refreshTelemetry];
        }
    });
}

- (void)setupStatusBar {
    _statusItem = [[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength];
    _statusItem.button.font = [NSFont monospacedDigitSystemFontOfSize:12.0 weight:NSFontWeightMedium];
    _statusItem.button.title = @"MX250 --";

    _menu = [[NSMenu alloc] initWithTitle:@"MX250 Monitor"];
    _menu.delegate = self;
    _menu.autoenablesItems = NO;

    NSMenuItem *hdr = [[[NSMenuItem alloc] initWithTitle:@"NVIDIA GeForce MX250 (2 GB GDDR5)"
                                                  action:nil
                                           keyEquivalent:@""] autorelease];
    hdr.enabled = NO;
    [_menu addItem:hdr];

    _stateItem = [[[NSMenuItem alloc] initWithTitle:@"状态：正在读取..." action:nil keyEquivalent:@""] autorelease];
    _stateItem.enabled = NO;
    [_menu addItem:_stateItem];

    _utilTempItem = [[[NSMenuItem alloc] initWithTitle:@"核心：0%  |  温度：--°C" action:nil keyEquivalent:@""] autorelease];
    _utilTempItem.enabled = NO;
    [_menu addItem:_utilTempItem];

    _vramItem = [[[NSMenuItem alloc] initWithTitle:@"显存：0 MiB / 2048 MiB" action:nil keyEquivalent:@""] autorelease];
    _vramItem.enabled = NO;
    [_menu addItem:_vramItem];

    _irqItem = [[[NSMenuItem alloc] initWithTitle:@"PCIe：Gen1 x4  |  中断：0" action:nil keyEquivalent:@""] autorelease];
    _irqItem.enabled = NO;
    [_menu addItem:_irqItem];

    [_menu addItem:[NSMenuItem separatorItem]];

    _procHeaderItem = [[[NSMenuItem alloc] initWithTitle:@"活跃独显进程 (Vulkan / NVK)："
                                                  action:nil
                                           keyEquivalent:@""] autorelease];
    _procHeaderItem.enabled = NO;
    [_menu addItem:_procHeaderItem];

    [_menu addItem:[NSMenuItem separatorItem]];

    NSMenuItem *demoItem = [[[NSMenuItem alloc] initWithTitle:@"启动 3D 实时渲染演示 (mx250-demo)"
                                                       action:@selector(launchDemo:)
                                                keyEquivalent:@"d"] autorelease];
    demoItem.target = self;
    demoItem.enabled = YES;
    [_menu addItem:demoItem];

    _powerToggleItem = [[[NSMenuItem alloc] initWithTitle:@"切换独显电源 (休眠省电 / 唤醒)"
                                                   action:@selector(togglePower:)
                                            keyEquivalent:@"p"] autorelease];
    _powerToggleItem.target = self;
    _powerToggleItem.enabled = YES;
    [_menu addItem:_powerToggleItem];

    [_menu addItem:[NSMenuItem separatorItem]];

    NSMenuItem *quitItem = [[[NSMenuItem alloc] initWithTitle:@"退出 MX250 状态栏监控"
                                                       action:@selector(quitApp:)
                                                keyEquivalent:@"q"] autorelease];
    quitItem.target = self;
    quitItem.enabled = YES;
    [_menu addItem:quitItem];

    _statusItem.menu = _menu;
}

- (void)refreshTelemetry {
    if (!_fnPoll) return;
    struct GP108LiveSnapshot s;
    if (_fnPoll(&s, 0) != 0) {
        _statusItem.button.title = @"MX250 离线";
        return;
    }
    _lastSnap = s;

    if (!s.power_state) {
        _statusItem.button.title = @"MX250 0W";
        _stateItem.title = @"状态：D3cold 断电休眠 (0W 零功耗 | 按需自动唤醒)";
        _utilTempItem.title = @"核心占用：0%   |   核心温度：已断电";
        _vramItem.title = [NSString stringWithFormat:@"显存占用：0 MiB / %u MiB", s.vram_total_mib];
        _irqItem.title = @"模式 B：调用 Vulkan / Metal 时自动 ~1.5s 满血唤醒";
        _powerToggleItem.title = @"⚡ 手动唤醒 MX250 独显 (满血 P0)";
        _powerToggleItem.enabled = YES;
    } else {
        uint32_t gpc = 0, mem = 0, rm_act = 0;
        if (_fnGetClocks) _fnGetClocks(&gpc, &mem, &rm_act);
        NSString *tStr = (s.temp_celsius >= 0)
            ? [NSString stringWithFormat:@"%d°C", s.temp_celsius] : @"--°C";
        NSString *tag = rm_act ? @"P0" : @"D0";
        if (s.gpu_util_pct > 0 || s.proc_count > 0) {
            _statusItem.button.title = [NSString stringWithFormat:@"⚡MX250 %@ %u%% %@",
                                        tag, s.gpu_util_pct, tStr];
        } else {
            _statusItem.button.title = [NSString stringWithFormat:@"MX250 %@ %@", tag, tStr];
        }
        if (rm_act) {
            _stateItem.title = [NSString stringWithFormat:@"状态：满血 P0 在线 (%u MHz / %u Mbps | 空闲60s自动0W)",
                                gpc, mem * 2];
        } else {
            _stateItem.title = [NSString stringWithFormat:@"状态：D0 在线 (0000:%02X:%02X.%X)",
                                s.bus, s.device, s.function];
        }
        _utilTempItem.title = [NSString stringWithFormat:@"核心占用：%u%%   |   核心温度：%@",
                               s.gpu_util_pct, tStr];
        _vramItem.title = [NSString stringWithFormat:@"显存占用：%u MiB / %u MiB GDDR5",
                           s.vram_used_mib, s.vram_total_mib];
        _irqItem.title = [NSString stringWithFormat:@"PCIe 链路：Gen%u x%u   |   提交：%llu",
                          s.pcie_gen, s.pcie_width, (unsigned long long)s.irq_count];

        if (s.proc_count > 0) {
            _powerToggleItem.title = @"正在运行任务 (退出后空闲 60s 自动 0W 断电)";
            _powerToggleItem.enabled = NO;
        } else {
            _powerToggleItem.title = @"🌙 立即断电休眠 MX250 (D3cold 0W 省电)";
            _powerToggleItem.enabled = YES;
        }
    }

    /* Rebuild active process items under _procHeaderItem */
    for (NSMenuItem *old in _procItems) {
        [_menu removeItem:old];
    }
    [_procItems removeAllObjects];

    NSInteger insertIdx = [_menu indexOfItem:_procHeaderItem] + 1;
    if (!s.power_state || s.proc_count == 0) {
        NSMenuItem *empty = [[[NSMenuItem alloc] initWithTitle:@"  • 当前无程序占用独显 (空闲待命)"
                                                        action:nil
                                                 keyEquivalent:@""] autorelease];
        empty.enabled = NO;
        [_menu insertItem:empty atIndex:insertIdx];
        [_procItems addObject:empty];
    } else {
        for (uint32_t i = 0; i < s.proc_count; i++) {
            NSString *line = [NSString stringWithFormat:@"  • [%d] %s — %u MiB",
                              s.procs[i].pid, s.procs[i].comm, s.procs[i].vram_mib];
            NSMenuItem *pi = [[[NSMenuItem alloc] initWithTitle:line
                                                         action:nil
                                                  keyEquivalent:@""] autorelease];
            pi.enabled = NO;
            [_menu insertItem:pi atIndex:insertIdx + i];
            [_procItems addObject:pi];
        }
    }
}

- (void)menuWillOpen:(NSMenu *)menu {
    [self refreshTelemetry];
}

- (void)launchDemo:(id)sender {
    (void)sender;
    NSString *home = NSHomeDirectory();
    NSString *runner = [home stringByAppendingPathComponent:@"Library/Application Support/NVIDIA-GP108/bin/mx250-metal-run"];
    NSString *demo   = [home stringByAppendingPathComponent:@"Library/Application Support/NVIDIA-GP108/bin/mx250-metal-demo"];
    if (![[NSFileManager defaultManager] isExecutableFileAtPath:runner]) {
        runner = @"/Users/maple/projects/temp/mx250-macos/tools/mx250-metal-run";
        demo   = @"/Users/maple/projects/temp/mx250-macos/build/mx250-metal-demo";
    }
    pid_t pid = 0;
    const char *argv[] = { [runner UTF8String], [demo UTF8String], "6.0", NULL };
    posix_spawn(&pid, [runner UTF8String], NULL, NULL, (char *const *)argv, environ);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(2.0 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{ [self refreshTelemetry]; });
}

- (void)togglePower:(id)sender {
    if (!_fnSetPower) return;
    int targetOn = _lastSnap.power_state ? 0 : 1;
    _fnSetPower(targetOn);
    [self refreshTelemetry];
}

- (void)quitApp:(id)sender {
    [NSApp terminate:nil];
}

@end

__attribute__((visibility("default")))
int gp108_menubar_run(const char *smi_dylib_path, double max_seconds) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];

        MX250MenuController *ctrl =
            [[[MX250MenuController alloc] initWithSmiDylib:smi_dylib_path] autorelease];
        (void)ctrl;

        if (max_seconds > 0.0) {
            [NSTimer scheduledTimerWithTimeInterval:max_seconds
                                            repeats:NO
                                              block:^(NSTimer *t) { [NSApp stop:nil]; }];
        }
        [NSApp run];
        return 0;
    }
}
