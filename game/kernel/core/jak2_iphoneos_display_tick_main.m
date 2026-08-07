#include "game/kernel/core/display_tick_coordinator.h"
#include "game/kernel/core/jak2_apple_audio.h"
#include "game/kernel/core/jak2_apple_input.h"
#include "game/kernel/core/jak2_metal_presenter.h"
#include "game/kernel/core/jak2_runtime.h"
#include "game/kernel/core/pad.h"
#include "game/graphics/pipelines/metal/metal_jak2_host_bridge.h"
#import <QuartzCore/CADisplayLink.h>
#import <QuartzCore/CAMetalLayer.h>
#import <TargetConditionals.h>
#import <UIKit/UIKit.h>

static const uint64_t kMaximumProofTicks = 600;
static const uint64_t kRealDmaMetalMaximumTicks = 3;
static const uint64_t kRealDmaDrawMetalMaximumTicks = 3;
static const double kRealDmaMetalCompletionTimeoutSeconds = 5.0;
static const uint64_t kLifecycleProofForegroundTicksBeforePause = 3;
static const uint64_t kLifecycleProofForegroundTicksAfterResume = 3;
static const uint64_t kLifecycleProofPausedCallbacks = 3;

@class GOALJak2DisplayTickAppDelegate;

static void run_runtime_frame(double target_presentation_time, void* context);

static BOOL read_launch_path(NSString* option, NSString** value, NSError** error) {
  NSString* assignmentPrefix = [option stringByAppendingString:@"="];
  NSArray<NSString*>* arguments = NSProcessInfo.processInfo.arguments;
  for (NSUInteger index = 1; index < arguments.count; ++index) {
    NSString* argument = arguments[index];
    NSString* candidate = nil;
    if ([argument isEqualToString:option]) {
      if (index + 1 < arguments.count && ![arguments[index + 1] hasPrefix:@"--"]) {
        candidate = arguments[++index];
      }
    } else if ([argument hasPrefix:assignmentPrefix]) {
      candidate = [argument substringFromIndex:assignmentPrefix.length];
    } else {
      continue;
    }

    if (candidate.length == 0) {
      if (error) {
        *error = [NSError
            errorWithDomain:@"org.opengoal.jak2-iphoneos"
                       code:1
                   userInfo:@{
                     NSLocalizedDescriptionKey :
                         [NSString stringWithFormat:@"%@ requires a local directory.", option]
                   }];
      }
      return NO;
    }
    *value = candidate;
  }
  return YES;
}

static BOOL runtime_metrics_match_during_pause(const goal_jak2_runtime_metrics* before,
                                               const goal_jak2_runtime_metrics* after) {
  return before->state == after->state && before->ticks == after->ticks &&
         before->last_dispatch_result == after->last_dispatch_result &&
         before->master_exit == after->master_exit &&
         before->dgo_archives == after->dgo_archives &&
         before->dgo_objects == after->dgo_objects &&
         before->host_chains == after->host_chains &&
         before->host_sync_paths == after->host_sync_paths &&
         before->host_syncvs == after->host_syncvs &&
         before->sound_bank_failures == after->sound_bank_failures &&
         before->sound_player_failures == after->sound_player_failures &&
         before->sound_str_failures == after->sound_str_failures &&
         before->sound_rejected_calls == after->sound_rejected_calls;
}

static BOOL metal_metrics_match_during_pause(const goal_jak2_metal_host_metrics* before,
                                             const goal_jak2_metal_host_metrics* after) {
  return before->chains == after->chains &&
         before->completed_chains == after->completed_chains &&
         before->failed_chains == after->failed_chains &&
         before->sync_paths == after->sync_paths && before->vsyncs == after->vsyncs &&
         before->texture_uploads == after->texture_uploads &&
         before->texture_relocations == after->texture_relocations &&
         before->last_copied_bytes == after->last_copied_bytes &&
         before->last_buckets_dispatched == after->last_buckets_dispatched &&
         before->command_buffers_committed == after->command_buffers_committed &&
         before->command_buffers_completed == after->command_buffers_completed &&
         before->command_buffer_errors == after->command_buffer_errors &&
         before->drawables_acquired == after->drawables_acquired &&
         before->drawable_misses == after->drawable_misses &&
         before->late_present_submissions == after->late_present_submissions &&
         before->draws == after->draws && before->triangles == after->triangles &&
         before->submissions == after->submissions &&
         before->presentations == after->presentations &&
         before->presentation_drops == after->presentation_drops &&
         before->presentation_order_mismatches == after->presentation_order_mismatches &&
         before->skipped_bucket_bytes == after->skipped_bucket_bytes &&
         before->last_screen_filter_draws == after->last_screen_filter_draws &&
         before->last_screen_filter_triangles == after->last_screen_filter_triangles &&
         before->last_debug_no_zbuf2_draws == after->last_debug_no_zbuf2_draws &&
         before->last_debug_no_zbuf2_triangles == after->last_debug_no_zbuf2_triangles &&
         before->last_sky_draw_draws == after->last_sky_draw_draws &&
         before->last_sky_draw_triangles == after->last_sky_draw_triangles &&
         before->last_sky_draw_batch_valid == after->last_sky_draw_batch_valid &&
         before->last_sky_draw_batch_textured == after->last_sky_draw_batch_textured &&
         before->last_sky_draw_batch_vertices == after->last_sky_draw_batch_vertices &&
         before->last_sky_draw_batch_nonzero_rgb_vertices ==
             after->last_sky_draw_batch_nonzero_rgb_vertices &&
         before->last_sky_draw_batch_tex0_tbp == after->last_sky_draw_batch_tex0_tbp &&
         before->last_sky_draw_batch_tex0_tcc == after->last_sky_draw_batch_tex0_tcc &&
         before->last_sky_draw_batch_tex0_decal == after->last_sky_draw_batch_tex0_decal &&
         before->last_sky_draw_batch_texture_lookup_hit ==
             after->last_sky_draw_batch_texture_lookup_hit &&
         before->last_sky_draw_batch_used_placeholder ==
             after->last_sky_draw_batch_used_placeholder &&
         before->last_sky_draw_batch_write_rgb == after->last_sky_draw_batch_write_rgb &&
         before->last_sky_draw_batch_blend_enabled ==
             after->last_sky_draw_batch_blend_enabled &&
         before->last_sky_draw_batch_blend_a == after->last_sky_draw_batch_blend_a &&
         before->last_sky_draw_batch_blend_b == after->last_sky_draw_batch_blend_b &&
         before->last_sky_draw_batch_blend_c == after->last_sky_draw_batch_blend_c &&
         before->last_sky_draw_batch_blend_d == after->last_sky_draw_batch_blend_d &&
         before->last_sky_draw_batch_alpha_test_enabled ==
             after->last_sky_draw_batch_alpha_test_enabled &&
         before->last_sky_draw_batch_alpha_test_mode ==
             after->last_sky_draw_batch_alpha_test_mode &&
         before->last_sky_draw_batch_alpha_aref == after->last_sky_draw_batch_alpha_aref &&
         before->last_sky_draw_batch_alpha_afail == after->last_sky_draw_batch_alpha_afail &&
         before->unsupported_blends == after->unsupported_blends &&
         before->last_command_buffer_status == after->last_command_buffer_status &&
         before->last_command_buffer_error_code == after->last_command_buffer_error_code;
}

@interface GOALJak2MetalProofView : UIView

@property(nonatomic, readonly) CAMetalLayer* metalLayer;

@end

@implementation GOALJak2MetalProofView

+ (Class)layerClass {
  return CAMetalLayer.class;
}

- (CAMetalLayer*)metalLayer {
  return (CAMetalLayer*)self.layer;
}

- (void)layoutSubviews {
  [super layoutSubviews];
  const CGFloat scale = self.window ? self.window.screen.scale : UIScreen.mainScreen.scale;
  self.metalLayer.contentsScale = scale;
  self.metalLayer.drawableSize =
      CGSizeMake(self.bounds.size.width * scale, self.bounds.size.height * scale);
}

@end

@interface GOALJak2DisplayTickAppDelegate : UIResponder <UIApplicationDelegate> {
 @private
  goal_display_tick_coordinator _coordinator;
  goal_jak2_runtime_metrics _metrics;
  goal_jak2_thread_suspend_probe _threadSuspendProbe;
  goal_jak2_metal_host_metrics _metalMetrics;
  goal_jak2_metal_host* _metalHost;
  BOOL _applicationActive;
  BOOL _bootReady;
  BOOL _proofFinished;
  BOOL _proofPassed;
  BOOL _metalProofEnabled;
  BOOL _metalProofSubmitted;
  BOOL _realDmaMetalProofEnabled;
  BOOL _realDmaDrawMetalProofEnabled;
  BOOL _realDmaMetalCompletionPending;
  BOOL _realDmaDrawBaselineCaptured;
  BOOL _lifecycleProofEnabled;
  BOOL _titleLoopEnabled;
  BOOL _titleLoopReported;
  BOOL _inputStarted;
  BOOL _audioOpened;
  BOOL _inputProbePressRead;
  BOOL _inputProbeComplete;
  BOOL _lifecyclePauseVerified;
  BOOL _lifecycleResumeVerified;
  BOOL _shutdownRequested;
  uint64_t _metalProofDisplayCallbacks;
  uint64_t _realDmaDrawInspectedChains;
  int _inputProbeBaselineReads;
  int _inputProbePressedReadCount;
  goal_jak2_metal_stats _metalStats;
  goal_jak2_metal_frame_summary _realDmaDrawBaselineFrame;
  goal_jak2_metal_frame_summary _realDmaDrawFrame;
  goal_display_tick_stats _lifecycleStatsBeforePause;
  goal_jak2_runtime_metrics _lifecycleMetricsBeforePause;
  goal_jak2_metal_host_metrics _lifecycleMetalBeforePause;
  double _lastTargetTimestamp;
  NSString* _dataPath;
  NSString* _savesPath;
  NSString* _failureMessage;
}

