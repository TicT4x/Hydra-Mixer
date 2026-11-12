plugins {
    id("com.android.application")
}

android {
    namespace = "de.hydra.link"
    compileSdk = 36

    defaultConfig {
        applicationId = "de.hydra.link"
        minSdk = 28          // AAudio with usage / content type / input presets (Android 9)
        targetSdk = 36
        versionCode = 4
        versionName = "0.8.2"
        ndk { abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64") }
        externalNativeBuild { cmake { arguments += listOf("-DANDROID_STL=c++_static") } }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            // Signed with the debug key so the APK installs directly (no store). Set up a real key before publishing.
            signingConfig = signingConfigs.getByName("debug")
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
            buildStagingDirectory = file("${System.getProperty("user.home")}/.hydra-build/android/cxx")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}
