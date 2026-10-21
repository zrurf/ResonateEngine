#ifndef RESONATE_WINDOW_ANDROID_HPP
#define RESONATE_WINDOW_ANDROID_HPP

/*
 * The Android window's source is the NativeActivity: android_main owns the
 * android_app and hands it here before the run starts. Host-side: a plugin
 * resolves the window capability, never this.
 */

struct android_app;

namespace resonate::window
{

/* Installs this backend's command and input callbacks and adopts a window the
   activity already created. Call once, on the android_main thread, before
   Session::start. */
void attachAndroidApp(android_app* app);

} // namespace resonate::window

#endif /* RESONATE_WINDOW_ANDROID_HPP */