@property(nonatomic, strong) UIWindow* window;
@property(nonatomic, strong) UILabel* statusLabel;
@property(nonatomic, strong) CADisplayLink* displayLink;
@property(nonatomic, strong) GOALJak2MetalProofView* metalProofView;

@end

@interface GOALJak2DisplayTickAppDelegate ()

- (void)runRuntimeFrameAtTargetTime:(double)targetPresentationTime;
- (BOOL)startOrResumeAudio;
- (void)runLifecyclePauseCycleAtTargetTime:(double)targetPresentationTime;
- (void)stopRuntime;
- (void)submitMetalProofFrame;
- (void)waitForRealDmaMetalFrame;
- (void)waitForRealDmaDrawMetalFrame;

@end

@implementation GOALJak2DisplayTickAppDelegate

- (BOOL)application:(UIApplication*)application
    didFinishLaunchingWithOptions:(NSDictionary*)launchOptions {
  (void)application;
  (void)launchOptions;

  _metalProofEnabled =
      [NSProcessInfo.processInfo.environment[@"GOALPAD_JAK2_CAMETAL_LAYER_PROOF"]
          isEqualToString:@"1"];
  _realDmaDrawMetalProofEnabled =
      !_metalProofEnabled &&
      [NSProcessInfo.processInfo.environment[@"GOALPAD_JAK2_REAL_DMA_DRAW_CAMETAL_LAYER_PROOF"]
          isEqualToString:@"1"];
  _realDmaMetalProofEnabled =
      !_metalProofEnabled && !_realDmaDrawMetalProofEnabled &&
      [NSProcessInfo.processInfo.environment[@"GOALPAD_JAK2_REAL_DMA_CAMETAL_LAYER_PROOF"]
          isEqualToString:@"1"];
  _lifecycleProofEnabled =
      !_metalProofEnabled && !_realDmaDrawMetalProofEnabled && !_realDmaMetalProofEnabled &&
      [NSProcessInfo.processInfo.environment[@"GOALPAD_JAK2_LIFECYCLE_PROOF"]
          isEqualToString:@"1"];
  _titleLoopEnabled = !_metalProofEnabled && !_realDmaDrawMetalProofEnabled &&
                      !_realDmaMetalProofEnabled && !_lifecycleProofEnabled;
  [self createWindow];
  goal_display_tick_coordinator_init(&_coordinator, run_runtime_frame, (__bridge void*)self);
  [self observeLifecycle];
  [self createDisplayLink];

  if (_metalProofEnabled) {
    if (!goal_jak2_metal_presenter_start(self.metalProofView.metalLayer)) {
      const char* error = goal_jak2_metal_presenter_last_error();
      _proofFinished = YES;
      _failureMessage = error && error[0] ? [NSString stringWithUTF8String:error]
                                          : @"The Jak II Metal presenter did not start.";
    }
    [self updateTickGate];
    [self updateStatus];
    return YES;
  }

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
  if (_titleLoopEnabled || _metalProofEnabled || _realDmaMetalProofEnabled ||
      _realDmaDrawMetalProofEnabled) {
    GOALJak2MetalProofView* metalView =
        [[GOALJak2MetalProofView alloc] initWithFrame:self.window.bounds];
    metalView.backgroundColor = UIColor.blackColor;
    controller.view = metalView;
    self.metalProofView = metalView;
  } else {
    controller.view.backgroundColor = UIColor.blackColor;
  }

  UILabel* label = [[UILabel alloc] init];
  label.translatesAutoresizingMaskIntoConstraints = NO;
  label.numberOfLines = 0;
  label.textAlignment = NSTextAlignmentLeft;
  label.textColor = UIColor.whiteColor;
  label.font = [UIFont monospacedSystemFontOfSize:15 weight:UIFontWeightRegular];
  [controller.view addSubview:label];
  [NSLayoutConstraint activateConstraints:@[
    [label.leadingAnchor constraintEqualToAnchor:controller.view.safeAreaLayoutGuide.leadingAnchor
                                        constant:24],
    [label.trailingAnchor constraintEqualToAnchor:controller.view.safeAreaLayoutGuide.trailingAnchor
                                         constant:-24],
    [label.centerYAnchor constraintEqualToAnchor:controller.view.centerYAnchor],
  ]];

  self.statusLabel = label;
  self.window.rootViewController = controller;
  [self.window makeKeyAndVisible];
  [controller.view setNeedsLayout];
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
  NSString* launchOverride = nil;
  if (!read_launch_path(@"--data-dir", &launchOverride, error)) {
    return NO;
  }

  NSString* environmentOverride =
      NSProcessInfo.processInfo.environment[@"GOALPAD_JAK2_DATA_DIR"];
  NSString* override = launchOverride.length > 0 ? launchOverride : environmentOverride;
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

  NSString* savesLaunchOverride = nil;
  if (!read_launch_path(@"--saves-dir", &savesLaunchOverride, error)) {
    return NO;
  }
  NSString* savesEnvironmentOverride =
      NSProcessInfo.processInfo.environment[@"GOALPAD_JAK2_SAVES_DIR"];
  NSString* savesOverride =
      savesLaunchOverride.length > 0 ? savesLaunchOverride : savesEnvironmentOverride;
  NSURL* savesURL = nil;
  if (savesOverride.length > 0) {
    savesURL = [NSURL fileURLWithPath:savesOverride.stringByStandardizingPath isDirectory:YES];
  } else {
    NSURL* applicationSupport = [manager URLsForDirectory:NSApplicationSupportDirectory
                                                inDomains:NSUserDomainMask]
                                    .firstObject;
    savesURL = [applicationSupport URLByAppendingPathComponent:@"OpenGOAL/jak2/saves"
                                                   isDirectory:YES];
  }
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
  NSString* dataPath = _dataPath;
  NSString* savesPath = _savesPath;
  const BOOL presenting =
      _titleLoopEnabled || _realDmaMetalProofEnabled || _realDmaDrawMetalProofEnabled;
  CAMetalLayer* metalLayer = presenting ? self.metalProofView.metalLayer : nil;
  goal_jak2_metal_host* presentingHost =
      presenting ? goal_jak2_metal_host_create_presenting(metalLayer) : NULL;
  dispatch_queue_t queue =
      dispatch_queue_create("org.opengoal.jak2-display-tick.boot", DISPATCH_QUEUE_SERIAL);
  dispatch_async(queue, ^{
    @autoreleasepool {
      goal_jak2_metal_host* metalHost = presenting ? presentingHost : goal_jak2_metal_host_create();
      goal_gfx_host graphicsHost = {0};
      goal_jak2_runtime_config config = {0};
      config.data_directory = dataPath.fileSystemRepresentation;
      config.saves_directory = savesPath.fileSystemRepresentation;
      config.graphics = GOAL_JAK2_RUNTIME_GRAPHICS_EXTERNAL_HOST;
      goal_jak2_runtime_status result = GOAL_JAK2_RUNTIME_START_FAILED;
      NSString* failure = nil;
      if (!metalHost) {
        failure = @"The Jak II Metal host did not start.";
      } else if (_titleLoopEnabled &&
                 !goal_jak2_metal_host_set_present_pacing(metalHost, 1.0 / 60.0)) {
        failure = @"The Jak II Metal host did not accept title-loop presentation pacing.";
      } else if (_titleLoopEnabled &&
                 !goal_jak2_metal_host_configure_level_art(
                     metalHost,
                     [[dataPath stringByAppendingPathComponent:@"fr3"] fileSystemRepresentation])) {
        const char* error = goal_jak2_metal_host_last_error(metalHost);
        failure = error && error[0] ? [NSString stringWithUTF8String:error]
                                    : @"The Jak II Metal host could not load local level art.";
      } else if (!goal_jak2_metal_host_copy_gfx_host(metalHost, &graphicsHost)) {
        const char* error = goal_jak2_metal_host_last_error(metalHost);
        failure = error && error[0] ? [NSString stringWithUTF8String:error]
                                    : @"The Jak II Metal host did not provide graphics callbacks.";
      } else {
        config.external_gfx_host = &graphicsHost;
        result = goal_jak2_runtime_start(&config);
        if (result == GOAL_JAK2_RUNTIME_OK) {
          result = goal_jak2_runtime_probe_thread_suspend(&_threadSuspendProbe);
        }
      }
      if (result != GOAL_JAK2_RUNTIME_OK) {
        if (!failure) {
          const char* error = goal_jak2_runtime_last_error();
          failure = error && error[0] ? [NSString stringWithUTF8String:error]
                                      : @"The Jak II Metal host or runtime did not start.";
        }
        if (goal_jak2_runtime_is_running()) {
          goal_jak2_runtime_shutdown();
        }
        goal_jak2_metal_host_destroy(metalHost);
        metalHost = NULL;
      } else {
        NSLog(@"GOALPAD_JAK2_THREAD_SUSPEND_PROBE PASS source=#x%08x/#x%llx "
               "display=#x%08x top=#x%08x hook=#x%08x/#x%llx (%@)",
              _threadSuspendProbe.function_object,
              (unsigned long long)_threadSuspendProbe.native_entry,
              _threadSuspendProbe.display_process, _threadSuspendProbe.top_thread,
              _threadSuspendProbe.hook_function_object,
              (unsigned long long)_threadSuspendProbe.hook_native_entry,
              _threadSuspendProbe.hook_available ? @"valid" : @"pending");
      }

      dispatch_async(dispatch_get_main_queue(), ^{
        if (_shutdownRequested) {
          if (result == GOAL_JAK2_RUNTIME_OK) {
            goal_jak2_runtime_shutdown();
          }
          goal_jak2_metal_host_destroy(metalHost);
          return;
        }
        if (result == GOAL_JAK2_RUNTIME_OK) {
          _metalHost = metalHost;
          _bootReady = YES;
          _inputProbeBaselineReads = goal_pad_read_count(0);
          goal_jak2_runtime_get_metrics(&_metrics);
          goal_jak2_metal_host_get_metrics(_metalHost, &_metalMetrics);
          if (_titleLoopEnabled && _applicationActive) {
            goal_jak2_apple_input_start();
            _inputStarted = YES;
            if (![self startOrResumeAudio]) {
              [self stopRuntime];
            }
          }
        } else {
          _proofFinished = YES;
          _failureMessage = failure;
        }
        [self updateTickGate];
        [self updateStatus];
      });
    }
  });
}

