// SPDX-License-Identifier: MIT
plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.eloquence.tts"
    compileSdk = 34
    ndkVersion = "27.2.12479018"   // NDK r27c — matches the converter's libc++ __ndk1 rewrite

    defaultConfig {
        applicationId = "com.eloquence.tts"
        minSdk = 24
        targetSdk = 34
        versionCode = 1
        versionName = "0.1.0"

        ndk {
            // Only arm64 is converted/validated today. x86_64 (emulator) would
            // need an "android-x86_64" ARCH_CONFIG entry — simpler than arm64,
            // since x86_64 Android shares the SysV variadic ABI (no shims).
            abiFilters += "arm64-v8a"
        }
        externalNativeBuild {
            cmake { arguments += "-DELOQUENCE_CJK_ATEXIT=ON" }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("../jni/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    // The converted engine .so files (eci.so + lib<lang>.so + libc++_shared.so)
    // are dropped into src/main/jniLibs/arm64-v8a/ — see android/README.md.
    // They install into the read-only nativeLibraryDir, where dlopen of
    // executable code is permitted (writable app storage is not).
    packaging {
        // Keep the page-aligned engine libs uncompressed so they mmap directly.
        jniLibs { useLegacyPackaging = false }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"))
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
}
