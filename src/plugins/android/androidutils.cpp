// Copyright (C) 2016 BogDan Vatra <bog_dan_ro@yahoo.com>
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "androidutils.h"

#include "androidbuildapkstep.h"
#include "androidconfigurations.h"
#include "androidconstants.h"
#include "androidqtversion.h"
#include "androidsdkmanager.h"
#include "androidtr.h"

#include <cmakeprojectmanager/cmakeprojectconstants.h>

#include <coreplugin/icontext.h>
#include <coreplugin/icore.h>
#include <coreplugin/messagemanager.h>

#include <projectexplorer/buildsteplist.h>
#include <projectexplorer/buildsystem.h>
#include <projectexplorer/projectexplorerconstants.h>
#include <projectexplorer/target.h>
#include <projectexplorer/toolchainkitaspect.h>

#include <qtsupport/qtkitaspect.h>

#include <solutions/tasking/conditional.h>
#include <solutions/tasking/tcpsocket.h>

#include <utils/algorithm.h>
#include <utils/async.h>
#include <utils/qtcprocess.h>
#include <utils/qtcassert.h>

#include <QDomDocument>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QMessageBox>
#include <QVersionNumber>

using namespace Core;
using namespace ProjectExplorer;
using namespace Tasking;
using namespace Utils;

using namespace std::chrono_literals;

