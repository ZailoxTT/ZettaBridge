/* Hand-written additions to the generated libandroid.so trap stub. Built with -nostdlib like
 * the stub itself, so these may only call through the JNI function tables. */
#include <android/native_activity.h>

/* Activity.finish() on the guest's activity object, through the guest JNIEnv. */
__attribute__((visibility("default"))) void ANativeActivity_finish(ANativeActivity* activity) {
    if (activity == 0 || activity->vm == 0) return;
    JavaVM* vm = activity->vm;
    JNIEnv* env = 0;
    if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK &&
        (*vm)->AttachCurrentThread(vm, &env, 0) != JNI_OK)
        return;
    jclass cls = (*env)->GetObjectClass(env, activity->clazz);
    jmethodID finish = cls != 0 ? (*env)->GetMethodID(env, cls, "finish", "()V") : 0;
    if (finish != 0) (*env)->CallVoidMethod(env, activity->clazz, finish);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (cls != 0) (*env)->DeleteLocalRef(env, cls);
}
