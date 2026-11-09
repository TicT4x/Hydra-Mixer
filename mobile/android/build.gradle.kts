plugins {
    id("com.android.application") version "9.4.1" apply false
}

// Build output outside OneDrive (the project folder is synced): %USERPROFILE%\.hydra-build\android
allprojects {
    layout.buildDirectory.set(file("${System.getProperty("user.home")}/.hydra-build/android/${project.name}"))
}