namespace Android::Internal {

const char AndroidManifestName[] = "AndroidManifest.xml";
const char AndroidDeviceSn[] = "AndroidDeviceSerialNumber";
const char AndroidDeviceAbis[] = "AndroidDeviceAbis";
const char ApiLevelKey[] = "AndroidVersion.ApiLevel";
const char qtcSignature[] = "This file is generated by QtCreator to be read by "
                            "androiddeployqt and should not be modified by hand.";

static Q_LOGGING_CATEGORY(androidManagerLog, "qtc.android.androidManager", QtWarningMsg)

static std::optional<QDomElement> documentElement(const FilePath &fileName)
{
    if (!fileName.exists()) {
        qCDebug(androidManagerLog, "Manifest file %s doesn't exist.",
                fileName.toUserOutput().toUtf8().data());
        return {};
    }

    const expected_str<QByteArray> result = fileName.fileContents();
    if (!result) {
        MessageManager::writeDisrupting(Tr::tr("Cannot open \"%1\".")
                                            .arg(fileName.toUserOutput())
                                            .append(' ')
                                            .append(result.error()));
        return {};
    }
    QDomDocument doc;
    if (!doc.setContent(*result)) {
        MessageManager::writeDisrupting(Tr::tr("Cannot parse \"%1\".").arg(fileName.toUserOutput()));
        return {};
    }
    return doc.documentElement();
}

static int parseMinSdk(const QDomElement &manifestElem)
{
    const QDomElement usesSdk = manifestElem.firstChildElement("uses-sdk");
    if (!usesSdk.isNull() && usesSdk.hasAttribute("android:minSdkVersion")) {
        bool ok;
        int tmp = usesSdk.attribute("android:minSdkVersion").toInt(&ok);
        if (ok)
            return tmp;
    }
    return 0;
}

static const ProjectNode *currentProjectNode(const Target *target)
{
    return target->project()->findNodeForBuildKey(target->activeBuildKey());
}

QString packageName(const Target *target)
{
    QString packageName;

    // Check build.gradle
    auto isComment = [](const QByteArray &trimmed) {
        return trimmed.startsWith("//") || trimmed.startsWith('*') || trimmed.startsWith("/*");
    };

    const FilePath androidBuildDir = androidBuildDirectory(target);
    const expected_str<QByteArray> gradleContents = androidBuildDir.pathAppended("build.gradle")
                                                        .fileContents();
    if (gradleContents) {
        const auto lines = gradleContents->split('\n');
        for (const auto &line : lines) {
            const QByteArray trimmed = line.trimmed();
            if (isComment(trimmed) || !trimmed.contains("namespace"))
                continue;

            int idx = trimmed.indexOf('=');
            if (idx == -1)
                idx = trimmed.indexOf(' ');
            if (idx > -1) {
                packageName = QString::fromUtf8(trimmed.mid(idx + 1).trimmed());
                if (packageName == "androidPackageName") {
                    // Check gradle.properties
                    const QSettings gradleProperties = QSettings(
                        androidBuildDir.pathAppended("gradle.properties").toFSPathString(),
                        QSettings::IniFormat);
                    packageName = gradleProperties.value("androidPackageName").toString();
                } else {
                    // Remove quotes
                    if (packageName.size() > 2)
                        packageName = packageName.remove(0, 1).chopped(1);
                }

                break;
            }
        }
    }

    if (packageName.isEmpty()) {
        // Check AndroidManifest.xml
        const auto element = documentElement(manifestPath(target));
        if (element)
            packageName = element->attribute("package");
    }

    return packageName;
}

QString activityName(const Target *target)
{
    const auto element = documentElement(manifestPath(target));
    if (!element)
        return {};
    return element->firstChildElement("application").firstChildElement("activity")
                                                    .attribute("android:name");
}

static FilePath manifestSourcePath(const Target *target)
{
    if (const ProjectNode *node = currentProjectNode(target)) {
        const QString packageSource
            = node->data(Android::Constants::AndroidPackageSourceDir).toString();
        if (!packageSource.isEmpty()) {
            const FilePath manifest = FilePath::fromUserInput(packageSource + "/AndroidManifest.xml");
            if (manifest.exists())
                return manifest;
        }
    }
    return manifestPath(target);
}

/*!
    Returns the minimum Android API level set for the APK. Minimum API level
    of the kit is returned if the manifest file of the APK cannot be found
    or parsed.
*/
int minimumSDK(const Target *target)
{
    const auto element = documentElement(manifestSourcePath(target));
    if (!element)
        return minimumSDK(target->kit());

    const int minSdkVersion = parseMinSdk(*element);
    if (minSdkVersion == 0)
        return defaultMinimumSDK(QtSupport::QtKitAspect::qtVersion(target->kit()));
    return minSdkVersion;
}

/*!
    Returns the minimum Android API level required by the kit to compile. -1 is
    returned if the kit does not support Android.
*/
int minimumSDK(const Kit *kit)
{
    int minSdkVersion = -1;
    QtSupport::QtVersion *version = QtSupport::QtKitAspect::qtVersion(kit);
    if (version && version->targetDeviceTypes().contains(Constants::ANDROID_DEVICE_TYPE)) {
        const FilePath stockManifestFilePath = version->prefix().pathAppended(
            "src/android/templates/AndroidManifest.xml");

        const auto element = documentElement(stockManifestFilePath);
        if (element)
            minSdkVersion = parseMinSdk(*element);
    }
    if (minSdkVersion == 0)
        return defaultMinimumSDK(version);
    return minSdkVersion;
}

QString buildTargetSDK(const Target *target)
{
    if (auto bc = target->activeBuildConfiguration()) {
        if (auto androidBuildApkStep = bc->buildSteps()->firstOfType<AndroidBuildApkStep>())
            return androidBuildApkStep->buildTargetSdk();
    }

    QString fallback = AndroidConfig::apiLevelNameFor(sdkManager().latestAndroidSdkPlatform());
    return fallback;
}

QStringList applicationAbis(const Target *target)
{
    auto qt = dynamic_cast<AndroidQtVersion *>(QtSupport::QtKitAspect::qtVersion(target->kit()));
    return qt ? qt->androidAbis() : QStringList();
}

QString archTriplet(const QString &abi)
{
    if (abi == ProjectExplorer::Constants::ANDROID_ABI_X86) {
        return {"i686-linux-android"};
    } else if (abi == ProjectExplorer::Constants::ANDROID_ABI_X86_64) {
        return {"x86_64-linux-android"};
    } else if (abi == ProjectExplorer::Constants::ANDROID_ABI_ARM64_V8A) {
        return {"aarch64-linux-android"};
    }
    return {"arm-linux-androideabi"};
}

QJsonObject deploymentSettings(const Target *target)
{
    QtSupport::QtVersion *qt = QtSupport::QtKitAspect::qtVersion(target->kit());
    if (!qt)
        return {};

    auto tc = ToolchainKitAspect::cxxToolchain(target->kit());
    if (!tc || tc->typeId() != Constants::ANDROID_TOOLCHAIN_TYPEID)
        return {};
    QJsonObject settings;
    settings["_description"] = qtcSignature;
    settings["qt"] = qt->prefix().toFSPathString();
    settings["ndk"] = AndroidConfig::ndkLocation(qt).toFSPathString();
    settings["sdk"] = AndroidConfig::sdkLocation().toFSPathString();
    if (!qt->supportsMultipleQtAbis()) {
        const QStringList abis = applicationAbis(target);
        QTC_ASSERT(abis.size() == 1, return {});
        settings["stdcpp-path"] = (AndroidConfig::toolchainPath(qt) / "sysroot/usr/lib"
                                   / archTriplet(abis.first()) / "libc++_shared.so")
                                      .toFSPathString();
    } else {
        settings["stdcpp-path"]
            = AndroidConfig::toolchainPath(qt).pathAppended("sysroot/usr/lib").toFSPathString();
    }
    settings["toolchain-prefix"] =  "llvm";
    settings["tool-prefix"] = "llvm";
    settings["useLLVM"] = true;
    settings["ndk-host"] = AndroidConfig::toolchainHost(qt);
    return settings;
}

bool isQtCreatorGenerated(const FilePath &deploymentFile)
{
    const expected_str<QByteArray> result = deploymentFile.fileContents();
    if (!result)
        return false;
    return QJsonDocument::fromJson(*result).object()["_description"].toString() == qtcSignature;
}

FilePath androidBuildDirectory(const Target *target)
{
    QString suffix;
    const Project *project = target->project();
    if (project->extraData(Android::Constants::AndroidBuildTargetDirSupport).toBool()
        && project->extraData(Android::Constants::UseAndroidBuildTargetDir).toBool())
        suffix = QString("-%1").arg(target->activeBuildKey());

    return buildDirectory(target) / (Constants::ANDROID_BUILD_DIRECTORY + suffix);
}

FilePath androidAppProcessDir(const Target *target)
{
    return buildDirectory(target) / Constants::ANDROID_APP_PROCESS_DIRECTORY;
}

bool isQt5CmakeProject(const ProjectExplorer::Target *target)
{
    const QtSupport::QtVersion *qt = QtSupport::QtKitAspect::qtVersion(target->kit());
    const bool isQt5 = qt && qt->qtVersion() < QVersionNumber(6, 0, 0);
    const Context cmakeCtx(CMakeProjectManager::Constants::CMAKE_PROJECT_ID);
    const bool isCmakeProject = (target->project()->projectContext() == cmakeCtx);
    return isQt5 && isCmakeProject;
}

FilePath buildDirectory(const Target *target)
{
    if (const BuildSystem *bs = target->buildSystem()) {
        const QString buildKey = target->activeBuildKey();

        // Get the target build dir based on the settings file path
        FilePath buildDir;
        const ProjectNode *node = target->project()->findNodeForBuildKey(buildKey);
        if (node) {
            const QString settingsFile = node->data(Constants::AndroidDeploySettingsFile).toString();
            buildDir = FilePath::fromUserInput(settingsFile).parentDir();
        }

        if (!buildDir.isEmpty())
            return buildDir;

        // Otherwise fallback to target working dir
        buildDir = bs->buildTarget(target->activeBuildKey()).workingDirectory;
        if (isQt5CmakeProject(target)) {
            // Return the main build dir and not the android libs dir
            const QString libsDir = QString(Constants::ANDROID_BUILD_DIRECTORY) + "/libs";
            FilePath parentDuildDir = buildDir.parentDir();
            if (parentDuildDir.endsWith(libsDir) || libsDir.endsWith(libsDir + "/"))
                return parentDuildDir.parentDir().parentDir();
        } else {
            // Qt6 + CMake: Very cautios hack to work around QTCREATORBUG-26479 for simple projects
            const QString jsonFileName =
                AndroidQtVersion::androidDeploymentSettingsFileName(target);
            const FilePath jsonFile = buildDir / jsonFileName;
            if (!jsonFile.exists()) {
                const FilePath projectBuildDir = bs->buildConfiguration()->buildDirectory();
                if (buildDir != projectBuildDir) {
                    const FilePath projectJsonFile = projectBuildDir / jsonFileName;
                    if (projectJsonFile.exists())
                        buildDir = projectBuildDir;
                }
            }
        }
        return buildDir;
    }
    return {};
}

enum PackageFormat { Apk, Aab };

static QString packageSubPath(PackageFormat format, BuildConfiguration::BuildType buildType,
                              bool sig)
{
    const bool deb = (buildType == BuildConfiguration::Debug);

    if (format == Apk) {
        if (deb) {
            return sig ? packageSubPath(Apk, BuildConfiguration::Release, true) // Intentional
                       : QLatin1String("apk/debug/android-build-debug.apk");
        }
        return QLatin1String(sig ? "apk/release/android-build-release-signed.apk"
                                 : "apk/release/android-build-release-unsigned.apk");
    }
    return QLatin1String(deb ? "bundle/debug/android-build-debug.aab"
                             : "bundle/release/android-build-release.aab");
}

FilePath packagePath(const Target *target)
{
    QTC_ASSERT(target, return {});

    auto bc = target->activeBuildConfiguration();
    if (!bc)
        return {};
    auto buildApkStep = bc->buildSteps()->firstOfType<AndroidBuildApkStep>();
    if (!buildApkStep)
        return {};

    const QString subPath = packageSubPath(buildApkStep->buildAAB() ? Aab : Apk,
                                           bc->buildType(), buildApkStep->signPackage());

    return androidBuildDirectory(target) / "build/outputs" / subPath;
}

Abi androidAbi2Abi(const QString &androidAbi)
{
    if (androidAbi == ProjectExplorer::Constants::ANDROID_ABI_ARM64_V8A) {
        return Abi{Abi::Architecture::ArmArchitecture,
                   Abi::OS::LinuxOS,
                   Abi::OSFlavor::AndroidLinuxFlavor,
                   Abi::BinaryFormat::ElfFormat,
                   64, androidAbi};
    } else if (androidAbi == ProjectExplorer::Constants::ANDROID_ABI_ARMEABI_V7A) {
        return Abi{Abi::Architecture::ArmArchitecture,
                   Abi::OS::LinuxOS,
                   Abi::OSFlavor::AndroidLinuxFlavor,
                   Abi::BinaryFormat::ElfFormat,
                   32, androidAbi};
    } else if (androidAbi == ProjectExplorer::Constants::ANDROID_ABI_X86_64) {
        return Abi{Abi::Architecture::X86Architecture,
                   Abi::OS::LinuxOS,
                   Abi::OSFlavor::AndroidLinuxFlavor,
                   Abi::BinaryFormat::ElfFormat,
                   64, androidAbi};
    } else if (androidAbi == ProjectExplorer::Constants::ANDROID_ABI_X86) {
        return Abi{Abi::Architecture::X86Architecture,
                   Abi::OS::LinuxOS,
                   Abi::OSFlavor::AndroidLinuxFlavor,
                   Abi::BinaryFormat::ElfFormat,
                   32, androidAbi};
    } else {
        return Abi{Abi::Architecture::UnknownArchitecture,
                   Abi::OS::LinuxOS,
                   Abi::OSFlavor::AndroidLinuxFlavor,
                   Abi::BinaryFormat::ElfFormat,
                   0, androidAbi};
    }
}

bool skipInstallationAndPackageSteps(const Target *target)
{
    // For projects using Qt 5.15 and Qt 6, the deployment settings file
    // is generated by CMake/qmake and not Qt Creator, so if such file doesn't exist
    // or it's been generated by Qt Creator, we can assume the project is not
    // an android app.
    const FilePath inputFile = AndroidQtVersion::androidDeploymentSettings(target);
    if (!inputFile.exists() || isQtCreatorGenerated(inputFile))
        return true;

    const Project *p = target->project();

    const Context cmakeCtx(CMakeProjectManager::Constants::CMAKE_PROJECT_ID);
    const bool isCmakeProject = p->projectContext() == cmakeCtx;
    if (isCmakeProject)
        return false; // CMake reports ProductType::Other for Android Apps

    const ProjectNode *n = p->rootProjectNode()->findProjectNode([] (const ProjectNode *n) {
        return n->productType() == ProductType::App;
    });
    return n == nullptr; // If no Application target found, then skip steps
}

FilePath manifestPath(const Target *target)
{
    QVariant manifest = target->namedSettings(AndroidManifestName);
    if (manifest.isValid())
        return manifest.value<FilePath>();
    return androidBuildDirectory(target).pathAppended(AndroidManifestName);
}

void setManifestPath(Target *target, const FilePath &path)
{
     target->setNamedSettings(AndroidManifestName, QVariant::fromValue(path));
}

QString deviceSerialNumber(const Target *target)
{
    return target->namedSettings(AndroidDeviceSn).toString();
}

void setDeviceSerialNumber(Target *target, const QString &deviceSerialNumber)
{
    qCDebug(androidManagerLog) << "Target device serial changed:"
                               << target->displayName() << deviceSerialNumber;
    target->setNamedSettings(AndroidDeviceSn, deviceSerialNumber);
}

static QString preferredAbi(const QStringList &appAbis, const Target *target)
{
    const auto deviceAbis = target->namedSettings(AndroidDeviceAbis).toStringList();
    for (const auto &abi : deviceAbis) {
        if (appAbis.contains(abi))
            return abi;
    }
    return {};
}

QString apkDevicePreferredAbi(const Target *target)
{
    const FilePath libsPath = androidBuildDirectory(target).pathAppended("libs");
    if (!libsPath.exists()) {
        if (const ProjectNode *node = currentProjectNode(target)) {
            const QString abi = preferredAbi(
                        node->data(Android::Constants::AndroidAbis).toStringList(), target);
            if (abi.isEmpty())
                return node->data(Android::Constants::AndroidAbi).toString();
        }
    }
    QStringList apkAbis;
    const FilePaths libsPaths = libsPath.dirEntries(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const FilePath &abiDir : libsPaths) {
        if (!abiDir.dirEntries({{"*.so"}, QDir::Files | QDir::NoDotAndDotDot}).isEmpty())
            apkAbis << abiDir.fileName();
    }
    return preferredAbi(apkAbis, target);
}

void setDeviceAbis(Target *target, const QStringList &deviceAbis)
{
    target->setNamedSettings(AndroidDeviceAbis, deviceAbis);
}

int deviceApiLevel(const Target *target)
{
    return target->namedSettings(ApiLevelKey).toInt();
}

void setDeviceApiLevel(Target *target, int level)
{
    qCDebug(androidManagerLog) << "Target device API level changed:"
                               << target->displayName() << level;
    target->setNamedSettings(ApiLevelKey, level);
}

int defaultMinimumSDK(const QtSupport::QtVersion *qtVersion)
{
    if (qtVersion && qtVersion->qtVersion() >= QVersionNumber(6, 0))
        return 23;
    else if (qtVersion && qtVersion->qtVersion() >= QVersionNumber(5, 13))
        return 21;
    else
        return 16;
}

QString androidNameForApiLevel(int x)
{
    switch (x) {
    case 2:
        return QLatin1String("Android 1.1");
    case 3:
        return QLatin1String("Android 1.5 (\"Cupcake\")");
    case 4:
        return QLatin1String("Android 1.6 (\"Donut\")");
    case 5:
        return QLatin1String("Android 2.0 (\"Eclair\")");
    case 6:
        return QLatin1String("Android 2.0.1 (\"Eclair\")");
    case 7:
        return QLatin1String("Android 2.1 (\"Eclair\")");
    case 8:
        return QLatin1String("Android 2.2 (\"Froyo\")");
    case 9:
        return QLatin1String("Android 2.3 (\"Gingerbread\")");
    case 10:
        return QLatin1String("Android 2.3.3 (\"Gingerbread\")");
    case 11:
        return QLatin1String("Android 3.0 (\"Honeycomb\")");
    case 12:
        return QLatin1String("Android 3.1 (\"Honeycomb\")");
    case 13:
        return QLatin1String("Android 3.2 (\"Honeycomb\")");
    case 14:
        return QLatin1String("Android 4.0 (\"IceCreamSandwich\")");
    case 15:
        return QLatin1String("Android 4.0.3 (\"IceCreamSandwich\")");
    case 16:
        return QLatin1String("Android 4.1 (\"Jelly Bean\")");
    case 17:
        return QLatin1String("Android 4.2 (\"Jelly Bean\")");
    case 18:
        return QLatin1String("Android 4.3 (\"Jelly Bean\")");
    case 19:
        return QLatin1String("Android 4.4 (\"KitKat\")");
    case 20:
        return QLatin1String("Android 4.4W (\"KitKat Wear\")");
    case 21:
        return QLatin1String("Android 5.0 (\"Lollipop\")");
    case 22:
        return QLatin1String("Android 5.1 (\"Lollipop\")");
    case 23:
        return QLatin1String("Android 6.0 (\"Marshmallow\")");
    case 24:
        return QLatin1String("Android 7.0 (\"Nougat\")");
    case 25:
        return QLatin1String("Android 7.1.1 (\"Nougat\")");
    case 26:
        return QLatin1String("Android 8.0 (\"Oreo\")");
    case 27:
        return QLatin1String("Android 8.1 (\"Oreo\")");
    case 28:
        return QLatin1String("Android 9.0 (\"Pie\")");
    case 29:
        return QLatin1String("Android 10.0 (\"Q\")");
    case 30:
        return QLatin1String("Android 11.0 (\"R\")");
    case 31:
        return QLatin1String("Android 12.0 (\"S\")");
    case 32:
        return QLatin1String("Android 12L (\"Sv2\")");
    case 33:
        return QLatin1String("Android 13.0 (\"Tiramisu\")");
    case 34:
        return QLatin1String("Android 14.0 (\"UpsideDownCake\")");
    default:
        return Tr::tr("Unknown Android version. API Level: %1").arg(x);
    }
}

/**
 * Workaround for '????????????' serial numbers
 * @return ("-d") for buggy devices, ("-s", <serial no>) for normal
 */
QStringList adbSelector(const QString &serialNumber)
{
    if (serialNumber.startsWith(QLatin1String("????")))
        return {"-d"};
    return {"-s", serialNumber};
}

static void startAvdDetached(QPromise<void> &promise, const CommandLine &avdCommand)
{
    qCDebug(androidManagerLog).noquote() << "Running command (startAvdDetached):" << avdCommand.toUserOutput();
    if (!Process::startDetached(avdCommand, {}, DetachedChannelMode::Discard))
        promise.future().cancel();
}

static CommandLine avdCommand(const QString &avdName, bool is32BitUserSpace)
{
    CommandLine cmd(AndroidConfig::emulatorToolPath());
    if (is32BitUserSpace)
        cmd.addArg("-force-32bit");
    cmd.addArgs(AndroidConfig::emulatorArgs(), CommandLine::Raw);
    cmd.addArgs({"-avd", avdName});
    return cmd;
}

static ExecutableItem startAvdAsyncRecipe(const QString &avdName)
{
    const Storage<bool> is32Storage;

    const auto onSetup = [] {
        const FilePath emulatorPath = AndroidConfig::emulatorToolPath();
        if (emulatorPath.exists())
            return SetupResult::Continue;

        QMessageBox::critical(Core::ICore::dialogParent(), Tr::tr("Emulator Tool Is Missing"),
                              Tr::tr("Install the missing emulator tool (%1) to the "
                                     "installed Android SDK.").arg(emulatorPath.displayName()));
        return SetupResult::StopWithError;
    };

    const auto onGetConfSetup = [](Process &process) {
        if (!HostOsInfo::isLinuxHost() || QSysInfo::WordSize != 32)
            return SetupResult::StopWithSuccess; // is64

        process.setCommand({"getconf", {"LONG_BIT"}});
        return SetupResult::Continue;
    };
    const auto onGetConfDone = [is32Storage](const Process &process, DoneWith result) {
        if (result == DoneWith::Success)
            *is32Storage = process.allOutput().trimmed() == "32";
        else
            *is32Storage = true;
        return true;
    };

    const auto onAvdSetup = [avdName, is32Storage](Async<void> &async) {
        async.setConcurrentCallData(startAvdDetached, avdCommand(avdName, *is32Storage));
    };
    const auto onAvdDone = [avdName] {
        QMessageBox::critical(Core::ICore::dialogParent(), Tr::tr("AVD Start Error"),
                              Tr::tr("Failed to start AVD emulator for \"%1\" device.").arg(avdName));
    };

    return Group {
        is32Storage,
        onGroupSetup(onSetup),
        ProcessTask(onGetConfSetup, onGetConfDone),
        AsyncTask<void>(onAvdSetup, onAvdDone, CallDoneIf::Error)
    };
}

static ExecutableItem serialNumberRecipe(const QString &avdName, const Storage<QString> &serialNumberStorage)
{
    const Storage<QStringList> outputStorage;
    const Storage<QString> currentSerialNumberStorage;
    const LoopUntil iterator([outputStorage](int iteration) { return iteration < outputStorage->size(); });

    const auto onSocketSetup = [iterator, outputStorage, currentSerialNumberStorage](TcpSocket &socket) {
        const QString line = outputStorage->at(iterator.iteration());
        if (line.startsWith("* daemon"))
            return SetupResult::StopWithError;

        const QString serialNumber = line.left(line.indexOf('\t')).trimmed();
        if (!serialNumber.startsWith("emulator"))
            return SetupResult::StopWithError;

        const int index = serialNumber.indexOf(QLatin1String("-"));
        if (index == -1)
            return SetupResult::StopWithError;

        bool ok;
        const int port = serialNumber.mid(index + 1).toInt(&ok);
        if (!ok)
            return SetupResult::StopWithError;

        *currentSerialNumberStorage = serialNumber;

        socket.setAddress(QHostAddress(QHostAddress::LocalHost));
        socket.setPort(port);
        socket.setWriteData("avd name\nexit\n");
        return SetupResult::Continue;
    };
    const auto onSocketDone = [avdName, currentSerialNumberStorage, serialNumberStorage](const TcpSocket &socket) {
        const QByteArrayList response = socket.socket()->readAll().split('\n');
        // The input "avd name" might not be echoed as-is, but contain ASCII control sequences.
        for (int i = response.size() - 1; i > 1; --i) {
            if (!response.at(i).startsWith("OK"))
                continue;

            const QString currentAvdName = QString::fromLatin1(response.at(i - 1)).trimmed();
            if (avdName != currentAvdName)
                break;

            *serialNumberStorage = *currentSerialNumberStorage;
            return DoneResult::Success;
        }
        return DoneResult::Error;
    };

    return Group {
        outputStorage,
        AndroidConfig::devicesCommandOutputRecipe(outputStorage),
        For (iterator) >> Do {
            parallel,
            stopOnSuccess,
            Group {
                currentSerialNumberStorage,
                TcpSocketTask(onSocketSetup, onSocketDone)
            }
        }
    };
}

static ExecutableItem isAvdBootedRecipe(const Storage<QString> &serialNumberStorage)
{
    const auto onSetup = [serialNumberStorage](Process &process) {
        const CommandLine cmd{AndroidConfig::adbToolPath(),
                              {adbSelector(*serialNumberStorage),
                               "shell", "getprop", "init.svc.bootanim"}};
        qCDebug(androidManagerLog).noquote() << "Running command (isAvdBooted):" << cmd.toUserOutput();
        process.setCommand(cmd);
    };
    const auto onDone = [](const Process &process, DoneWith result) {
        return result == DoneWith::Success && process.allOutput().trimmed() == "stopped";
    };
    return ProcessTask(onSetup, onDone);
}

static ExecutableItem waitForAvdRecipe(const QString &avdName, const Storage<QString> &serialNumberStorage)
{
    const Storage<QStringList> outputStorage;
    const Storage<bool> stopStorage;

    const auto onIsConnectedDone = [stopStorage, outputStorage, serialNumberStorage] {
        const QString serialNumber = *serialNumberStorage;
        for (const QString &line : *outputStorage) {
            // skip the daemon logs
            if (!line.startsWith("* daemon") && line.left(line.indexOf('\t')).trimmed() == serialNumber)
                return DoneResult::Error;
        }
        serialNumberStorage->clear();
        *stopStorage = true;
        return DoneResult::Success;
    };

    const auto onWaitForBootedDone = [stopStorage] { return !*stopStorage; };

    return Group {
        Forever {
            stopOnSuccess,
            serialNumberRecipe(avdName, serialNumberStorage),
            TimeoutTask([](std::chrono::milliseconds &timeout) { timeout = 100ms; }, DoneResult::Error)
        }.withTimeout(30s),
        Forever {
            stopStorage,
            stopOnSuccess,
            isAvdBootedRecipe(serialNumberStorage),
            TimeoutTask([](std::chrono::milliseconds &timeout) { timeout = 100ms; }, DoneResult::Error),
            Group {
                outputStorage,
                AndroidConfig::devicesCommandOutputRecipe(outputStorage),
                onGroupDone(onIsConnectedDone, CallDoneIf::Success)
            },
            onGroupDone(onWaitForBootedDone)
        }.withTimeout(120s)
    };
}

ExecutableItem startAvdRecipe(const QString &avdName, const Storage<QString> &serialNumberStorage)
{
    return Group {
        If (serialNumberRecipe(avdName, serialNumberStorage) || startAvdAsyncRecipe(avdName)) >> Then {
            waitForAvdRecipe(avdName, serialNumberStorage)
        } >> Else {
            errorItem
        }
    };
}


} // namespace Android::Internal