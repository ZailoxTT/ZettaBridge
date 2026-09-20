// A library the guest loads by itself: the shim's JNI_OnLoad dlopens it, so Java never sees it
// and ART resolves nothing against it. Its Java_* export must still be bound.
#include <jni.h>

JNIEXPORT jint JNICALL Java_zb_Hidden_value(JNIEnv* env, jclass cls) {
    return 41;
}
