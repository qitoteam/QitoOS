/*
 * QitoOS - version information
 *
 * QITO_BUILD_DATE and QITO_BUILD_ID are injected by the build system; the
 * fallbacks keep the tree compilable when building a single file by hand.
 */
#ifndef QITO_VERSION_H
#define QITO_VERSION_H

#define QITO_VERSION_MAJOR 0
#define QITO_VERSION_MINOR 4
#define QITO_VERSION_PATCH 0

#define QITO_VERSION_STRING "0.4a"
#define QITO_CODENAME       "Alpha"

#ifndef QITO_BUILD_DATE
#define QITO_BUILD_DATE "unknown"
#endif

#ifndef QITO_BUILD_ID
#define QITO_BUILD_ID "dev"
#endif

#define QITO_PROJECT_NAME "QitoOS"
#define QITO_PROJECT_URL  "https://github.com/qitoteam/QitoOS"
#define QITO_MAINTAINER   "Seigh-sword"
#define QITO_CONTACT      "zack.yt.7085@gmail.com"
#define QITO_REGISTRY_URL "https://github.com/qitoteam/qtpkg-registry"

#endif /* QITO_VERSION_H */
