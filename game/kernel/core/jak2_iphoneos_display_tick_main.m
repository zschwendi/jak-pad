#include "game/kernel/core/display_tick_coordinator.h"
#include "game/kernel/core/jak2_runtime.h"
#include "game/graphics/pipelines/metal/metal_jak2_host_bridge.h"
#import <QuartzCore/CADisplayLink.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#import <UIKit/UIKit.h>

static const uint64_t kMaximumProofTicks = 600;
static const uint64_t kMaximumPresentationWaitCallbacks = 120;

@class GOALJak2DisplayTickAppDelegate;

static void run_runtime_frame(double target_presentation_time, void* context);

@interface GOALJak2MetalView : UIView
@property(nonatomic, readonly) CAMetalLayer* metalLayer;
- (void)updateDrawableSize;
@end

@implementation GOALJak2MetalView

+ (Class)layerClass {
  return CAMetalLayer.class;
}

- (CAMetalLayer*)metalLayer {
  return (CAMetalLayer*)self.layer;
}

- (void)didMoveToWindow {
  [super didMoveToWindow];
  if (self.window) {
    self.layer.contentsScale = self.window.screen.nativeScale;
    [self updateDrawableSize];
  }
}

- (void)layoutSubviews {
  [super layoutSubviews];
  [self updateDrawableSize];
}

- (void)updateDrawableSize {
  const CGFloat scale = self.layer.contentsScale > 0 ? self.layer.contentsScale : 1;
  const CGSize size = self.bounds.size;
  if (size.width > 0 && size.height > 0) {
    self.metalLayer.drawableSize = CGSizeMake(size.width * scale, size.height * scale);
  }
}

@end

@interface GOALJak2DisplayTickAppDelegate : UIResponder <UIApplicationDelegate> {
 @private
  goal_display_tick_coordinator _coordinator;
  goal_jak2_runtime_metrics _metrics;
  goal_jak2_metal_host_metrics _metalMetrics;
  goal_jak2_metal_host* _metalHost;
  BOOL _applicationActive;
  BOOL _bootReady;
  BOOL _proofFinished;
  BOOL _proofPassed;
  BOOL _awaitingPresentation;
  uint64_t _presentationWaitCallbacks;
  double _lastTargetTimestamp;
  NSString* _dataPath;
  NSString* _savesPath;
  NSString* _failureMessage;
}

@property(nonatomic, strong) UIWindow* window;
@property(nonatomic, strong) GOALJak2MetalView* surfaceView;
@property(nonatomic, strong) UILabel* statusLabel;
@property(nonatomic, strong) CADisplayLink* displayLink;

@end

@interface GOALJak2DisplayTickAppDelegate ()

- (void)runRuntimeFrameAtTargetTime:(double)targetPresentationTime;
- (void)stopRuntime;

@end

@implementation GOALJak2DisplayTickAppDelegate

- (BOOL)application:(UIApplication*)application
    didFinishLaunchingWithOptions:(NSDictionary*)launchOptions {
  (void)application;
  (void)launchOptions;

  [self createWindow];
  goal_display_tick_coordinator_init(&_coordinator, run_runtime_frame, (__bridge void*)self);
  [self observeLifecycle];
  [self createDisplayLink];

  NSError* pathError = nil;
  if (![self preparePaths:&pathError]) {
    _proofFinished = YES;
    _failureMessage = pathError.localizedDescription ?: @"Could not prepare local directories.";
    [self updateTickGate];
    [self updateStatus];
    return YES;
  }

  [self updateStatus];
  [self startRuntime];
  return YES;
}

- (void)dealloc {
  [[NSNotificationCenter defaultCenter] removeObserver:self];
  [self.displayLink invalidate];
  [self stopRuntime];
}

