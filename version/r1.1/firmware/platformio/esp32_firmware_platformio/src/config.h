/**
 * config.h - Deployment configuration for the MiniSpeedCam firmware.
 *
 * Both values below are build-time overridable: define them in
 * platformio.ini `build_flags` (e.g. -DAPI_BEARER_TOKEN=\"...\") to keep
 * deployment-specific values out of the committed source. The defaults
 * keep a fresh clone building and reproduce the original behaviour.
 */
#pragma once

// Bearer token for the minispeedcam.com Bubble.io workflows.
//
// This is a shared workflow key (not a per-device secret). If you need it
// to stay private, override it via a build flag rather than editing this
// file, and rotate the key on the server side — note it already exists in
// this repository's git history.
#ifndef API_BEARER_TOKEN
#define API_BEARER_TOKEN "bc6d8bd23bbeb6b13fa67448c244a129"
#endif

// PEM-encoded CA root certificate that signs minispeedcam.com.
//
// Leave empty to fall back to WiFiClientSecure::setInsecure() (no
// certificate validation — the original, MITM-vulnerable behaviour). Paste
// the root cert here, or pass it via -DAPI_CA_ROOT_CERT, to enable TLS
// validation on every upload. Fetch it with:
//   openssl s_client -connect minispeedcam.com:443 -showcerts </dev/null
#ifndef API_CA_ROOT_CERT
#define API_CA_ROOT_CERT ""
#endif

static const char* kCaRootCert = API_CA_ROOT_CERT;
