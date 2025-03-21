// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ASH_PUBLIC_CPP_AMBIENT_AMBIENT_UI_MODEL_H_
#define ASH_PUBLIC_CPP_AMBIENT_AMBIENT_UI_MODEL_H_

#include "ash/public/cpp/ash_public_export.h"
#include "base/observer_list.h"
#include "base/observer_list_types.h"
#include "base/time/time.h"

namespace ash {

// Enumeration of UI visibility states.
enum class AmbientUiVisibility {
  kShown,    // Screen saver is shown.
  kPreview,  // Same as kShown, but do not lock screen or acquire wake lock.
             // kPreview state is used to show a preview of the screen saver.
             // Users should be able to exit from the preview mode directly.
             // Hence, no need to lock the screen or acquire wake lock.
  kHidden,   // Screen saver is closed; start inactivity timer to restart it.
  kClosed,   // Screen saver is closed; all observers and timers are cancelled.
};

// Enumeration of ambient UI modes. This is used for metrics reporting and
// values should not be re-ordered or removed.
enum class AmbientUiMode {
  kLockScreenUi = 0,
  kInSessionUi = 1,
  kMaxValue = kInSessionUi,
};

// The default time before starting Ambient mode on lock screen.
constexpr base::TimeDelta kLockScreenInactivityTimeout = base::Seconds(7);

// The default time to lock screen in the background after Ambient mode begins.
constexpr base::TimeDelta kLockScreenBackgroundTimeout = base::Seconds(5);

// The default interval to refresh photos.
constexpr base::TimeDelta kPhotoRefreshInterval = base::Seconds(60);

// The default time to run screen saver before put the device into sleep.
constexpr base::TimeDelta kDefaultScreenSaverDuration = base::Minutes(10);

// The default animation playback speed. Not used in slideshow mode.
constexpr float kAnimationPlaybackSpeed = 1.f;

// The default time before starting the managed screensaver. Must match with the
// default value declared in the DeviceScreensaverLoginScreenIdleTimeoutSeconds
// and ScreensaverLockScreenIdleTimeoutSeconds policies.
constexpr base::TimeDelta kManagedScreensaverInactivityTimeout =
    base::Seconds(7);

// The default interval to refresh images in the managed screensaver. Must match
// with the default value declared in the
// DeviceScreensaverImageDisplayIntervalSeconds and
// ScreensaverImageDisplayIntervalSeconds policies.
constexpr base::TimeDelta kManagedScreensaverImageRefreshInterval =
    base::Seconds(60);

// A checked observer which receives notification of changes to the Ambient Mode
// UI model.
class ASH_PUBLIC_EXPORT AmbientUiModelObserver : public base::CheckedObserver {
 public:
  // Invoked when the Ambient Mode UI visibility changed.
  virtual void OnAmbientUiVisibilityChanged(AmbientUiVisibility visibility) {}

  virtual void OnLockScreenInactivityTimeoutChanged(base::TimeDelta timeout) {}

  virtual void OnBackgroundLockScreenTimeoutChanged(base::TimeDelta timeout) {}
};

// Models the Ambient Mode UI.
class ASH_PUBLIC_EXPORT AmbientUiModel {
 public:
  static AmbientUiModel* Get();

  AmbientUiModel();
  AmbientUiModel(const AmbientUiModel&) = delete;
  AmbientUiModel& operator=(AmbientUiModel&) = delete;
  ~AmbientUiModel();

  void AddObserver(AmbientUiModelObserver* observer);
  void RemoveObserver(AmbientUiModelObserver* observer);

  // Updates current UI visibility and notifies all subscribers.
  void SetUiVisibility(AmbientUiVisibility visibility);

  AmbientUiVisibility ui_visibility() const { return ui_visibility_; }

  void SetLockScreenInactivityTimeout(base::TimeDelta timeout);

  base::TimeDelta lock_screen_inactivity_timeout() const {
    return lock_screen_inactivity_timeout_;
  }

  void SetBackgroundLockScreenTimeout(base::TimeDelta timeout);

  base::TimeDelta background_lock_screen_timeout() const {
    return background_lock_screen_timeout_;
  }

  void SetPhotoRefreshInterval(base::TimeDelta interval);

  base::TimeDelta photo_refresh_interval() const {
    return photo_refresh_interval_;
  }

  void set_animation_playback_speed(float animation_playback_speed) {
    animation_playback_speed_ = animation_playback_speed;
  }

  float animation_playback_speed() const { return animation_playback_speed_; }

 private:
  void NotifyAmbientUiVisibilityChanged();

  void NotifyLockScreenInactivityTimeoutChanged();

  void NotifyBackgroundLockScreenTimeoutChanged();

  AmbientUiVisibility ui_visibility_ = AmbientUiVisibility::kClosed;

  // The delay to show ambient mode after lock screen is activated.
  base::TimeDelta lock_screen_inactivity_timeout_ =
      kLockScreenInactivityTimeout;

  // The delay between ambient mode starts and enabling lock screen.
  base::TimeDelta background_lock_screen_timeout_ =
      kLockScreenBackgroundTimeout;

  // The interval to refresh photos.
  base::TimeDelta photo_refresh_interval_ = kPhotoRefreshInterval;

  // Animation playback speed. Not used in slideshow mode.
  float animation_playback_speed_ = kAnimationPlaybackSpeed;

  base::ObserverList<AmbientUiModelObserver> observers_;
};

ASH_PUBLIC_EXPORT std::ostream& operator<<(std::ostream& out,
                                           AmbientUiMode mode);

ASH_PUBLIC_EXPORT std::ostream& operator<<(std::ostream& out,
                                           AmbientUiVisibility visibility);

}  // namespace ash

#endif  // ASH_PUBLIC_CPP_AMBIENT_AMBIENT_UI_MODEL_H_