- (void)createWindow {
  self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
  UIViewController* controller = [[UIViewController alloc] init];
  controller.view.backgroundColor = UIColor.blackColor;

  GOALJak2MetalView* surface = [[GOALJak2MetalView alloc] init];
  surface.translatesAutoresizingMaskIntoConstraints = NO;
  surface.opaque = YES;
  surface.backgroundColor = UIColor.blackColor;
  [controller.view addSubview:surface];

  UILabel* label = [[UILabel alloc] init];
  label.translatesAutoresizingMaskIntoConstraints = NO;
  label.numberOfLines = 0;
  label.textAlignment = NSTextAlignmentLeft;
  label.textColor = UIColor.whiteColor;
  label.backgroundColor = [UIColor.blackColor colorWithAlphaComponent:0.72];
  label.font = [UIFont monospacedSystemFontOfSize:15 weight:UIFontWeightRegular];
  [controller.view addSubview:label];
  [NSLayoutConstraint activateConstraints:@[
    [surface.leadingAnchor constraintEqualToAnchor:controller.view.leadingAnchor],
    [surface.trailingAnchor constraintEqualToAnchor:controller.view.trailingAnchor],
    [surface.topAnchor constraintEqualToAnchor:controller.view.topAnchor],
    [surface.bottomAnchor constraintEqualToAnchor:controller.view.bottomAnchor],
    [label.leadingAnchor constraintEqualToAnchor:controller.view.safeAreaLayoutGuide.leadingAnchor
                                        constant:24],
    [label.trailingAnchor constraintEqualToAnchor:controller.view.safeAreaLayoutGuide.trailingAnchor
                                         constant:-24],
    [label.centerYAnchor constraintEqualToAnchor:controller.view.centerYAnchor],
  ]];

  self.surfaceView = surface;
  self.statusLabel = label;
  self.window.rootViewController = controller;
  [self.window makeKeyAndVisible];
  [controller.view layoutIfNeeded];
}

- (void)observeLifecycle {
  NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
  [center addObserver:self
             selector:@selector(didBecomeActive:)
                 name:UIApplicationDidBecomeActiveNotification
               object:nil];
  [center addObserver:self
             selector:@selector(willResignActive:)
                 name:UIApplicationWillResignActiveNotification
               object:nil];
  [center addObserver:self
             selector:@selector(willEnterForeground:)
                 name:UIApplicationWillEnterForegroundNotification
               object:nil];
  [center addObserver:self
             selector:@selector(didEnterBackground:)
                 name:UIApplicationDidEnterBackgroundNotification
               object:nil];
  [center addObserver:self
             selector:@selector(willTerminate:)
                 name:UIApplicationWillTerminateNotification
               object:nil];
}

