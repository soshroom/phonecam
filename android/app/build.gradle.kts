plugins {
    id("com.android.application")
    kotlin("android")
}

android {
    namespace = "dev.soshroom.phonecam"
    compileSdk = 35

    defaultConfig {
        applicationId = "dev.soshroom.phonecam"
        minSdk = 31
        targetSdk = 35
        versionCode = 4
        versionName = "0.1.4"
    }

    buildFeatures {
        buildConfig = true
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
}

dependencies {
    implementation("androidx.core:core-ktx:1.15.0")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("androidx.activity:activity-ktx:1.10.0")
    implementation("com.google.android.material:material:1.12.0")
    implementation("org.nanohttpd:nanohttpd:2.3.1")
}