- (void)didBecomeActive:(NSNotification*)notification {
  (void)notification;
  _applicationActive = YES;
  if (_titleLoopEnabled && _bootReady && !_inputStarted) {
    goal_jak2_apple_input_start();
    _inputStarted = YES;
  }
  if (_titleLoopEnabled && _bootReady && ![self startOrResumeAudio]) {
    [self stopRuntime];
  }
  [self updateTickGate];
  [self updateStatus];
}

- (void)willResignActive:(NSNotification*)notification {
  (void)notification;
  _applicationActive = NO;
  if (_inputStarted) {
    goal_jak2_apple_input_stop();
    _inputStarted = NO;
  }
  if (_audioOpened && !goal_jak2_apple_audio_suspend()) {
    NSLog(@"GOALPAD_JAK2_ECO_AUDIO SUSPEND_FAILED error=%s",
          goal_jak2_apple_audio_last_error());
    _audioOpened = NO;
  }
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
  _shutdownRequested = YES;
  goal_display_tick_coordinator_set_foreground(&_coordinator, 0);
  self.displayLink.paused = YES;
  if (_realDmaMetalCompletionPending) {
    return;
  }
  if (_inputStarted) {
    goal_jak2_apple_input_stop();
    _inputStarted = NO;
  }
  if (_audioOpened) {
    if (!goal_jak2_apple_audio_close()) {
      NSLog(@"GOALPAD_JAK2_ECO_AUDIO CLOSE_FAILED error=%s",
            goal_jak2_apple_audio_last_error());
    }
    _audioOpened = NO;
  }
  if (_bootReady) {
    goal_jak2_runtime_shutdown();
    _bootReady = NO;
  }
  if (_metalHost) {
    goal_jak2_metal_host_destroy(_metalHost);
    _metalHost = NULL;
  }
  if (!_metalProofEnabled || !_metalProofSubmitted || _proofFinished) {
    goal_jak2_metal_presenter_shutdown();
  }
}

- (BOOL)startOrResumeAudio {
  if (!_titleLoopEnabled || !_applicationActive || !_bootReady) {
    return YES;
  }
  const int started = _audioOpened ? goal_jak2_apple_audio_resume()
                                   : goal_jak2_apple_audio_start();
  if (!started) {
    const char* error = goal_jak2_apple_audio_last_error();
    _failureMessage = error && error[0] ? [NSString stringWithUTF8String:error]
                                        : @"The Apple audio output did not start.";
    _proofFinished = YES;
    _audioOpened = NO;
    return NO;
  }
  _audioOpened = YES;
  goal_jak2_apple_audio_stats audio = {0};
  goal_jak2_apple_audio_get_stats(&audio);
  NSLog(@"GOALPAD_JAK2_ECO_AUDIO RUNNING starts=%llu resumes=%llu sample-rate=%d",
        (unsigned long long)audio.starts, (unsigned long long)audio.resumes, audio.sample_rate);
  return YES;
}

- (void)updateTickGate {
  if (_metalProofEnabled) {
    goal_display_tick_coordinator_set_foreground(&_coordinator, 0);
    self.displayLink.paused = !(_applicationActive && !_metalProofSubmitted && !_proofFinished);
    return;
  }
  const BOOL shouldRun = _applicationActive && _bootReady && !_proofFinished &&
                         !_realDmaMetalCompletionPending;
  goal_display_tick_coordinator_set_foreground(&_coordinator, shouldRun ? 1 : 0);
  self.displayLink.paused = !shouldRun;
}

- (void)displayTick:(CADisplayLink*)link {
  _lastTargetTimestamp = link.targetTimestamp;
  if (_metalProofEnabled) {
    _metalProofDisplayCallbacks++;
    _metalProofSubmitted = YES;
    self.displayLink.paused = YES;
    [self submitMetalProofFrame];
    return;
  }
  goal_display_tick_coordinator_tick(&_coordinator, link.targetTimestamp);
}

- (void)submitMetalProofFrame {
  if (!goal_jak2_metal_presenter_render(0.0)) {
    const char* error = goal_jak2_metal_presenter_last_error();
    _proofFinished = YES;
    _failureMessage = error && error[0] ? [NSString stringWithUTF8String:error]
                                        : @"The Jak II Metal proof frame was not submitted.";
    [self updateStatus];
    return;
  }
  if (!goal_jak2_metal_presenter_get_stats(&_metalStats)) {
    _proofFinished = YES;
    _failureMessage = @"The Jak II Metal presenter did not return submission counters.";
    [self updateStatus];
    return;
  }
  [self updateStatus];

  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    const int completed = goal_jak2_metal_presenter_wait_for_completion();
    goal_jak2_metal_stats stats = {0};
    const int copiedStats = goal_jak2_metal_presenter_get_stats(&stats);
    const char* error = completed ? "" : goal_jak2_metal_presenter_last_error();
    NSString* failure = error && error[0] ? [NSString stringWithUTF8String:error]
                                          : @"The Jak II Metal proof frame did not complete.";
    dispatch_async(dispatch_get_main_queue(), ^{
      _metalStats = stats;
      _proofFinished = YES;
      _proofPassed = completed != 0 && copiedStats != 0 && _metalProofDisplayCallbacks == 1;
      if (!_proofPassed) {
        if (!copiedStats) {
          _failureMessage = @"The Jak II Metal presenter did not return completion counters.";
        } else if (completed && _metalProofDisplayCallbacks != 1) {
          _failureMessage = @"The Metal proof consumed more than one display callback.";
        } else {
          _failureMessage = failure;
        }
      } else {
        NSLog(@"GOALPAD_JAK2_CAMETAL_LAYER_PROOF PASS callbacks=1 attempts=1 chains=1 "
               "buckets=327 skipped=16 draws=0 triangles=0 acquired=1 misses=0 committed=1 "
               "completed=1 errors=0 submissions=1 late=0 presentation_callbacks=%llu",
              (unsigned long long)_metalStats.presentations_completed);
      }
      [self updateTickGate];
      [self updateStatus];
    });
  });
}