- (void)createDisplayLink {
  CADisplayLink* link = [CADisplayLink displayLinkWithTarget:self selector:@selector(displayTick:)];
  link.preferredFrameRateRange = CAFrameRateRangeMake(60, 60, 60);
  link.paused = YES;
  [link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
  self.displayLink = link;
}

- (BOOL)preparePaths:(NSError**)error {
  NSFileManager* manager = NSFileManager.defaultManager;
  NSString* override = NSProcessInfo.processInfo.environment[@"GOALPAD_JAK2_DATA_DIR"];
  if (override.length > 0) {
    _dataPath = override.stringByStandardizingPath;
  } else {
    NSURL* documents =
        [manager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
    NSURL* dataURL = [documents URLByAppendingPathComponent:@"Jak2" isDirectory:YES];
    if (![manager createDirectoryAtURL:dataURL
            withIntermediateDirectories:YES
                             attributes:nil
                                  error:error]) {
      return NO;
    }
    _dataPath = dataURL.path;
  }

  NSURL* applicationSupport = [manager URLsForDirectory:NSApplicationSupportDirectory
                                              inDomains:NSUserDomainMask]
                                  .firstObject;
  NSURL* savesURL = [applicationSupport URLByAppendingPathComponent:@"OpenGOAL/jak2/saves"
                                                        isDirectory:YES];
  if (![manager createDirectoryAtURL:savesURL
          withIntermediateDirectories:YES
                           attributes:nil
                                error:error]) {
    return NO;
  }
  _savesPath = savesURL.path;
  return YES;
}

- (void)startRuntime {
  goal_jak2_metal_host* metalHost = goal_jak2_metal_host_create();
  if (!metalHost || !goal_jak2_metal_host_set_layer(metalHost, self.surfaceView.metalLayer)) {
    goal_jak2_metal_host_destroy(metalHost);
    _proofFinished = YES;
    _failureMessage = @"The Jak II Metal host could not attach its presentation surface.";
    [self updateTickGate];
    [self updateStatus];
    return;
  }

  goal_gfx_host graphicsHost = {0};
  goal_jak2_runtime_config config = {0};
  config.data_directory = _dataPath.fileSystemRepresentation;
  config.saves_directory = _savesPath.fileSystemRepresentation;
  config.graphics = GOAL_JAK2_RUNTIME_GRAPHICS_EXTERNAL_HOST;
  goal_jak2_runtime_status result = GOAL_JAK2_RUNTIME_START_FAILED;
  if (goal_jak2_metal_host_copy_gfx_host(metalHost, &graphicsHost)) {
    config.external_gfx_host = &graphicsHost;
    result = goal_jak2_runtime_start(&config);
  }

  if (result == GOAL_JAK2_RUNTIME_OK) {
    if (goal_jak2_metal_host_flush_pending(metalHost)) {
      _metalHost = metalHost;
      _bootReady = YES;
      goal_jak2_runtime_get_metrics(&_metrics);
      goal_jak2_metal_host_get_metrics(_metalHost, &_metalMetrics);
    } else {
      const char* error = goal_jak2_metal_host_last_error(metalHost);
      _failureMessage = error && error[0] ? [NSString stringWithUTF8String:error]
                                          : @"The Jak II Metal host could not flush startup work.";
      goal_jak2_metal_host_discard_pending(metalHost);
      goal_jak2_runtime_shutdown();
      goal_jak2_metal_host_set_layer(metalHost, nil);
      goal_jak2_metal_host_destroy(metalHost);
      _proofFinished = YES;
    }
  } else {
    const char* error = goal_jak2_runtime_last_error();
    _failureMessage = error && error[0] ? [NSString stringWithUTF8String:error]
                                        : @"The Jak II Metal host or runtime did not start.";
    goal_jak2_metal_host_wait_until_idle(metalHost);
    goal_jak2_metal_host_set_layer(metalHost, nil);
    goal_jak2_metal_host_destroy(metalHost);
    _proofFinished = YES;
  }
  [self updateTickGate];
  [self updateStatus];
}

- (void)didBecomeActive:(NSNotification*)notification {
  (void)notification;
  _applicationActive = YES;
  [self updateTickGate];
  [self updateStatus];
}

- (void)willResignActive:(NSNotification*)notification {
  (void)notification;
  _applicationActive = NO;
  [self updateTickGate];
  [self updateStatus];
}

- (void)willEnterForeground:(NSNotification*)notification {
  (void)notification;
  _applicationActive = NO;
  [self updateTickGate];
}

- (void)didEnterBackground:(NSNotification*)notification {
  (void)notification;
  _applicationActive = NO;
  [self updateTickGate];
}

- (void)willTerminate:(NSNotification*)notification {
  (void)notification;
  _applicationActive = NO;
  [self updateTickGate];
  [self stopRuntime];
}

- (void)stopRuntime {
  self.displayLink.paused = YES;
  goal_display_tick_coordinator_set_foreground(&_coordinator, 0);
  if (_metalHost) {
    if (goal_jak2_runtime_is_running()) {
      goal_jak2_metal_host_flush_pending(_metalHost);
    }
    goal_jak2_metal_host_wait_until_idle(_metalHost);
  }
  if (_bootReady) {
    goal_jak2_runtime_shutdown();
    _bootReady = NO;
  }
  if (_metalHost) {
    goal_jak2_metal_host_set_layer(_metalHost, nil);
    goal_jak2_metal_host_destroy(_metalHost);
    _metalHost = NULL;
  }
}

- (void)updateTickGate {
  const BOOL shouldRun = _applicationActive && _bootReady && !_proofFinished;
  goal_display_tick_coordinator_set_foreground(&_coordinator, shouldRun ? 1 : 0);
  self.displayLink.paused = !shouldRun;
}

- (void)displayTick:(CADisplayLink*)link {
  _lastTargetTimestamp = link.targetTimestamp;
  if (_awaitingPresentation) {
    _presentationWaitCallbacks++;
    if (!goal_jak2_metal_host_get_metrics(_metalHost, &_metalMetrics)) {
      _failureMessage = @"The Jak II Metal host did not return presentation metrics.";
      _proofFinished = YES;
      _awaitingPresentation = NO;
    } else if (_metalMetrics.submissions > 0 &&
               _metalMetrics.presentations == _metalMetrics.submissions &&
               _metalMetrics.last_presented_submission_id == _metalMetrics.last_submission_id) {
      _proofFinished = YES;
      _proofPassed = YES;
      _awaitingPresentation = NO;
    } else if (_presentationWaitCallbacks >= kMaximumPresentationWaitCallbacks) {
      _failureMessage = @"The bounded proof did not observe its submitted drawable presentation.";
      _proofFinished = YES;
      _awaitingPresentation = NO;
    }
    if (_proofFinished || (_presentationWaitCallbacks % 30) == 0) {
      [self updateTickGate];
      [self updateStatus];
    }
    return;
  }
  goal_display_tick_coordinator_tick(&_coordinator, link.targetTimestamp);
}

- (void)runRuntimeFrameAtTargetTime:(double)targetPresentationTime {
  _lastTargetTimestamp = targetPresentationTime;
  goal_jak2_metal_host_set_presentation_time(_metalHost, targetPresentationTime);
  const goal_jak2_runtime_status result = goal_jak2_runtime_tick();
  if (result != GOAL_JAK2_RUNTIME_OK) {
    const char* error = goal_jak2_runtime_last_error();
    _failureMessage = error && error[0] ? [NSString stringWithUTF8String:error]
                                        : @"The Jak II runtime tick failed.";
    goal_jak2_metal_host_discard_pending(_metalHost);
    _proofFinished = YES;
    [self updateTickGate];
    [self updateStatus];
    return;
  }
  if (!goal_jak2_metal_host_flush_pending(_metalHost)) {
    const char* error = goal_jak2_metal_host_last_error(_metalHost);
    _failureMessage = error && error[0] ? [NSString stringWithUTF8String:error]
                                        : @"The Jak II Metal host could not flush its copied frame.";
    _proofFinished = YES;
    [self updateTickGate];
    [self updateStatus];
    return;
  }

  if (goal_jak2_runtime_get_metrics(&_metrics) != GOAL_JAK2_RUNTIME_OK) {
    _failureMessage = @"The Jak II runtime did not return metrics after its tick.";
    _proofFinished = YES;
    [self updateTickGate];
    [self updateStatus];
    return;
  }
  if (!goal_jak2_metal_host_get_metrics(_metalHost, &_metalMetrics)) {
    _failureMessage = @"The Jak II Metal host did not return metrics after its tick.";
    _proofFinished = YES;
    [self updateTickGate];
    [self updateStatus];
    return;
  }
  if (_metalMetrics.failed_chains > 0) {
    const char* error = goal_jak2_metal_host_last_error(_metalHost);
    _failureMessage = error && error[0] ? [NSString stringWithUTF8String:error]
                                        : @"The Jak II Metal host rejected a frame.";
    _proofFinished = YES;
    [self updateTickGate];
    [self updateStatus];
    return;
  }

  goal_display_tick_stats display = {0};
  goal_display_tick_coordinator_get_stats(&_coordinator, &display);
  const BOOL oneFramePerTick = display.accepted_ticks == _metrics.ticks;
  const BOOL validMetalDispatch =
      _metalMetrics.surface_attached == 1 && _metalMetrics.chains > 0 &&
      _metalMetrics.completed_chains == _metalMetrics.chains &&
      _metalMetrics.failed_chains == 0 && _metalMetrics.last_buckets_dispatched == 327 &&
      _metalMetrics.command_buffers_committed == _metalMetrics.chains &&
      _metalMetrics.completed_command_buffers == _metalMetrics.command_buffers_committed &&
      _metalMetrics.command_buffer_errors == 0 &&
      _metalMetrics.drawables_acquired == _metalMetrics.chains &&
      _metalMetrics.drawable_misses == 0 && _metalMetrics.draws == 0 &&
      _metalMetrics.triangles == 0 && _metalMetrics.submissions == _metalMetrics.chains &&
      _metalMetrics.presentation_drops == 0 &&
      _metalMetrics.presentation_order_mismatches == 0;
  const BOOL crossedGraphicsHost =
      _metalMetrics.sync_paths > 0 && _metalMetrics.vsyncs > 0;

  if (_metrics.title_ready && validMetalDispatch && crossedGraphicsHost && oneFramePerTick) {
    const BOOL presentationObserved =
        _metalMetrics.presentation_observation_supported == 0 ||
        (_metalMetrics.submissions > 0 &&
         _metalMetrics.presentations == _metalMetrics.submissions &&
         _metalMetrics.last_presented_submission_id == _metalMetrics.last_submission_id);
    if (presentationObserved) {
      _proofFinished = YES;
      _proofPassed = YES;
    } else {
      _awaitingPresentation = YES;
      _presentationWaitCallbacks = 0;
    }
  } else if (_metrics.ticks >= kMaximumProofTicks) {
    _proofFinished = YES;
    _failureMessage = @"The bounded proof reached 600 ticks without satisfying every gate.";
  }

  if (_proofFinished || _metrics.ticks == 1 || (_metrics.ticks % 60) == 0) {
    [self updateTickGate];
    [self updateStatus];
  }
}

- (NSString*)runtimeStateName {
  switch (_metrics.state) {
    case GOAL_JAK2_RUNTIME_STOPPED:
      return @"stopped";
    case GOAL_JAK2_RUNTIME_STARTING:
      return @"starting";
    case GOAL_JAK2_RUNTIME_RUNNING:
      return @"running";
    case GOAL_JAK2_RUNTIME_FAILED:
      return @"failed";
    case GOAL_JAK2_RUNTIME_STOPPED_BY_GAME:
      return @"stopped by game";
  }
  return @"unknown";
}

- (void)updateStatus {
  goal_display_tick_stats display = {0};
  goal_display_tick_coordinator_get_stats(&_coordinator, &display);

  NSString* result = @"BOOTING";
  if (_proofPassed) {
    result = @"PASS";
  } else if (_proofFinished) {
    result = @"FAILED";
  } else if (_bootReady) {
    result = _applicationActive ? @"RUNNING" : @"PAUSED";
  }

  NSString* titleDGO = _metrics.first_dgo_name[0]
                           ? [NSString stringWithUTF8String:_metrics.first_dgo_name]
                           : @"<none>";
  NSString* failure = _failureMessage.length > 0
                          ? [NSString stringWithFormat:@"\nError: %@", _failureMessage]
                          : @"";
  self.statusLabel.text = [NSString
      stringWithFormat:@"Jak II Metal surface display-tick proof — %@\n\n"
                        "Data: %@\nSaves: %@\nRuntime: %@\n"
                        "TITLE: %@ (%@)\n\n"
                        "Display callbacks: %llu\nAccepted ticks: %llu\nPaused ticks: %llu\n"
                        "Dispatcher frames: %llu\nLast targetTimestamp: %.6f\n\n"
                        "Metal host: %llu chain / %llu sync-path / %llu syncv\n"
                        "Completed policy chains: %llu / failures: %llu\n"
                        "GPU buffers: %llu / %llu committed / %llu errors\n"
                        "Drawables: %llu / %llu misses\n"
                        "Last dispatch: %llu buckets / %u copied bytes\n\n"
                        "Draw calls: %llu\nPresent requests: %llu\n"
                        "Observed presentations: %llu (supported: %u)\n"
                        "Last presented/submitted ID: %llu / %llu\n"
                        "Presentation drops: %llu%@",
                       result, _dataPath ?: @"<unavailable>", _savesPath ?: @"<unavailable>",
                       [self runtimeStateName], titleDGO,
                       _metrics.title_ready ? @"ready" : @"not ready",
                       (unsigned long long)display.display_ticks,
                       (unsigned long long)display.accepted_ticks,
                       (unsigned long long)display.paused_ticks, (unsigned long long)_metrics.ticks,
                       _lastTargetTimestamp, (unsigned long long)_metalMetrics.chains,
                       (unsigned long long)_metalMetrics.sync_paths,
                       (unsigned long long)_metalMetrics.vsyncs,
                       (unsigned long long)_metalMetrics.completed_chains,
                       (unsigned long long)_metalMetrics.failed_chains,
                       (unsigned long long)_metalMetrics.completed_command_buffers,
                       (unsigned long long)_metalMetrics.command_buffers_committed,
                       (unsigned long long)_metalMetrics.command_buffer_errors,
                       (unsigned long long)_metalMetrics.drawables_acquired,
                       (unsigned long long)_metalMetrics.drawable_misses,
                       (unsigned long long)_metalMetrics.last_buckets_dispatched,
                       _metalMetrics.last_copied_bytes,
                       (unsigned long long)_metalMetrics.draws,
                       (unsigned long long)_metalMetrics.submissions,
                       (unsigned long long)_metalMetrics.presentations,
                       _metalMetrics.presentation_observation_supported,
                       (unsigned long long)_metalMetrics.last_presented_submission_id,
                       (unsigned long long)_metalMetrics.last_submission_id,
                       (unsigned long long)_metalMetrics.presentation_drops,
                       failure];
}

@end

static void run_runtime_frame(double target_presentation_time, void* context) {
  GOALJak2DisplayTickAppDelegate* delegate = (__bridge GOALJak2DisplayTickAppDelegate*)context;
  [delegate runRuntimeFrameAtTargetTime:target_presentation_time];
}

int main(int argc, char* argv[]) {
  @autoreleasepool {
    return UIApplicationMain(argc, argv, nil,
                             NSStringFromClass(GOALJak2DisplayTickAppDelegate.class));
  }
}
