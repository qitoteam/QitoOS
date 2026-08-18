/*
 * Qira OS - version information
 *
 * QIRA_BUILD_DATE and QIRA_BUILD_ID are injected by the build system; the
 * fallbacks keep the tree compilable when building a single file by hand.
 */
#ifndef QIRA_VERSION_H
#define QIRA_VERSION_H

#define QIRA_VERSION_MAJOR 0
#define QIRA_VERSION_MINOR 3
#define QIRA_VERSION_PATCH 0

#define QIRA_VERSION_STRING "0.3.0"
#define QIRA_CODENAME       "Aurora"

#ifndef QIRA_BUILD_DATE
#define QIRA_BUILD_DATE "unknown"
#endif

#ifndef QIRA_BUILD_ID
#define QIRA_BUILD_ID "dev"
#endif

#define QIRA_PROJECT_NAME "Qira OS"
#define QIRA_PROJECT_URL  "https://github.com/Seigh-sword/QiraOS"
#define QIRA_MAINTAINER   "Seigh-sword"
#define QIRA_CONTACT      "zack.yt.7085@gmail.com"

#endif /* QIRA_VERSION_H */