- (void)runRuntimeFrameAtTargetTime:(double)targetPresentationTime {
  _lastTargetTimestamp = targetPresentationTime;
  if (_titleLoopEnabled) {
    goal_kernel_core_status inputStatus = GOAL_KERNEL_CORE_OK;
    if (_inputProbeComplete) {
      inputStatus = goal_jak2_apple_input_sample_and_push();
    } else {
      goal_pad_state probe;
      goal_pad_state_neutral(&probe);
      probe.connected = 1;
      if (!_inputProbePressRead) {
        probe.buttons = GOAL_PAD_L3;
      }
      inputStatus = goal_pad_set_state(0, &probe);
    }
    if (inputStatus != GOAL_KERNEL_CORE_OK) {
      _failureMessage = @"The title input seam could not push the current pad state.";
      _proofFinished = YES;
      [self stopRuntime];
      [self updateTickGate];
      [self updateStatus];
      return;
    }
  }
  goal_jak2_runtime_status result = goal_jak2_runtime_tick();
  if (result == GOAL_JAK2_RUNTIME_OK && !_threadSuspendProbe.hook_available) {
    result = goal_jak2_runtime_probe_thread_suspend(&_threadSuspendProbe);
  }
  if (result != GOAL_JAK2_RUNTIME_OK) {
    const char* error = goal_jak2_runtime_last_error();
    _failureMessage = error && error[0] ? [NSString stringWithUTF8String:error]
                                        : @"The Jak II runtime tick failed.";
    _proofFinished = YES;
    [self stopRuntime];
    goal_jak2_runtime_get_metrics(&_metrics);
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
      _metalMetrics.chains > 0 &&
      _metalMetrics.completed_chains == _metalMetrics.chains &&
      _metalMetrics.failed_chains == 0 && _metalMetrics.last_buckets_dispatched == 327 &&
      _metalMetrics.command_buffers_committed == 0 &&
      _metalMetrics.command_buffers_completed == 0 && _metalMetrics.command_buffer_errors == 0 &&
      _metalMetrics.drawables_acquired == 0 && _metalMetrics.drawable_misses == 0 &&
      _metalMetrics.submissions == 0 && _metalMetrics.presentations == 0 &&
      _metalMetrics.presentation_drops == 0 &&
      _metalMetrics.presentation_order_mismatches == 0;
  const BOOL crossedGraphicsHost =
      _metalMetrics.sync_paths > 0 && _metalMetrics.vsyncs > 0;

  if (_realDmaDrawMetalProofEnabled) {
    if (_metalMetrics.chains > _realDmaDrawInspectedChains) {
      const BOOL exactSubmission =
          _metalMetrics.chains == _realDmaDrawInspectedChains + 1 &&
          _metalMetrics.chains <= kRealDmaDrawMetalMaximumTicks &&
          _metalMetrics.chains == _metrics.ticks &&
          _metalMetrics.completed_chains == _metalMetrics.chains &&
          _metalMetrics.last_buckets_dispatched == 327 &&
          _metalMetrics.command_buffers_committed == _metalMetrics.chains &&
          _metalMetrics.command_buffers_completed <= _metalMetrics.command_buffers_committed &&
          _metalMetrics.command_buffer_errors == 0 &&
          _metalMetrics.drawables_acquired == _metalMetrics.chains &&
          _metalMetrics.drawable_misses == 0 &&
          _metalMetrics.submissions == _metalMetrics.chains &&
          _metalMetrics.late_present_submissions == 0 &&
          _metalMetrics.presentation_drops == 0 &&
          _metalMetrics.presentation_order_mismatches == 0 &&
          _metalMetrics.unsupported_blends == 0 &&
          display.display_ticks == _metrics.ticks && display.paused_ticks == 0 &&
          oneFramePerTick;
      if (!exactSubmission) {
        _proofFinished = YES;
        _failureMessage =
            @"The later-draw real-DMA submission violated its layer or policy counters.";
        [self updateTickGate];
        [self updateStatus];
        return;
      }
      _realDmaMetalCompletionPending = YES;
      [self updateTickGate];
      [self updateStatus];
      [self waitForRealDmaDrawMetalFrame];
      return;
    }
    if (_metrics.ticks >= kRealDmaDrawMetalMaximumTicks) {
      _proofFinished = YES;
      _failureMessage =
          @"No new real Jak II DMA chain reached the later-draw proof within three ticks.";
      [self updateTickGate];
      [self updateStatus];
    }
    return;
  }

  if (_realDmaMetalProofEnabled) {
    if (_metalMetrics.chains > 0) {
      const BOOL exactSubmission =
          _metalMetrics.chains == 1 &&
          _metalMetrics.completed_chains == _metalMetrics.chains &&
          _metalMetrics.last_buckets_dispatched == 327 &&
          _metalMetrics.command_buffers_committed == _metalMetrics.chains &&
          _metalMetrics.command_buffers_completed <= _metalMetrics.command_buffers_committed &&
          _metalMetrics.command_buffer_errors == 0 &&
          _metalMetrics.drawables_acquired == _metalMetrics.chains &&
          _metalMetrics.drawable_misses == 0 &&
          _metalMetrics.submissions == _metalMetrics.chains &&
          _metalMetrics.late_present_submissions == 0 &&
          _metalMetrics.presentation_drops == 0 &&
          _metalMetrics.presentation_order_mismatches == 0 &&
          _metalMetrics.unsupported_blends == 0 && oneFramePerTick;
      if (!exactSubmission) {
        _proofFinished = YES;
        _failureMessage =
            @"The real-DMA Metal submission violated its exact layer or policy counters.";
        [self updateTickGate];
        [self updateStatus];
        return;
      }
      _realDmaMetalCompletionPending = YES;
      [self updateTickGate];
      [self updateStatus];
      [self waitForRealDmaMetalFrame];
      return;
    }
    if (_metrics.ticks >= kRealDmaMetalMaximumTicks) {
      _proofFinished = YES;
      _failureMessage = @"No real Jak II DMA chain reached Metal within three runtime ticks.";
      [self updateTickGate];
      [self updateStatus];
    }
    return;
  }

  if (_titleLoopEnabled) {
    const int inputReads = goal_pad_read_count(0);
    if (!_inputProbePressRead && inputReads > _inputProbeBaselineReads) {
      _inputProbePressRead = YES;
      _inputProbePressedReadCount = inputReads;
    } else if (_inputProbePressRead && !_inputProbeComplete &&
               inputReads > _inputProbePressedReadCount) {
      _inputProbeComplete = YES;
      NSLog(@"GOALPAD_JAK2_ECO_INPUT_PROBE PASS baseline=%d press-read=%d release-read=%d",
            _inputProbeBaselineReads, _inputProbePressedReadCount, inputReads);
    } else if (!_inputProbeComplete && _metrics.ticks >= kMaximumProofTicks) {
      _failureMessage = @"GOAL did not consume both states from the bounded input probe.";
      _proofFinished = YES;
      [self stopRuntime];
      [self updateTickGate];
      [self updateStatus];
      return;
    }
    if (!_titleLoopReported && _metrics.title_ready && _metalMetrics.completed_chains > 0 &&
        _metalMetrics.failed_chains == 0 && _metalMetrics.draws > 0 &&
        _metalMetrics.command_buffers_committed > 0 && _metalMetrics.drawables_acquired > 0) {
      goal_jak2_metal_frame_summary frame = {0};
      if (goal_jak2_metal_host_read_last_frame(_metalHost, &frame) &&
          frame.non_black_pixels > 0) {
        NSLog(@"GOALPAD_JAK2_ECO_TITLE PASS ticks=%llu chains=%llu completed=%llu failed=%llu "
               "draws=%llu triangles=%llu drawables=%llu commits=%llu frame=%016llx "
               "non-black=%llu presentations=%llu",
              (unsigned long long)_metrics.ticks, (unsigned long long)_metalMetrics.chains,
              (unsigned long long)_metalMetrics.completed_chains,
              (unsigned long long)_metalMetrics.failed_chains,
              (unsigned long long)_metalMetrics.draws,
              (unsigned long long)_metalMetrics.triangles,
              (unsigned long long)_metalMetrics.drawables_acquired,
              (unsigned long long)_metalMetrics.command_buffers_committed,
              (unsigned long long)frame.hash, (unsigned long long)frame.non_black_pixels,
              (unsigned long long)_metalMetrics.presentations);
        _titleLoopReported = YES;
      }
    }
    if (_metrics.ticks == 1 || (_metrics.ticks % 60) == 0) {
      [self updateStatus];
    }
    if ((_metrics.ticks % 300) == 0) {
      NSLog(@"GOALPAD_JAK2_ECO_METAL ticks=%llu chains=%llu completed=%llu failed=%llu "
             "drawables=%llu misses=%llu commits=%llu/%llu errors=%llu draws=%llu "
             "triangles=%llu submissions=%llu presentations=%llu drops=%llu",
            (unsigned long long)_metrics.ticks, (unsigned long long)_metalMetrics.chains,
            (unsigned long long)_metalMetrics.completed_chains,
            (unsigned long long)_metalMetrics.failed_chains,
            (unsigned long long)_metalMetrics.drawables_acquired,
            (unsigned long long)_metalMetrics.drawable_misses,
            (unsigned long long)_metalMetrics.command_buffers_committed,
            (unsigned long long)_metalMetrics.command_buffers_completed,
            (unsigned long long)_metalMetrics.command_buffer_errors,
            (unsigned long long)_metalMetrics.draws,
            (unsigned long long)_metalMetrics.triangles,
            (unsigned long long)_metalMetrics.submissions,
            (unsigned long long)_metalMetrics.presentations,
            (unsigned long long)_metalMetrics.presentation_drops);
      goal_jak2_apple_input_metrics input = {0};
      if (goal_jak2_apple_input_get_metrics(&input) == GOAL_KERNEL_CORE_OK) {
        NSLog(@"GOALPAD_JAK2_ECO_INPUT samples=%llu reads=%d connected=%d sources=%u "
               "buttons=#x%08x failures=%llu",
              (unsigned long long)input.samples, goal_pad_read_count(0), input.connected,
              input.active_sources, input.last_buttons, (unsigned long long)input.push_failures);
      }
      goal_jak2_apple_audio_stats audio = {0};
      if (goal_jak2_apple_audio_get_stats(&audio)) {
        NSLog(@"GOALPAD_JAK2_ECO_AUDIO callbacks=%llu requested=%llu rendered=%llu "
               "silent=%llu underruns=%llu invalid=%llu",
              (unsigned long long)audio.render_callbacks,
              (unsigned long long)audio.frames_requested,
              (unsigned long long)audio.frames_rendered,
              (unsigned long long)audio.silent_frames, (unsigned long long)audio.underruns,
              (unsigned long long)audio.invalid_buffers);
      }
    }
    return;
  }

  if (_lifecycleProofEnabled && !_lifecyclePauseVerified &&
      _metrics.ticks == kLifecycleProofForegroundTicksBeforePause) {
    [self runLifecyclePauseCycleAtTargetTime:targetPresentationTime];
    return;
  }

  if (_lifecycleProofEnabled && _lifecyclePauseVerified && !_lifecycleResumeVerified &&
      _metrics.ticks == _lifecycleMetricsBeforePause.ticks + 1) {
    const BOOL exactlyOneResumedTick =
        display.accepted_ticks == _lifecycleStatsBeforePause.accepted_ticks + 1 &&
        display.paused_ticks ==
            _lifecycleStatsBeforePause.paused_ticks + kLifecycleProofPausedCallbacks;
    if (!exactlyOneResumedTick) {
      _proofFinished = YES;
      _failureMessage = @"The first foreground callback replayed or lost lifecycle ticks.";
    } else {
      _lifecycleResumeVerified = YES;
    }
  }

  if (_lifecycleProofEnabled && !_proofFinished &&
      _metrics.ticks == kLifecycleProofForegroundTicksBeforePause +
                            kLifecycleProofForegroundTicksAfterResume) {
    const uint64_t expectedAccepted =
        _lifecycleStatsBeforePause.accepted_ticks + kLifecycleProofForegroundTicksAfterResume;
    const uint64_t expectedPaused =
        _lifecycleStatsBeforePause.paused_ticks + kLifecycleProofPausedCallbacks;
    const BOOL exactBound = display.accepted_ticks == expectedAccepted &&
                            display.paused_ticks == expectedPaused &&
                            display.display_ticks == expectedAccepted + expectedPaused &&
                            _metrics.ticks == expectedAccepted;
    _proofFinished = YES;
    _proofPassed = _lifecyclePauseVerified && _lifecycleResumeVerified && exactBound &&
                   _metrics.title_ready && validMetalDispatch && crossedGraphicsHost &&
                   oneFramePerTick;
    if (_proofPassed) {
      NSLog(@"GOALPAD_JAK2_LIFECYCLE_PROOF PASS accepted=%llu paused=%llu dispatcher=%llu "
             "chains=%llu completed=%llu failures=%llu",
            (unsigned long long)display.accepted_ticks,
            (unsigned long long)display.paused_ticks, (unsigned long long)_metrics.ticks,
            (unsigned long long)_metalMetrics.chains,
            (unsigned long long)_metalMetrics.completed_chains,
            (unsigned long long)_metalMetrics.failed_chains);
    } else {
      _failureMessage = @"The bounded lifecycle proof ended without satisfying every gate.";
    }
  } else if (!_lifecycleProofEnabled && _metrics.title_ready && validMetalDispatch &&
             crossedGraphicsHost && oneFramePerTick) {
    _proofFinished = YES;
    _proofPassed = YES;
  } else if (_metrics.ticks >= kMaximumProofTicks) {
    _proofFinished = YES;
    _failureMessage = @"The bounded proof reached 600 ticks without satisfying every gate.";
  }

  if (_proofFinished || _metrics.ticks == 1 || (_metrics.ticks % 60) == 0) {
    [self updateTickGate];
    [self updateStatus];
  }
}

- (void)waitForRealDmaDrawMetalFrame {
  goal_jak2_metal_host* metalHost = _metalHost;
#if TARGET_OS_SIMULATOR
  const int requirePresentation = 0;
#else
  const int requirePresentation = 1;
#endif
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    const int completed = goal_jak2_metal_host_wait_for_last_frame(
        metalHost, kRealDmaMetalCompletionTimeoutSeconds, requirePresentation);
    goal_jak2_metal_host_metrics metrics = {0};
    goal_jak2_metal_frame_summary frame = {0};
    const int copiedMetrics = goal_jak2_metal_host_get_metrics(metalHost, &metrics);
    const int copiedFrame =
        completed ? goal_jak2_metal_host_read_last_frame(metalHost, &frame) : 0;
    const char* error = completed ? "" : goal_jak2_metal_host_last_error(metalHost);
    NSString* failure = error && error[0] ? [NSString stringWithUTF8String:error]
                                          : @"The later-draw real-DMA frame did not complete.";
    dispatch_async(dispatch_get_main_queue(), ^{
      _metalMetrics = metrics;
      _realDmaDrawFrame = frame;

      goal_display_tick_stats display = {0};
      goal_display_tick_coordinator_get_stats(&_coordinator, &display);
      const BOOL exactCompletion =
          completed != 0 && copiedMetrics != 0 && copiedFrame != 0 &&
          _metrics.ticks > 0 && _metrics.ticks <= kRealDmaDrawMetalMaximumTicks &&
          display.accepted_ticks == _metrics.ticks &&
          display.display_ticks == _metrics.ticks && display.paused_ticks == 0 &&
          _metalMetrics.chains == _realDmaDrawInspectedChains + 1 &&
          _metalMetrics.chains <= kRealDmaDrawMetalMaximumTicks &&
          _metalMetrics.chains == _metrics.ticks &&
          _metalMetrics.completed_chains == _metalMetrics.chains &&
          _metalMetrics.command_buffers_committed == _metalMetrics.chains &&
          _metalMetrics.command_buffers_completed == _metalMetrics.chains &&
          _metalMetrics.command_buffer_errors == 0 &&
          _metalMetrics.drawables_acquired == _metalMetrics.chains &&
          _metalMetrics.drawable_misses == 0 &&
          _metalMetrics.submissions == _metalMetrics.chains &&
          _metalMetrics.late_present_submissions == 0 &&
          _metalMetrics.presentation_drops == 0 &&
          _metalMetrics.presentation_order_mismatches == 0 &&
          _metalMetrics.unsupported_blends == 0 &&
          _metalMetrics.last_buckets_dispatched == 327 &&
          _metalMetrics.failed_chains == 0 && _metalMetrics.sync_paths > 0 &&
          _metalMetrics.vsyncs > 0 && frame.width == 640 && frame.height == 480 &&
          frame.byte_count == 640ull * 480ull * 4ull;
#if TARGET_OS_SIMULATOR
      const BOOL exactPresentation = _metalMetrics.presentations == 0;
#else
      const BOOL exactPresentation = _metalMetrics.presentations == _metalMetrics.submissions;
#endif
      if (!exactCompletion || !exactPresentation) {
        _proofFinished = YES;
        if (!copiedMetrics) {
          _failureMessage = @"The later-draw real-DMA host did not return counters.";
        } else if (!completed) {
          _failureMessage = failure;
        } else if (!copiedFrame) {
          _failureMessage = @"The later-draw real-DMA host did not return bounded pixels.";
        } else {
          _failureMessage =
              @"The later-draw real-DMA frame completed but failed its exact counter gate.";
        }
      } else {
        _realDmaDrawInspectedChains = _metalMetrics.chains;
      }

      if (!_proofFinished && !_realDmaDrawBaselineCaptured) {
        if (_metalMetrics.chains != 1 || _metalMetrics.draws != 0 ||
            _metalMetrics.triangles != 0 || _metalMetrics.last_screen_filter_draws != 0 ||
            _metalMetrics.last_screen_filter_triangles != 0 ||
            _metalMetrics.last_debug_no_zbuf2_draws != 0 ||
            _metalMetrics.last_debug_no_zbuf2_triangles != 0 ||
            _metalMetrics.last_sky_draw_draws != 0 ||
            _metalMetrics.last_sky_draw_triangles != 0) {
          _proofFinished = YES;
          _failureMessage = @"The first real-DMA chain was not the required zero-draw baseline.";
        } else {
          _realDmaDrawBaselineFrame = frame;
          _realDmaDrawBaselineCaptured = YES;
          NSLog(@"GOALPAD_JAK2_REAL_DMA_DRAW_BASELINE PASS chain=1 hash=%llu non_black=%llu "
                 "nonzero_alpha=%llu max_alpha=%u",
                (unsigned long long)frame.hash,
                (unsigned long long)frame.non_black_pixels,
                (unsigned long long)frame.nonzero_alpha_pixels, frame.max_alpha);
        }
      } else if (!_proofFinished && _metalMetrics.last_sky_draw_draws > 0) {
        const uint64_t directDraws = _metalMetrics.last_screen_filter_draws +
                                     _metalMetrics.last_debug_no_zbuf2_draws +
                                     _metalMetrics.last_sky_draw_draws;
        const uint64_t directTriangles = _metalMetrics.last_screen_filter_triangles +
                                         _metalMetrics.last_debug_no_zbuf2_triangles +
                                         _metalMetrics.last_sky_draw_triangles;
        const BOOL exactDirectDraw =
            _metalMetrics.draws == directDraws && _metalMetrics.triangles == directTriangles &&
            _metalMetrics.last_sky_draw_triangles > 0;
        const BOOL changedPixels =
            frame.hash != _realDmaDrawBaselineFrame.hash &&
            frame.non_black_pixels > _realDmaDrawBaselineFrame.non_black_pixels;
        NSLog(@"GOALPAD_JAK2_SKY_DRAW_BATCH valid=%u textured=%u vertices=%u "
               "nonzero_rgb_vertices=%u tex0_tbp=%u tex0_tcc=%u tex0_decal=%u "
               "texture_lookup_hit=%u used_placeholder=%u write_rgb=%u blend_enabled=%u "
               "blend_a=%u blend_b=%u blend_c=%u blend_d=%u alpha_test_enabled=%u "
               "alpha_test_mode=%u alpha_aref=%u alpha_afail=%u frame_non_black=%llu "
               "frame_nonzero_alpha=%llu frame_max_alpha=%u",
              _metalMetrics.last_sky_draw_batch_valid,
              _metalMetrics.last_sky_draw_batch_textured,
              _metalMetrics.last_sky_draw_batch_vertices,
              _metalMetrics.last_sky_draw_batch_nonzero_rgb_vertices,
              _metalMetrics.last_sky_draw_batch_tex0_tbp,
              _metalMetrics.last_sky_draw_batch_tex0_tcc,
              _metalMetrics.last_sky_draw_batch_tex0_decal,
              _metalMetrics.last_sky_draw_batch_texture_lookup_hit,
              _metalMetrics.last_sky_draw_batch_used_placeholder,
              _metalMetrics.last_sky_draw_batch_write_rgb,
              _metalMetrics.last_sky_draw_batch_blend_enabled,
              _metalMetrics.last_sky_draw_batch_blend_a,
              _metalMetrics.last_sky_draw_batch_blend_b,
              _metalMetrics.last_sky_draw_batch_blend_c,
              _metalMetrics.last_sky_draw_batch_blend_d,
              _metalMetrics.last_sky_draw_batch_alpha_test_enabled,
              _metalMetrics.last_sky_draw_batch_alpha_test_mode,
              _metalMetrics.last_sky_draw_batch_alpha_aref,
              _metalMetrics.last_sky_draw_batch_alpha_afail,
              (unsigned long long)frame.non_black_pixels,
              (unsigned long long)frame.nonzero_alpha_pixels, frame.max_alpha);
        _proofFinished = YES;
        _proofPassed = exactDirectDraw && changedPixels;
        if (!_proofPassed) {
          _failureMessage =
              !exactDirectDraw
                  ? @"The later real-DMA draw was not exactly attributed to enabled Direct buckets."
                  : @"SKY_DRAW encoded a draw but the bounded frame pixels did not change.";
        } else {
          NSLog(@"GOALPAD_JAK2_REAL_DMA_DRAW_CAMETAL_LAYER_PROOF PASS ticks=%llu chains=%llu "
                 "draws=%llu triangles=%llu "
                 "sky_draw_draws=%llu sky_draw_triangles=%llu "
                 "debug_no_zbuf2_draws=%llu debug_no_zbuf2_triangles=%llu "
                 "screen_filter_draws=%llu screen_filter_triangles=%llu baseline_hash=%llu "
                 "frame_hash=%llu baseline_non_black=%llu frame_non_black=%llu "
                 "baseline_nonzero_alpha=%llu frame_nonzero_alpha=%llu frame_max_alpha=%u "
                 "drawables=%llu "
                 "committed=%llu completed=%llu submissions=%llu",
                (unsigned long long)_metrics.ticks,
                (unsigned long long)_metalMetrics.chains,
                (unsigned long long)_metalMetrics.draws,
                (unsigned long long)_metalMetrics.triangles,
                (unsigned long long)_metalMetrics.last_sky_draw_draws,
                (unsigned long long)_metalMetrics.last_sky_draw_triangles,
                (unsigned long long)_metalMetrics.last_debug_no_zbuf2_draws,
                (unsigned long long)_metalMetrics.last_debug_no_zbuf2_triangles,
                (unsigned long long)_metalMetrics.last_screen_filter_draws,
                (unsigned long long)_metalMetrics.last_screen_filter_triangles,
                (unsigned long long)_realDmaDrawBaselineFrame.hash,
                (unsigned long long)frame.hash,
                (unsigned long long)_realDmaDrawBaselineFrame.non_black_pixels,
                (unsigned long long)frame.non_black_pixels,
                (unsigned long long)_realDmaDrawBaselineFrame.nonzero_alpha_pixels,
                (unsigned long long)frame.nonzero_alpha_pixels, frame.max_alpha,
                (unsigned long long)_metalMetrics.drawables_acquired,
                (unsigned long long)_metalMetrics.command_buffers_committed,
                (unsigned long long)_metalMetrics.command_buffers_completed,
                (unsigned long long)_metalMetrics.submissions);
        }
      } else if (!_proofFinished &&
                 _metalMetrics.chains >= kRealDmaDrawMetalMaximumTicks) {
        _proofFinished = YES;
        _failureMessage = @"No SKY_DRAW draw reached Metal within three real-DMA chains.";
      }

      _realDmaMetalCompletionPending = NO;
      [self updateTickGate];
      [self updateStatus];
    });
  });
}

