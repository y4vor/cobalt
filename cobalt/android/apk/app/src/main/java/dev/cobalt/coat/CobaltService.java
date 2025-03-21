// Copyright 2019 The Cobalt Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package dev.cobalt.coat;

import static dev.cobalt.util.Log.TAG;

import android.util.Base64;
import dev.cobalt.util.Log;
import dev.cobalt.util.UsedByNative;

/** Abstract class that provides an interface for Cobalt to interact with a platform service. */
public abstract class CobaltService {
  // Indicate is the service opened, and be able to send data to client
  protected boolean opened = true;
  private final Object lock = new Object();
  private StarboardBridge bridge;
  protected CobaltActivity cobaltActivity;

  /** Interface that returns an object that extends CobaltService. */
  public interface Factory {
    /** Create the service. */
    public CobaltService createCobaltService(long nativeService);

    /** Get the name of the service. */
    public String getServiceName();
  }

  /** Take in a reference to StarboardBridge & use it as needed. Default behavior is no-op. */
  public void receiveStarboardBridge(StarboardBridge bridge) {}

  public void setCobaltActivity(CobaltActivity cobaltActivity) {
    this.cobaltActivity = cobaltActivity;
  }

  // Lifecycle
  /** Prepare service for start or resume. */
  public abstract void beforeStartOrResume();

  /** Prepare service for suspend. */
  public abstract void beforeSuspend();

  /** Prepare service for stop. */
  public abstract void afterStopped();

  // Service API
  /** Response to client from calls to receiveFromClient(). */
  @SuppressWarnings("unused")
  @UsedByNative
  public static class ResponseToClient {
    /** Indicate if the service was unable to receive data because it is in an invalid state. */
    @SuppressWarnings("unused")
    @UsedByNative
    public boolean invalidState;

    /** The synchronous response data from the service. */
    @SuppressWarnings("unused")
    @UsedByNative
    public byte[] data;
  }

  /** Receive data from client of the service. */
  @SuppressWarnings("unused")
  @UsedByNative
  public abstract ResponseToClient receiveFromClient(byte[] data);

  /**
   * Close the service.
   *
   * <p>Once this function returns, it is invalid to call sendToClient for the nativeService, so
   * synchronization must be used to protect against this.
   */
  @SuppressWarnings("unused")
  @UsedByNative
  public void onClose() {
    synchronized (lock) {
      opened = false;
      close();
    }
  }

  public abstract void close();

  /**
   * Send data from the service to the client.
   */
  protected void sendToClient(long nativeService, byte[] data) {
    if (this.cobaltActivity == null) {
      Log.e(TAG, "CobaltActivity is null, can not run evaluateJavaScript()");
      return;
    }

    // Use Base64.NO_WRAP instead of Base64.DEFAULT to avoid adding a new line.
    String base64Data = Base64.encodeToString(data, Base64.NO_WRAP);

    // TODO(b/403638702): - Cobalt: Migrate away from Java Bridge for H5vccPlatformService.
    // Workaround: Explicitly target the 'anchor' iframe for H5vccPlatformService callbacks.
    // This is necessary because the polyfill is injected broadly, but callbacks
    // are registered within the Kabuki app's iframe, which needs to be the
    // execution context for CobaltService.sendToClient(). see b/403277033 for the details.
    String jsCodeTemplate =
      "((w) => {\n"
      + "  let targetWindow = w;\n"
      + "  const appIframe = document.getElementById('anchor');\n"
      + "  if (appIframe?.contentWindow) {\n"
      + "    targetWindow = appIframe.contentWindow;\n"
      + "  }\n"
      + "  targetWindow.H5vccPlatformService.callbackFromAndroid(%d, '%s');\n"
      + "})(window)";

    String jsCode = String.format(jsCodeTemplate, nativeService, base64Data);
    this.cobaltActivity.evaluateJavaScript(jsCode);
  }
}
