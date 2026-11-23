plugins {
    id("com.android.application")
}

android {
    namespace = "com.quinton.sapple"
    compileSdk = 36
    ndkVersion = "29.0.14206865"

    defaultConfig {
        applicationId = "com.quinton.sapple"
        minSdk = 26
        targetSdk = 36
        versionCode = 1
        versionName = "1.2"
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        ndk { abiFilters += listOf("armeabi-v7a", "arm64-v8a", "x86_64") }
        externalNativeBuild {
            cmake { arguments += listOf("-DANDROID_STL=none") }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    // Microsoft's Speech SDK libraries (the natural voices) come from the build's own download, never from git.
    sourceSets["main"].jniLibs.srcDir(layout.buildDirectory.dir("generated/speechSdk").get().asFile)
    // Extracted on install, so the neural bridge can dlopen them by path and the core finds its extensions beside it.
    packaging { jniLibs { useLegacyPackaging = true } }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "4.1.2"
        }
    }
}

// Microsoft's embedded Speech SDK for Android, the same version (1.33) the iPhone and Mac builds bundle, so the natural
// voices behave the same everywhere. Only the native libraries the offline voices need are taken; the Java SDK is not
// used. Gradle downloads it from Maven Central at build time.
val speechSdk by configurations.creating
val speechSdkVersion = "1.33.0"
val speechSdkLibs = listOf("core", "extension.embedded.tts", "extension.embedded.tts.runtime", "extension.onnxruntime")
val stageSpeechSdk by tasks.registering(Sync::class) {
    from({ zipTree(speechSdk.singleFile) }) {
        include(speechSdkLibs.map { "jni/*/libMicrosoft.CognitiveServices.Speech.$it.so" })
        exclude("jni/x86/**")
        eachFile { path = path.removePrefix("jni/") }
        includeEmptyDirs = false
    }
    into(layout.buildDirectory.dir("generated/speechSdk"))
}
tasks.named("preBuild") { dependsOn(stageSpeechSdk) }

dependencies {
    speechSdk("com.microsoft.cognitiveservices.speech:client-sdk-embedded:$speechSdkVersion@aar")
    androidTestImplementation("androidx.test:runner:1.6.2")
    androidTestImplementation("androidx.test.ext:junit:1.2.1")
    androidTestImplementation("junit:junit:4.13.2")
}