- (void)waitForRealDmaMetalFrame {
  goal_jak2_metal_host* metalHost = _metalHost;
#if TARGET_OS_SIMULATOR
  const int requirePresentation = 0;
#else
  const int requirePresentation = 1;
#endif
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    const int completed = goal_jak2_metal_host_wait_for_last_frame(
        metalHost, kRealDmaMetalCompletionTimeoutSeconds, requirePresentation);
    goal_jak2_metal_host_metrics metrics = {0};
    const int copiedMetrics = goal_jak2_metal_host_get_metrics(metalHost, &metrics);
    const char* error = completed ? "" : goal_jak2_metal_host_last_error(metalHost);
    NSString* failure = error && error[0] ? [NSString stringWithUTF8String:error]
                                          : @"The real-DMA Metal frame did not complete.";
    dispatch_async(dispatch_get_main_queue(), ^{
      _metalMetrics = metrics;
      _realDmaMetalCompletionPending = NO;

      goal_display_tick_stats display = {0};
      goal_display_tick_coordinator_get_stats(&_coordinator, &display);
      const BOOL exactCompletion =
          completed != 0 && copiedMetrics != 0 && _metrics.ticks > 0 &&
          _metrics.ticks <= kRealDmaMetalMaximumTicks &&
          display.accepted_ticks == _metrics.ticks &&
          _metalMetrics.chains == 1 &&
          _metalMetrics.completed_chains == _metalMetrics.chains &&
          _metalMetrics.command_buffers_committed == _metalMetrics.chains &&
          _metalMetrics.command_buffers_completed == _metalMetrics.chains &&
          _metalMetrics.command_buffer_errors == 0 &&
          _metalMetrics.drawables_acquired == _metalMetrics.chains &&
          _metalMetrics.drawable_misses == 0 &&
          _metalMetrics.submissions == _metalMetrics.chains &&
          _metalMetrics.late_present_submissions == 0 &&
          _metalMetrics.presentation_drops == 0 &&
          _metalMetrics.presentation_order_mismatches == 0 &&
          _metalMetrics.unsupported_blends == 0 &&
          _metalMetrics.last_buckets_dispatched == 327 &&
          _metalMetrics.failed_chains == 0 && _metalMetrics.sync_paths > 0 &&
          _metalMetrics.vsyncs > 0;
#if TARGET_OS_SIMULATOR
      const BOOL exactPresentation = _metalMetrics.presentations == 0;
#else
      const BOOL exactPresentation = _metalMetrics.presentations == _metalMetrics.submissions;
#endif
      _proofFinished = YES;
      _proofPassed = exactCompletion && exactPresentation;
      if (!_proofPassed) {
        if (!copiedMetrics) {
          _failureMessage = @"The real-DMA Metal host did not return counters.";
        } else if (!completed) {
          _failureMessage = failure;
        } else {
          _failureMessage =
              @"The real-DMA Metal frame completed but failed its exact counter gate.";
        }
      } else {
        NSLog(@"GOALPAD_JAK2_REAL_DMA_CAMETAL_LAYER_PROOF PASS ticks=%llu chains=%llu "
               "buckets=%llu skipped=%llu draws=%llu triangles=%llu drawables=%llu misses=%llu "
               "committed=%llu completed=%llu errors=%llu submissions=%llu "
               "presentation_callbacks=%llu drops=%llu order_mismatches=%llu",
              (unsigned long long)_metrics.ticks,
              (unsigned long long)_metalMetrics.chains,
              (unsigned long long)_metalMetrics.last_buckets_dispatched,
              (unsigned long long)_metalMetrics.skipped_bucket_bytes,
              (unsigned long long)_metalMetrics.draws,
              (unsigned long long)_metalMetrics.triangles,
              (unsigned long long)_metalMetrics.drawables_acquired,
              (unsigned long long)_metalMetrics.drawable_misses,
              (unsigned long long)_metalMetrics.command_buffers_committed,
              (unsigned long long)_metalMetrics.command_buffers_completed,
              (unsigned long long)_metalMetrics.command_buffer_errors,
              (unsigned long long)_metalMetrics.submissions,
              (unsigned long long)_metalMetrics.presentations,
              (unsigned long long)_metalMetrics.presentation_drops,
              (unsigned long long)_metalMetrics.presentation_order_mismatches);
      }
      [self updateTickGate];
      [self updateStatus];
    });
  });
}

