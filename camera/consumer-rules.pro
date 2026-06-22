# Consumer ProGuard rules for the :camera library.
# CameraActivity / HdrCameraSession / RecordingService and the engine-audio Java
# classes are reached via JNI (FindClass / GetMethodID by name) and reflection
# from the NativeActivity loader, so they must survive shrinking in a host app.
-keep class io.nava.camera.** { *; }
-keep class com.nerio.audioengine.** { *; }
