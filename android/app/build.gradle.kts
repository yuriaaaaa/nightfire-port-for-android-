plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "org.nightfirepc.port"
    compileSdk = 35

    defaultConfig {
        applicationId = "org.nightfirepc.port"
        minSdk = 26
        targetSdk = 35
        versionCode = System.getenv("NF_VERSION_CODE")?.toIntOrNull() ?: 1
        versionName = System.getenv("NF_VERSION_NAME") ?: "0.1.0"
        ndk {
            abiFilters += listOf("arm64-v8a")
        }
    }

    sourceSets {
        getByName("main") {
            // As bibliotecas nativas vêm do android/build.sh (o CMakeLists.txt da raiz com o NDK).
            jniLibs.srcDir("src/main/jniLibs")
            // O lado Java do SDL2 vem da própria fonte que o build.sh baixa (android-project), sem cópia no repositório.
            java.srcDir("../deps/SDL2/android-project/app/src/main/java")
            // A pasta hd_ui do port (as fontes e o tokens.json do estilo Modern, os SVG da camada HD), copiada pelo
            // lançador para Internal storage/Nightfire/hd_ui.
            assets.srcDir(layout.buildDirectory.dir("generated/nf-assets"))
        }
    }

    // Nenhuma chave no repositório. Com NF_STORE_PASSWORD e NF_KEYSTORE (ou android/keystore/release.keystore) o APK sai
    // assinado com ela; sem elas, com a chave de depuração.
    val releaseKeystore = System.getenv("NF_KEYSTORE")?.let { file(it) } ?: rootProject.file("keystore/release.keystore")
    val releaseStorePassword = System.getenv("NF_STORE_PASSWORD")
    val hasReleaseKey = releaseKeystore.isFile && !releaseStorePassword.isNullOrEmpty()
    signingConfigs {
        if (hasReleaseKey) {
            create("release") {
                storeFile = releaseKeystore
                storePassword = releaseStorePassword
                keyAlias = System.getenv("NF_KEY_ALIAS") ?: "release"
                keyPassword = System.getenv("NF_KEY_PASSWORD") ?: releaseStorePassword
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = signingConfigs.findByName("release") ?: signingConfigs.getByName("debug")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
    lint {
        abortOnError = false
        checkReleaseBuilds = false
    }
    packaging {
        jniLibs {
            useLegacyPackaging = true
        }
    }
}

val copyHdUi by tasks.registering(Copy::class) {
    from(rootProject.file("../hd_ui")) {
        exclude("fonts/**") // a pasta de fontes do usuário (fora do git)
    }
    into(layout.buildDirectory.dir("generated/nf-assets/hd_ui"))
}
tasks.named("preBuild") { dependsOn(copyHdUi) }