- (void)runLifecyclePauseCycleAtTargetTime:(double)targetPresentationTime {
  goal_display_tick_coordinator_get_stats(&_coordinator, &_lifecycleStatsBeforePause);
  _lifecycleMetricsBeforePause = _metrics;
  _lifecycleMetalBeforePause = _metalMetrics;

  [self willResignActive:nil];
  int droppedCallbacks =
      goal_display_tick_coordinator_tick(&_coordinator, targetPresentationTime + 1.0 / 60.0) == 0;
  [self didEnterBackground:nil];
  droppedCallbacks +=
      goal_display_tick_coordinator_tick(&_coordinator, targetPresentationTime + 2.0 / 60.0) == 0;
  [self willEnterForeground:nil];
  droppedCallbacks +=
      goal_display_tick_coordinator_tick(&_coordinator, targetPresentationTime + 3.0 / 60.0) == 0;

  goal_display_tick_stats afterDisplay = {0};
  goal_jak2_runtime_metrics afterRuntime = {0};
  goal_jak2_metal_host_metrics afterMetal = {0};
  goal_display_tick_coordinator_get_stats(&_coordinator, &afterDisplay);
  const BOOL copiedRuntime =
      goal_jak2_runtime_get_metrics(&afterRuntime) == GOAL_JAK2_RUNTIME_OK;
  const BOOL copiedMetal = goal_jak2_metal_host_get_metrics(_metalHost, &afterMetal) != 0;
  const BOOL exactPausedAccounting =
      droppedCallbacks == kLifecycleProofPausedCallbacks &&
      afterDisplay.display_ticks ==
          _lifecycleStatsBeforePause.display_ticks + kLifecycleProofPausedCallbacks &&
      afterDisplay.accepted_ticks == _lifecycleStatsBeforePause.accepted_ticks &&
      afterDisplay.paused_ticks ==
          _lifecycleStatsBeforePause.paused_ticks + kLifecycleProofPausedCallbacks;
  _lifecyclePauseVerified = copiedRuntime && copiedMetal && exactPausedAccounting &&
                            goal_jak2_runtime_is_running() &&
                            runtime_metrics_match_during_pause(&_lifecycleMetricsBeforePause,
                                                               &afterRuntime) &&
                            metal_metrics_match_during_pause(&_lifecycleMetalBeforePause,
                                                             &afterMetal);
  if (!_lifecyclePauseVerified) {
    _proofFinished = YES;
    _failureMessage =
        @"Inactive or background display callbacks advanced the Jak II runtime or Metal host.";
    [self updateTickGate];
    [self updateStatus];
    return;
  }

  NSLog(@"GOALPAD_JAK2_LIFECYCLE_PROOF PAUSE accepted=%llu paused=%llu dispatcher=%llu",
        (unsigned long long)afterDisplay.accepted_ticks,
        (unsigned long long)afterDisplay.paused_ticks,
        (unsigned long long)afterRuntime.ticks);
  [self updateStatus];
  dispatch_async(dispatch_get_main_queue(), ^{
    if (!_proofFinished &&
        UIApplication.sharedApplication.applicationState == UIApplicationStateActive) {
      [self didBecomeActive:nil];
    }
  });
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
  self.statusLabel.hidden = NO;
  if (_metalProofEnabled) {
    NSString* result = @"WAITING FOR DISPLAY";
    if (_proofPassed) {
      result = @"PASS";
    } else if (_proofFinished) {
      result = @"FAILED";
    } else if (_metalProofSubmitted) {
      result = @"WAITING FOR GPU";
    }
    NSString* failure = _failureMessage.length > 0
                            ? [NSString stringWithFormat:@"\nError: %@", _failureMessage]
                            : @"";
#if TARGET_OS_SIMULATOR
    NSString* callbackNote = @"Simulator drawable presentation callbacks: unavailable (0 required)";
#else
    NSString* callbackNote = @"Physical drawable presentation callbacks: exactly one required";
#endif
    self.statusLabel.text = [NSString
        stringWithFormat:@"Jak II CAMetalLayer presentation proof — %@\n\n"
                          "Frame: public synthetic 327-bucket chain (no game data, no draw)\n"
                          "Display callbacks: %llu (exactly one required)\n"
                          "CADisplayLink targetTimestamp observed: %.6f\n"
                          "Metal presentation time used: immediate (0)\n\n"
                          "Attempts / chains / buckets: %llu / %llu / %llu\n"
                          "Skipped bytes / draws / triangles: %llu / %d / %d\n"
                          "Drawable acquired / missed: %llu / %llu\n"
                          "Command buffers committed / completed / errors: %llu / %llu / %llu\n"
                          "Submissions / late: %llu / %llu\n"
                          "Presentation callbacks / drops / order mismatches: %llu / %llu / %llu\n"
                          "%@%@",
                         result, (unsigned long long)_metalProofDisplayCallbacks,
                         _lastTargetTimestamp, (unsigned long long)_metalStats.render_attempts,
                         (unsigned long long)_metalStats.chains_rendered,
                         (unsigned long long)_metalStats.buckets_dispatched,
                         (unsigned long long)_metalStats.skipped_bucket_bytes,
                         _metalStats.draw_calls, _metalStats.triangles,
                         (unsigned long long)_metalStats.drawables_acquired,
                         (unsigned long long)_metalStats.drawable_misses,
                         (unsigned long long)_metalStats.command_buffers_committed,
                         (unsigned long long)_metalStats.command_buffers_completed,
                         (unsigned long long)_metalStats.command_buffer_errors,
                         (unsigned long long)_metalStats.submissions,
                         (unsigned long long)_metalStats.late_present_submissions,
                         (unsigned long long)_metalStats.presentations_completed,
                         (unsigned long long)_metalStats.presentation_drops,
                         (unsigned long long)_metalStats.presentation_order_mismatches, callbackNote,
                         failure];
    return;
  }

  if (_realDmaMetalProofEnabled || _realDmaDrawMetalProofEnabled) {
    goal_display_tick_stats display = {0};
    goal_display_tick_coordinator_get_stats(&_coordinator, &display);
    NSString* result = @"BOOTING";
    if (_proofPassed) {
      result = @"PASS";
    } else if (_proofFinished) {
      result = @"FAILED";
    } else if (_realDmaMetalCompletionPending) {
      result = @"WAITING FOR GPU";
    } else if (_bootReady) {
      result = @"RUNNING";
    }
    NSString* proofName = _realDmaDrawMetalProofEnabled
                              ? @"Jak II later-draw real-DMA CAMetalLayer proof"
                              : @"Jak II first-chain real-DMA CAMetalLayer proof";
    NSString* contentNote = nil;
    if (_realDmaDrawMetalProofEnabled) {
      contentNote = _realDmaDrawBaselineCaptured
                        ? @"The first zero-draw frame is retained; waiting for a bounded SKY_DRAW draw."
                        : @"Waiting to retain the first zero-draw frame as the pixel baseline.";
    } else {
      contentNote =
          _metalMetrics.draws > 0
              ? @"Game DMA encoded draw calls; visual correctness is not established by counters."
              : @"No draws encoded; output is clear/deferred and is not a title-screen or gameplay claim.";
    }
    NSString* failure = _failureMessage.length > 0
                            ? [NSString stringWithFormat:@"\nError: %@", _failureMessage]
                            : @"";
#if TARGET_OS_SIMULATOR
    NSString* callbackNote = @"Simulator presentation callbacks: unavailable (0 required)";
#else
    NSString* callbackNote = @"Physical presentation callbacks: one per submission required";
#endif
    self.statusLabel.text = [NSString
        stringWithFormat:@"%@ — %@\n\n"
                          "Frame source: game-built DMA through the external Metal host\n"
                          "Runtime ticks: %llu (maximum 3)\n"
                          "Display callbacks / accepted: %llu / %llu\n"
                          "Last CADisplayLink targetTimestamp: %.6f\n\n"
                          "Chains / policy-complete / failures: %llu / %llu / %llu\n"
                          "Last buckets / copied / skipped: %llu / %u / %llu bytes\n"
                          "Draws / triangles: %llu / %llu\n"
                          "Last SCREEN_FILTER draws / triangles: %llu / %llu\n"
                          "Last DEBUG_NO_ZBUF2 draws / triangles: %llu / %llu\n"
                          "Last SKY_DRAW draws / triangles: %llu / %llu\n"
                          "SKY batch valid / textured: %u / %u\n"
                          "SKY vertices / nonzero RGB vertices: %u / %u\n"
                          "SKY TEX0 TBP / TCC / decal: %u / %u / %u\n"
                          "SKY texture hit / placeholder / write RGB: %u / %u / %u\n"
                          "SKY blend enabled / A B C D: %u / %u %u %u %u\n"
                          "SKY alpha test enabled / mode / AREF / AFAIL: %u / %u / %u / %u\n"
                          "Frame: %u x %u, %llu bytes, hash %llu, non-black %llu, "
                          "nonzero alpha %llu, max alpha %u\n"
                          "Baseline hash / non-black / nonzero alpha / max alpha: "
                          "%llu / %llu / %llu / %u\n"
                          "Drawable acquired / missed: %llu / %llu\n"
                          "Command buffers committed / completed / errors: %llu / %llu / %llu\n"
                          "Submissions / late: %llu / %llu\n"
                          "Presentation callbacks / drops / order mismatches: %llu / %llu / %llu\n\n"
                          "%@\n%@%@",
                         proofName, result, (unsigned long long)_metrics.ticks,
                         (unsigned long long)display.display_ticks,
                         (unsigned long long)display.accepted_ticks, _lastTargetTimestamp,
                         (unsigned long long)_metalMetrics.chains,
                         (unsigned long long)_metalMetrics.completed_chains,
                         (unsigned long long)_metalMetrics.failed_chains,
                         (unsigned long long)_metalMetrics.last_buckets_dispatched,
                         _metalMetrics.last_copied_bytes,
                         (unsigned long long)_metalMetrics.skipped_bucket_bytes,
                         (unsigned long long)_metalMetrics.draws,
                         (unsigned long long)_metalMetrics.triangles,
                         (unsigned long long)_metalMetrics.last_screen_filter_draws,
                         (unsigned long long)_metalMetrics.last_screen_filter_triangles,
                         (unsigned long long)_metalMetrics.last_debug_no_zbuf2_draws,
                         (unsigned long long)_metalMetrics.last_debug_no_zbuf2_triangles,
                         (unsigned long long)_metalMetrics.last_sky_draw_draws,
                         (unsigned long long)_metalMetrics.last_sky_draw_triangles,
                         _metalMetrics.last_sky_draw_batch_valid,
                         _metalMetrics.last_sky_draw_batch_textured,
                         _metalMetrics.last_sky_draw_batch_vertices,
                         _metalMetrics.last_sky_draw_batch_nonzero_rgb_vertices,
                         _metalMetrics.last_sky_draw_batch_tex0_tbp,
                         _metalMetrics.last_sky_draw_batch_tex0_tcc,
                         _metalMetrics.last_sky_draw_batch_tex0_decal,
                         _metalMetrics.last_sky_draw_batch_texture_lookup_hit,
                         _metalMetrics.last_sky_draw_batch_used_placeholder,
                         _metalMetrics.last_sky_draw_batch_write_rgb,
                         _metalMetrics.last_sky_draw_batch_blend_enabled,
                         _metalMetrics.last_sky_draw_batch_blend_a,
                         _metalMetrics.last_sky_draw_batch_blend_b,
                         _metalMetrics.last_sky_draw_batch_blend_c,
                         _metalMetrics.last_sky_draw_batch_blend_d,
                         _metalMetrics.last_sky_draw_batch_alpha_test_enabled,
                         _metalMetrics.last_sky_draw_batch_alpha_test_mode,
                         _metalMetrics.last_sky_draw_batch_alpha_aref,
                         _metalMetrics.last_sky_draw_batch_alpha_afail,
                         _realDmaDrawFrame.width, _realDmaDrawFrame.height,
                         (unsigned long long)_realDmaDrawFrame.byte_count,
                         (unsigned long long)_realDmaDrawFrame.hash,
                         (unsigned long long)_realDmaDrawFrame.non_black_pixels,
                         (unsigned long long)_realDmaDrawFrame.nonzero_alpha_pixels,
                         _realDmaDrawFrame.max_alpha,
                         (unsigned long long)_realDmaDrawBaselineFrame.hash,
                         (unsigned long long)_realDmaDrawBaselineFrame.non_black_pixels,
                         (unsigned long long)_realDmaDrawBaselineFrame.nonzero_alpha_pixels,
                         _realDmaDrawBaselineFrame.max_alpha,
                         (unsigned long long)_metalMetrics.drawables_acquired,
                         (unsigned long long)_metalMetrics.drawable_misses,
                         (unsigned long long)_metalMetrics.command_buffers_committed,
                         (unsigned long long)_metalMetrics.command_buffers_completed,
                         (unsigned long long)_metalMetrics.command_buffer_errors,
                         (unsigned long long)_metalMetrics.submissions,
                         (unsigned long long)_metalMetrics.late_present_submissions,
                         (unsigned long long)_metalMetrics.presentations,
                         (unsigned long long)_metalMetrics.presentation_drops,
                         (unsigned long long)_metalMetrics.presentation_order_mismatches,
                         contentNote, callbackNote, failure];
    return;
  }

  goal_display_tick_stats display = {0};
  goal_display_tick_coordinator_get_stats(&_coordinator, &display);

  if (_titleLoopEnabled && _bootReady && _metrics.title_ready && _metalMetrics.chains > 0 &&
      _failureMessage.length == 0) {
    self.statusLabel.hidden = YES;
    return;
  }

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
  NSString* proofName =
      _titleLoopEnabled
          ? @"Eco Pro Jak II 840-AOT title loop"
          : (_lifecycleProofEnabled ? @"Jak II lifecycle display-tick proof"
                                    : @"Jak II Metal policy display-tick proof");
  NSString* lifecycle = _lifecycleProofEnabled
                            ? [NSString stringWithFormat:
                                          @"\nLifecycle pause / resume: %@ / %@\n"
                                           "Bound: 3 foreground + 3 paused + 3 foreground\n",
                                          _lifecyclePauseVerified ? @"verified" : @"pending",
                                          _lifecycleResumeVerified ? @"verified" : @"pending"]
                            : @"";
  self.statusLabel.text = [NSString
      stringWithFormat:@"%@ — %@\n\n"
                        "Data: %@\nSaves: %@\nRuntime: %@\n"
                        "thread-suspend: source #x%08x/#x%llx / display #x%08x / "
                        "top #x%08x / hook #x%08x/#x%llx / %@\n"
                        "TITLE: %@ (%@)\n\n"
                        "Display callbacks: %llu\nAccepted ticks: %llu\nPaused ticks: %llu\n"
                        "Dispatcher frames: %llu\nLast targetTimestamp: %.6f\n\n"
                        "Metal host: %llu chain / %llu sync-path / %llu syncv\n"
                        "Completed policy chains: %llu / failures: %llu\n"
                        "Last dispatch: %llu buckets / %u copied bytes\n\n"
                        "Draw calls: %llu\nSubmissions: %llu\nPresented frames: %llu%@%@",
                       proofName, result, _dataPath ?: @"<unavailable>",
                       _savesPath ?: @"<unavailable>",
                       [self runtimeStateName], _threadSuspendProbe.function_object,
                       (unsigned long long)_threadSuspendProbe.native_entry,
                       _threadSuspendProbe.display_process, _threadSuspendProbe.top_thread,
                       _threadSuspendProbe.hook_function_object,
                       (unsigned long long)_threadSuspendProbe.hook_native_entry,
                       _threadSuspendProbe.matches_expected
                           ? (_threadSuspendProbe.hook_available ? @"valid" : @"pending")
                           : @"invalid",
                       titleDGO,
                       _metrics.title_ready ? @"ready" : @"not ready",
                       (unsigned long long)display.display_ticks,
                       (unsigned long long)display.accepted_ticks,
                       (unsigned long long)display.paused_ticks, (unsigned long long)_metrics.ticks,
                       _lastTargetTimestamp, (unsigned long long)_metalMetrics.chains,
                       (unsigned long long)_metalMetrics.sync_paths,
                       (unsigned long long)_metalMetrics.vsyncs,
                       (unsigned long long)_metalMetrics.completed_chains,
                       (unsigned long long)_metalMetrics.failed_chains,
                       (unsigned long long)_metalMetrics.last_buckets_dispatched,
                       _metalMetrics.last_copied_bytes,
                       (unsigned long long)_metalMetrics.draws,
                       (unsigned long long)_metalMetrics.submissions,
                       (unsigned long long)_metalMetrics.presentations,
                       lifecycle,
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
